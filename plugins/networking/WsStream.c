//
// WsStream.c - lw-IP/FreeRTOS websocket stream implementation
//
// v1.0 / 2019-11-18 / Io Engineering / Terje
//

/*

Copyright (c) 2019, Terje Io
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

� Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

� Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

� Neither the name of the copyright holder nor the names of its contributors may
be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#define TCP_SLOW_INTERVAL 500

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

//#include "driverlib/debug.h"

#include "networking.h"

//#include "FreeRTOS.h"
//#include "task.h"

#include "serial.h"
#include "driver.h"
#include "WsStream.h"
#include "base64.h"
#include "sha1.h"

#include "GRBL/grbl.h"

//#define WSDEBUG

#define CRLF "\r\n"
#define SOCKET_TIMEOUT 0
#define BUFCOUNT(head, tail, size) ((head >= tail) ? (head - tail) : (size - tail + head))

static const char WS_HEADER[] = "Upgrade: websocket" CRLF;
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const char WS_KEY[] = "Sec-WebSocket-Key: ";
static const char WS_RSP[] = "HTTP/1.1 101 Switching Protocols" CRLF \
                             "Upgrade: websocket" CRLF \
                             "Connection: Upgrade" CRLF \
                             "Sec-WebSocket-Accept: ";
static const char HTTP_400[] = "HTTP/1.1 400" CRLF \
                               "Status: 400 Bad Request" CRLF CRLF;
static const char HTTP_404[] = "HTTP/1.1 404" CRLF \
                               "Status: 404 Not Found" CRLF CRLF;
static const char HTTP_500[] = "HTTP/1.1 500" CRLF \
                               "Status: 500 Internal Server Error" CRLF CRLF;

#define WS_BASE64_LEN   29
#define WS_RSP_LEN      (sizeof(WS_RSP) + sizeof(CRLF CRLF) - 2 + WS_BASE64_LEN)

typedef enum {
    WsOpcode_Continuation = 0x00,
    WsOpcode_Text = 0x1,
    WsOpcode_Binary = 0x2,
    WsOpcode_Close = 0x8,
    WsOpcode_Ping = 0x9,
    WsOpcode_Pong = 0xA
} websocket_opcode_t;

typedef enum
{
    WsState_Idle,
    WsState_Listen,
    WsState_Connected,
    WsStateClosing
} websocket_state_t;

typedef union {
    uint8_t start;
    struct {
        uint8_t opcode :4,
                rsv3   :1,
                rsv2   :1,
                rsv1   :1,
                fin    :1;
    };
} ws_frame_start_t;

typedef struct pbuf_entry
{
    struct pbuf *pbuf;
    struct pbuf_entry *next;
} pbuf_entry_t;

typedef struct ws_sessiondata
{
    uint16_t port;
    websocket_state_t state;
    bool linkLost;
    uint32_t timeout;
    uint32_t timeoutMax;
    struct tcp_pcb *pcbConnect;
    struct tcp_pcb *pcbListen;
    pbuf_entry_t queue[PBUF_POOL_SIZE];
    pbuf_entry_t *rcvTail;
    pbuf_entry_t *rcvHead;
    struct pbuf *pbufHead;
    struct pbuf *pbufCurrent;
    uint32_t bufferIndex;
    stream_rx_buffer_t rxbuf;
    stream_tx_buffer_t txbuf;
    TickType_t lastSendTime;
    err_t lastErr;
    uint8_t errorCount;
    uint8_t reconnectCount;
    uint8_t connectCount;
    char *http_request;
    void (*traffic_handler)(struct ws_sessiondata *session);
} ws_sessiondata_t;

static void WsConnectionHandler (ws_sessiondata_t *session);
static void WsStreamHandler (ws_sessiondata_t *session);

static const ws_sessiondata_t defaultSettings =
{
    .port = 80,
    .state = WsState_Listen,
    .timeout = 0,
    .timeoutMax = SOCKET_TIMEOUT,
    .pcbConnect = NULL,
    .pcbListen = NULL,
    .pbufHead = NULL,
    .pbufCurrent = NULL,
    .bufferIndex = 0,
    .rxbuf = {0},
    .txbuf = {0},
    .lastSendTime = 0,
    .linkLost = false,
    .connectCount = 0,
    .reconnectCount = 0,
    .errorCount = 0,
    .lastErr = ERR_OK,
    .http_request = NULL,
    .traffic_handler = WsConnectionHandler
};

static const ws_frame_start_t wshdr0 = {
  .fin    = true,
  .opcode = WsOpcode_Text
};

static ws_sessiondata_t streamSession;

char *stristr(const char *s1, const char *s2)
{
    const char *s = s1, *p = s2, *r = NULL;

//    size_t n2 = s2 ? strlen(s2) : 0;

    if (!s2 || strlen(s2) == 0)
        return (char *)s1;

    while(*s && *p) {

        if(CAPS(*p) == CAPS(*s)) {
            if(!r)
                r = s;
            p++;
        } else {
            p = s2;
            if(r)
                s = r + 1;
            if(CAPS(*p) == CAPS(*s)) {
                r = s;
                p++;
            } else
                r = NULL;
        }
        s++;
    }

    return *p ? NULL : (char *)r;
}

void WsStreamInit (void)
{
    memcpy(&streamSession, &defaultSettings, sizeof(ws_sessiondata_t));

    // turn the packet queue array into a circular linked list
    uint_fast8_t idx;
    for(idx = 0; idx < PBUF_POOL_SIZE; idx++) {
        streamSession.queue[idx].next = &streamSession.queue[idx == PBUF_POOL_SIZE - 1 ? 0 : idx + 1];
    }

    streamSession.rcvTail = streamSession.rcvHead = &streamSession.queue[0];
}

//
// WsStreamGetC - returns -1 if no data available
//
int16_t WsStreamGetC (void)
{
    int16_t data;
    uint_fast16_t bptr = streamSession.rxbuf.tail;

    if(bptr == streamSession.rxbuf.head)
        return -1; // no data available else EOF

    data = streamSession.rxbuf.data[bptr++];                // Get next character, increment tmp pointer
    streamSession.rxbuf.tail = bptr & (RX_BUFFER_SIZE - 1); // and update pointer

    return data;
}

inline uint16_t WsStreamRxCount (void)
{
    uint_fast16_t head = streamSession.rxbuf.head, tail = streamSession.rxbuf.tail;

    return BUFCOUNT(head, tail, RX_BUFFER_SIZE);
}

uint16_t WsStreamRxFree (void)
{
    return (RX_BUFFER_SIZE - 1) - WsStreamRxCount();
}

void WsStreamRxFlush (void)
{
    streamSession.rxbuf.tail = streamSession.rxbuf.head;
}

void WsStreamRxCancel (void)
{
    streamSession.rxbuf.data[streamSession.rxbuf.head] = ASCII_CAN;
    streamSession.rxbuf.tail = streamSession.rxbuf.head;
    streamSession.rxbuf.head = (streamSession.rxbuf.tail + 1) & (RX_BUFFER_SIZE - 1);
}

static void streamBufferRX (char c) {

    uint_fast16_t bptr = (streamSession.rxbuf.head + 1) & (RX_BUFFER_SIZE - 1); // Get next head pointer

    if(bptr == streamSession.rxbuf.tail) {                          // If buffer full TODO: remove this check?
        streamSession.rxbuf.overflow = 1;                           // flag overlow
    } else {
        if(!hal.stream.enqueue_realtime_command(c)) {
            streamSession.rxbuf.data[streamSession.rxbuf.head] = c; // Add data to buffer
            streamSession.rxbuf.head = bptr;                        // and update pointer
        }
    }
}

bool WsStreamPutC (const char c) {

    uint32_t next_head = (streamSession.txbuf.head + 1) & (TX_BUFFER_SIZE - 1);  // Get and update head pointer

    while(streamSession.txbuf.tail == next_head) {                               // Buffer full, block until space is available...
        if(!hal.stream_blocking_callback())
            return false;
    }

    streamSession.txbuf.data[streamSession.txbuf.head] = c;                     // Add data to buffer
    streamSession.txbuf.head = next_head;                                       // and update head pointer

    return true;
}

void WsStreamWriteS (const char *data)
{
    char c, *ptr = (char *)data;

    while((c = *ptr++) != '\0')
        WsStreamPutC(c);
}

void WsStreamWriteLn (const char *data)
{
    WsStreamWriteS(data);
    WsStreamWriteS(ASCII_EOL);
}

void WsStreamWrite (const char *data, unsigned int length)
{
    char *ptr = (char *)data;

    while(length--)
        WsStreamPutC(*ptr++);
}

uint16_t WsStreamTxCount(void) {

    uint_fast16_t head = streamSession.txbuf.head, tail = streamSession.txbuf.tail;

    return BUFCOUNT(head, tail, TX_BUFFER_SIZE);
}

static int16_t streamReadTXC (void)
{
    int16_t data;
    uint_fast16_t bptr = streamSession.txbuf.tail;

    if(bptr == streamSession.txbuf.head)
        return -1; // no data available else EOF

    data = streamSession.txbuf.data[bptr++];                 // Get next character, increment tmp pointer
    streamSession.txbuf.tail = bptr & (TX_BUFFER_SIZE - 1);  // and update pointer

    return data;
}

void WsStreamTxFlush (void)
{
    streamSession.txbuf.tail = streamSession.txbuf.head;
}

static void streamFreeBuffers (ws_sessiondata_t *session)
{
    SYS_ARCH_DECL_PROTECT(lev);
    SYS_ARCH_PROTECT(lev);

    // Free any buffer chain currently beeing processed
    if(session->pbufHead != NULL) {
        pbuf_free(session->pbufHead);
        session->pbufHead = session->pbufCurrent = NULL;
        session->bufferIndex = 0;
    }

    // Free any queued buffer chains
    while(session->rcvTail != session->rcvHead) {
        pbuf_free(session->rcvTail->pbuf);
        session->rcvTail = session->rcvTail->next;
    }

    // Free any http request currently beeing processed
    if(session->http_request) {
        free(session->http_request);
        session->http_request = NULL;
    }

    SYS_ARCH_UNPROTECT(lev);
}

void WsStreamNotifyLinkStatus (bool up)
{
    if(!up)
        streamSession.linkLost = true;
}

static void streamError (void *arg, err_t err)
{
    ws_sessiondata_t *streamSession = arg;

    streamFreeBuffers(streamSession);

    streamSession->state = WsState_Listen;
    streamSession->errorCount++;
    streamSession->lastErr = err;
    streamSession->pcbConnect = NULL;
    streamSession->timeout = 0;
    streamSession->pbufHead = streamSession->pbufCurrent = NULL;
    streamSession->bufferIndex = 0;
    streamSession->lastSendTime = 0;
    streamSession->linkLost = false;
    streamSession->rcvTail = streamSession->rcvHead;
}

static err_t streamPoll (void *arg, struct tcp_pcb *pcb)
{
    ws_sessiondata_t *streamSession = arg;

    streamSession->timeout++;

    if(streamSession->timeoutMax && streamSession->timeout > streamSession->timeoutMax)
        tcp_abort(pcb);

    return ERR_OK;
}

static void closeSocket (ws_sessiondata_t *session, struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 1);

    tcp_close(pcb);

    streamFreeBuffers(session);

    session->pcbConnect = NULL;
    session->state = WsState_Listen;
    session->traffic_handler = WsConnectionHandler;

    // Switch grbl I/O stream back to UART
    selectStream(StreamType_Serial);
}

//
// Queue incoming packet for processing
//
static err_t streamReceive (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if(err == ERR_OK) {

        ws_sessiondata_t *session = arg;

        if(p) {
            // Attempt to queue data
            SYS_ARCH_DECL_PROTECT(lev);
            SYS_ARCH_PROTECT(lev);

            if(session->rcvHead->next == session->rcvTail) {
                // Queue full, discard
                SYS_ARCH_UNPROTECT(lev);
                pbuf_free(p);
            } else {
                session->rcvHead->pbuf = p;
                session->rcvHead = session->rcvHead->next;
                SYS_ARCH_UNPROTECT(lev);
            }
        } else {
            // Null packet received, means close connection
            closeSocket(session, pcb);
        }
    }

    return ERR_OK;
}

static err_t streamSent (void *arg, struct tcp_pcb *pcb, u16_t ui16len)
{
    ((ws_sessiondata_t *)arg)->timeout = 0;

    return ERR_OK;
}

static err_t WsStreamAccept (void *arg, struct tcp_pcb *pcb, err_t err)
{
    ws_sessiondata_t *session = arg;

    if(session->state != WsState_Listen) {

        if(!session->linkLost)
            return ERR_CONN; // Busy, refuse connection

        // Link was previously lost, abort current connection

        tcp_abort(session->pcbConnect);

        streamFreeBuffers(session);

        session->linkLost = false;
    }

    session->pcbConnect = pcb;
    session->state = WsState_Connected;
    session->traffic_handler = WsConnectionHandler;

    tcp_accepted(pcb);

    WsStreamRxFlush();
    WsStreamTxFlush();

    session->timeout = 0;

    tcp_setprio(pcb, TCP_PRIO_MIN);
    tcp_recv(pcb, streamReceive);
    tcp_err(pcb, streamError);
    tcp_poll(pcb, streamPoll, 1000 / TCP_SLOW_INTERVAL);
    tcp_sent(pcb, streamSent);

    return ERR_OK;
}

void WsStreamClose (void)
{
    if(streamSession.pcbConnect != NULL) {
        tcp_arg(streamSession.pcbConnect, NULL);
        tcp_recv(streamSession.pcbConnect, NULL);
        tcp_sent(streamSession.pcbConnect, NULL);
        tcp_err(streamSession.pcbConnect, NULL);
        tcp_poll(streamSession.pcbConnect, NULL, 1);

        tcp_abort(streamSession.pcbConnect);
        streamFreeBuffers(&streamSession);
    }

    if(streamSession.pcbListen != NULL) {
        tcp_close(streamSession.pcbListen);
        streamFreeBuffers(&streamSession);
    }

    streamSession.state = WsState_Idle;
    streamSession.pcbConnect = streamSession.pcbListen = NULL;
    streamSession.timeout = 0;
    streamSession.rcvTail = streamSession.rcvHead;
    streamSession.pbufHead = streamSession.pbufCurrent = NULL;
    streamSession.bufferIndex = 0;
    streamSession.lastSendTime = 0;
    streamSession.linkLost = false;

    // Switch grbl I/O stream back to UART
    selectStream(StreamType_Serial);
}

void WsStreamListen (uint16_t port)
{
//    ASSERT(port != 0);

    streamSession.state = WsState_Listen;
    streamSession.pcbConnect = NULL;
    streamSession.timeout = 0;
    streamSession.timeoutMax = SOCKET_TIMEOUT;
    streamSession.port = port;
    streamSession.rcvTail = streamSession.rcvHead;
    streamSession.pbufHead = streamSession.pbufCurrent = NULL;
    streamSession.bufferIndex = 0;
    streamSession.lastSendTime = 0;
    streamSession.linkLost = false;

    void *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, port);

    streamSession.pcbListen = tcp_listen(pcb);

    tcp_arg(streamSession.pcbListen, &streamSession);
    tcp_accept(streamSession.pcbListen, WsStreamAccept);
}


/** Call tcp_write() in a loop trying smaller and smaller length
 *
 * @param pcb tcp_pcb to send
 * @param ptr Data to send
 * @param length Length of data to send (in/out: on return, contains the
 *        amount of data sent)
 * @param apiflags directly passed to tcp_write
 * @return the return value of tcp_write
 */
