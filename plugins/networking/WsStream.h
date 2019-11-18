//
// WsStream.h - lw-IP/FreeRTOS websocket stream implementation
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

#ifndef __WSSTREAM_H__
#define __WSSTREAM_H__

void WsStreamInit(void);
void WsStreamListen(uint16_t port);
void WsStreamClose(void);
void WsStreamPoll(void);
void WsStreamNotifyLinkStatus(bool bLinkStatusUp);
int16_t WsStreamGetC(void);
bool WsStreamPutC(const char data);
void WsStreamWriteS(const char *data);
void WsStreamWriteLn(const char *data);
void WsStreamWrite(const char *data, unsigned int length);
uint16_t WsStreamTxCount(void);
uint16_t WsStreamRxCount(void);
uint16_t WsStreamRxFree(void);
void WsStreamRxFlush(void);
void WsStreamRxCancel(void);

#endif
