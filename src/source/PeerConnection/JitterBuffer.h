/*******************************************
PeerConnection internal include file
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT__JITTERBUFFER_H
#define __KINESIS_VIDEO_WEBRTC_CLIENT__JITTERBUFFER_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef STATUS (*FrameReadyFunc)(UINT64, UINT16, UINT16, UINT32);
typedef STATUS (*FrameDroppedFunc)(UINT64, UINT16, UINT16, UINT32);
#define UINT16_DEC(a) ((UINT16) ((a) - 1))

// Base jitter buffer struct with vtable for pluggable implementations.
// Concrete implementations embed this as their first member.
typedef struct JitterBuffer {
    // Public fields used by callers (e.g. jitter/transit calculation, RTCP reports)
    UINT64 transit;
    DOUBLE jitter;
    UINT32 clockRate;
    UINT32 tailTimestamp; // latest RTP timestamp in buffer

    // vtable — set by each implementation's create function
    STATUS (*pushFn)(struct JitterBuffer*, PRtpPacket, PBOOL);
    STATUS (*destroyFn)(struct JitterBuffer**);
    STATUS (*fillFrameDataFn)(struct JitterBuffer*, PBYTE, UINT32, PUINT32, UINT16, UINT16);
    STATUS (*fillPartialFrameDataFn)(struct JitterBuffer*, PBYTE, UINT32, PUINT32, UINT16, UINT16);
    STATUS (*getPacketFn)(struct JitterBuffer*, UINT16, PRtpPacket*);
    STATUS (*dropBufferDataFn)(struct JitterBuffer*, UINT16, UINT16, UINT32);
} JitterBuffer, *PJitterBuffer;

// constructor
STATUS createJitterBuffer(FrameReadyFunc, FrameDroppedFunc, DepayRtpPayloadFunc, UINT32, UINT32, UINT64, BOOL, PJitterBuffer*);
// destructor
STATUS freeJitterBuffer(PJitterBuffer*);
// dispatchers
STATUS jitterBufferPush(PJitterBuffer, PRtpPacket, PBOOL);
STATUS jitterBufferDropBufferData(PJitterBuffer, UINT16, UINT16, UINT32);
STATUS jitterBufferFillFrameData(PJitterBuffer, PBYTE, UINT32, PUINT32, UINT16, UINT16);
STATUS jitterBufferFillPartialFrameData(PJitterBuffer, PBYTE, UINT32, PUINT32, UINT16, UINT16);
STATUS jitterBufferGetPacket(PJitterBuffer, UINT16, PRtpPacket*);

#ifdef __cplusplus
}
#endif
#endif //__KINESIS_VIDEO_WEBRTC_CLIENT__JITTERBUFFER_H