static err_t
http_write(struct tcp_pcb *pcb, const void* ptr, u16_t *length, u8_t apiflags)
{
   u16_t len;
   err_t err;
   LWIP_ASSERT("length != NULL", length != NULL);
   len = *length;
   if (len == 0) {
     return ERR_OK;
   }
   do {
     err = tcp_write(pcb, ptr, len, apiflags);
     if (err == ERR_MEM) {
       if ((tcp_sndbuf(pcb) == 0) ||
           (tcp_sndqueuelen(pcb) >= TCP_SND_QUEUELEN)) {
         /* no need to try smaller sizes */
         len = 1;
       } else {
         len /= 2;
       }
     }
   } while ((err == ERR_MEM) && (len > 1));

   *length = len;
   return err;
}

//
// Process connection handshake
//
static void WsConnectionHandler (ws_sessiondata_t *session)
{
    static uint32_t ptr = 0;

    if(session->http_request == NULL) {
        ptr = 0;
        session->http_request = malloc(512);
        if(session->http_request == NULL) {
            uint16_t len = sizeof(HTTP_500);
            http_write(session->pcbConnect, HTTP_500, &len, 1);
        }
    }

    uint8_t *payload = session->pbufCurrent ? session->pbufCurrent->payload : NULL;

    SYS_ARCH_DECL_PROTECT(lev);

    // 1. Process input
    while(ptr < 512) {

        // Get next pbuf chain to process
        if(session->pbufHead == NULL && session->rcvTail != session->rcvHead) {
            SYS_ARCH_PROTECT(lev);
            session->pbufCurrent = session->pbufHead = session->rcvTail->pbuf;
            session->rcvTail = session->rcvTail->next;
            session->bufferIndex = 0;
            SYS_ARCH_UNPROTECT(lev);
            payload = session->pbufCurrent ? session->pbufCurrent->payload : NULL;
        }

        if(payload == NULL)
            break; // No more data to be processed...

        // Add data to http request header
        session->http_request[ptr++] = payload[session->bufferIndex++];

        if(session->bufferIndex >= session->pbufCurrent->len) {
            session->pbufCurrent = session->pbufCurrent->next;
            session->bufferIndex = 0;
            payload = session->pbufCurrent ? session->pbufCurrent->payload : NULL;
        }

        // ACK current pbuf chain when all data has been processed
        if((session->pbufCurrent == NULL) && (session->bufferIndex == 0)) {
            tcp_recved(session->pcbConnect, session->pbufHead->tot_len);
            pbuf_free(session->pbufHead);
            session->pbufCurrent = session->pbufHead = NULL;
            session->bufferIndex = 0;
        }
    }

    session->http_request[ptr] = '\0';

    bool ok;

    if((ok = strstr(session->http_request, "\r\n\r\n"))) {

#ifdef WSDEBUG
	DEBUG_PRINT(session->http_request);
#endif

        char *keyp, *key_hdr;

        if((key_hdr = stristr(session->http_request, WS_KEY))) {
            keyp = key_hdr + sizeof(WS_KEY) - 1;
            if((key_hdr = strstr(keyp, "\r\n"))) {
                char key[64];
                *key_hdr = '\0';
                uint32_t len = key_hdr - keyp;

                  char *retval_ptr = memcpy(session->http_request, WS_RSP, sizeof(WS_RSP) - 1);
                  retval_ptr += sizeof(WS_RSP) - 1;

                  /* Concatenate key */
                  strcpy(key, keyp);
                  strcat(key, WS_GUID);

                  /* Get SHA1 */
                  int key_len = sizeof(WS_GUID) - 1 + len;
                  BYTE sha1sum[SHA1_BLOCK_SIZE];
                  SHA1_CTX ctx;
                  sha1_init(&ctx);
                  sha1_update(&ctx, (BYTE *)key, strlen(key));
                  sha1_final(&ctx, sha1sum);

                  /* Base64 encode */
                  size_t olen = WS_BASE64_LEN;
                  olen = base64_encode((BYTE *)sha1sum, (BYTE *)retval_ptr, SHA1_BLOCK_SIZE, 0);
                  if ((ok = olen > 0)) {
                      memcpy(&retval_ptr[olen], CRLF CRLF, sizeof(CRLF CRLF));
#ifdef WSDEBUG
	DEBUG_PRINT(session->http_request);
#endif
                      len = WS_RSP_LEN - 1;
                      http_write(session->pcbConnect, session->http_request, (u16_t *)&len, 1);
                      session->traffic_handler = WsStreamHandler;
                      selectStream(StreamType_WebSocket);
                  }
            }
        }

// got a header...
        // Switch grbl I/O stream to TCP/IP connection
//        selectStream(StreamType_Telnet);
//        session->traffic_handler = WsStreamHandler;


        free(session->http_request);
        session->http_request = NULL;
    }

    if(ptr >= 512) {
        uint16_t len = sizeof(HTTP_400);
        http_write(session->pcbConnect, HTTP_400, &len, 1);
        // bad request, just close socket?
    }
}

