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
typedef STATUS (*PacketMissingFunc)(UINT64, UINT16, UINT16); // customData, pid, blp
typedef STATUS (*KeyframeRequestFunc)(UINT64);                // customData - called when NACK list overflows

#define UINT16_DEC(a) ((UINT16) ((a) - 1))

// NACK configuration constants
#define NACK_MAX_MISSING_PACKETS      256  // Maximum packets to track for NACK (increased from 64 for burst loss)
#define NACK_REORDER_THRESHOLD        3    // Wait for N packets before sending NACK
#define NACK_TIME_THRESHOLD_MS        20   // Or N ms, whichever comes first
#define NACK_MAX_RETRIES              10   // Max NACKs per packet before giving up (increased from 3)
#define NACK_GIVEUP_THRESHOLD_MS      500  // Stop NACKing after this many ms
#define NACK_MIN_INTERVAL_MS          5    // Rate limit: min ms between NACKs
#define NACK_DEFAULT_RTT_MS           100  // Default RTT estimate when not yet measured
#define NACK_RECOVERED_LIST_SIZE      256  // Size of recovered packets tracking list

// NACK tracking entry for a single missing packet
typedef struct {
    UINT16 seqNum;              // Sequence number of missing packet
    UINT64 firstDetectedTime;   // When gap was first detected (100ns units)
    UINT16 packetsSeenAfter;    // Count of packets received after this gap
    UINT8 nackCount;            // Number of NACKs sent for this packet
    BOOL active;                // Whether this entry is in use
} NackTrackingEntry, *PNackTrackingEntry;

// NACK tracker state
typedef struct {
    NackTrackingEntry entries[NACK_MAX_MISSING_PACKETS];
    UINT16 count;               // Number of active entries
    UINT64 lastNackTime;        // Time of last NACK sent (for rate limiting)
    UINT32 rttMs;               // Current RTT estimate in milliseconds
    UINT16 recoveredList[NACK_RECOVERED_LIST_SIZE]; // List of recently recovered packet sequence numbers
    UINT16 recoveredCount;      // Number of entries in recovered list
    UINT16 recoveredHead;       // Circular buffer head index
} NackTracker, *PNackTracker;

#define JITTER_BUFFER_HASH_TABLE_BUCKET_COUNT  3000
#define JITTER_BUFFER_HASH_TABLE_BUCKET_LENGTH 2

typedef struct {
    FrameReadyFunc onFrameReadyFn;
    FrameDroppedFunc onFrameDroppedFn;
    DepayRtpPayloadFunc depayPayloadFn;

    // used for calculating interarrival jitter https://tools.ietf.org/html/rfc3550#section-6.4.1
    // https://tools.ietf.org/html/rfc3550#appendix-A.8
    // holds the relative transit time for the previous packet
    UINT64 transit;
    // holds estimated jitter, in clockRate units
    DOUBLE jitter;
    UINT16 headSequenceNumber;
    UINT16 tailSequenceNumber;
    UINT32 headTimestamp;
    UINT32 tailTimestamp;
    // this is set to U64 even though rtp timestamps are U32
    // in order to allow calculations to not cause overflow
    UINT64 maxLatency;
    UINT64 customData;
    UINT32 clockRate;
    BOOL started;
    BOOL firstFrameProcessed;
    BOOL sequenceNumberOverflowState;
    BOOL timestampOverFlowState;
    PHashTable pPkgBufferHashTable;

    // NACK support for requesting retransmission of lost packets
    NackTracker nackTracker;
    PacketMissingFunc onPacketMissingFn;
    UINT64 onPacketMissingCustomData;
    KeyframeRequestFunc onKeyframeRequestFn;
    UINT64 onKeyframeRequestCustomData;
} JitterBuffer, *PJitterBuffer;

// constructor
STATUS createJitterBuffer(FrameReadyFunc, FrameDroppedFunc, DepayRtpPayloadFunc, UINT32, UINT32, UINT64, PJitterBuffer*);
// destructor
STATUS freeJitterBuffer(PJitterBuffer*);
STATUS jitterBufferPush(PJitterBuffer, PRtpPacket, PBOOL);
STATUS jitterBufferDropBufferData(PJitterBuffer, UINT16, UINT16, UINT32);
STATUS jitterBufferFillFrameData(PJitterBuffer, PBYTE, UINT32, PUINT32, UINT16, UINT16);

// NACK support
STATUS jitterBufferSetOnPacketMissing(PJitterBuffer, PacketMissingFunc, UINT64);
STATUS jitterBufferSetOnKeyframeRequest(PJitterBuffer, KeyframeRequestFunc, UINT64);
STATUS jitterBufferSetRtt(PJitterBuffer, UINT32);
STATUS jitterBufferMarkRecovered(PJitterBuffer, UINT16);
// Process pending NACKs - can be called from an external timer for periodic processing
STATUS jitterBufferProcessNacks(PJitterBuffer);

#ifdef __cplusplus
}
#endif
#endif //__KINESIS_VIDEO_WEBRTC_CLIENT__JITTERBUFFER_H