//
// Process data for streaming
//
static err_t WsParse (ws_sessiondata_t *session, struct pbuf *p)
{
    uint8_t *payload = (uint8_t *)p->payload;
    uint_fast16_t payload_len = p->len, ws_payload_len = 0;

    if (payload && payload_len > 1) {

        ws_frame_start_t fs = (ws_frame_start_t)payload[0];

        if (!fs.fin) {
        //        LWIP_DEBUGF(HTTPD_DEBUG, ("Warning: continuation frames not supported\n"));
            return ERR_OK;
        }

        switch ((websocket_opcode_t)fs.opcode) {

            case WsOpcode_Text:
            case WsOpcode_Binary:
                if (payload_len > 6) {
                    uint_fast16_t ws_payload_offset = 6;
                    uint8_t *dptr = &payload[6], *kptr = &payload[2];
                    ws_payload_len = payload[1] & 0x7F;

                    if (ws_payload_len == 127) {
                      /* most likely won't happen inside non-fragmented frame */
                    //         LWIP_DEBUGF(HTTPD_DEBUG, ("Warning: frame is too long\n"));
                      return ERR_OK;
                    } else if (ws_payload_len == 126) {
                      /* extended length */
                      dptr += 2;
                      kptr += 2;
                      ws_payload_offset += 2;
                      ws_payload_len = (payload[2] << 8) | payload[3];
                    }

                    payload_len -= ws_payload_offset;

                    if (ws_payload_len > payload_len) {
                    //    LWIP_DEBUGF(HTTPD_DEBUG, ("Error: incorrect frame size\n"));
                      return ERR_VAL;
                    }

                    //   if (data_len != len)
                    //    LWIP_DEBUGF(HTTPD_DEBUG, ("Warning: segmented frame received\n"));
                    uint_fast32_t i;
                    /* unmask */
                    for (i = 0; i < ws_payload_len; i++) {
                      *(dptr) ^= kptr[i % 4];
                      streamBufferRX(*dptr++);
                    }
                }
                break;

            case WsOpcode_Close: // close
              tcp_write(session->pcbConnect, payload, payload_len, 1);
              tcp_output(session->pcbConnect);
              session->state = WsStateClosing;
              return ERR_CLSD;

            case WsOpcode_Ping:
//                ((ws_frame_start_t)data[0]).opcode = WsOpcode_Pong;
                tcp_write(session->pcbConnect, payload, payload_len, 1);
                tcp_output(session->pcbConnect);
                break;

            case WsOpcode_Pong:
                break;

            default:
//              LWIP_DEBUGF(HTTPD_DEBUG, ("Unsupported opcode 0x%hX\n", opcode));
              break;
        }
        return ERR_OK;
    }
    return ERR_VAL;
}


static void WsStreamHandler (ws_sessiondata_t *session)
{
    static uint8_t tempBuffer[PBUF_POOL_BUFSIZE];

    uint8_t *payload = session->pbufCurrent ? session->pbufCurrent->payload : NULL;

    SYS_ARCH_DECL_PROTECT(lev);

    // 1. Process input stream
    while(WsStreamRxFree()) {

        // Get next pbuf chain to process
        if(session->pbufHead == NULL && session->rcvTail != session->rcvHead) {
            SYS_ARCH_PROTECT(lev);
            session->pbufCurrent = session->pbufHead = session->rcvTail->pbuf;
            session->rcvTail = session->rcvTail->next;
            session->bufferIndex = 0;
            SYS_ARCH_UNPROTECT(lev);
            payload = session->pbufCurrent ? session->pbufCurrent->payload : NULL;
        }

        if(payload == NULL)
            break; // No more data to be processed...

        WsParse(session, session->pbufCurrent);

        // Add data to input stream buffer
//        streamBufferRX(payload[session->bufferIndex++]);

//        if(session->bufferIndex >= session->pbufCurrent->len) {
            session->pbufCurrent = session->pbufCurrent->next;
            session->bufferIndex = 0;
            payload = session->pbufCurrent ? session->pbufCurrent->payload : NULL;
//        }

        // ACK current pbuf chain when all data has been processed
        if((session->pbufCurrent == NULL) && (session->bufferIndex == 0)) {
            tcp_recved(session->pcbConnect, session->pbufHead->tot_len);
            pbuf_free(session->pbufHead);
            session->pbufCurrent = session->pbufHead = NULL;
            session->bufferIndex = 0;
        }
    }

//    tcp_output(session->pcbConnect);

    uint_fast16_t TXCount;

    // 2. Process output stream
    if((TXCount = WsStreamTxCount()) && tcp_sndbuf(session->pcbConnect) > 4) {

        int16_t c;
        uint_fast16_t idx = 0;

        if(TXCount > tcp_sndbuf(session->pcbConnect) - 4)
            TXCount = tcp_sndbuf(session->pcbConnect) - 4;

        if(TXCount > sizeof(tempBuffer) - 4)
            TXCount = sizeof(tempBuffer) - 4;

        uint_fast16_t plen = TXCount;

        tempBuffer[idx++] = wshdr0.start;
        tempBuffer[idx++] = TXCount < 126 ? TXCount : 126;
        if(TXCount >= 126) {
            tempBuffer[idx++] = (TXCount >> 8) & 0xFF;
            tempBuffer[idx++] = TXCount & 0xFF;
        }

        while(TXCount) {
            if((c = (uint8_t)streamReadTXC()) == -1)
                break;
            tempBuffer[idx++] = (uint8_t)c;
            TXCount--;
        }

#ifdef WSDEBUG
	DEBUG_PRINT(uitoa(tempBuffer[1]));
	DEBUG_PRINT(" - ");
	DEBUG_PRINT(uitoa(idx));
	DEBUG_PRINT(" - ");
	DEBUG_PRINT(uitoa(plen));
	DEBUG_PRINT("\r\n");
#endif

        tcp_write(session->pcbConnect, tempBuffer, (u16_t)idx, 1);
        tcp_output(session->pcbConnect);

        session->lastSendTime = xTaskGetTickCount();
    }
}

//
// Process data for streaming
//
void WsStreamPoll (void)
{
    if(streamSession.state == WsState_Connected)
        streamSession.traffic_handler(&streamSession);
    else if(streamSession.state == WsStateClosing)
        closeSocket(&streamSession, streamSession.pcbConnect);
}
