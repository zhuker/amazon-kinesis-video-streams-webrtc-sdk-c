#define LOG_CLASS "RealTimeJitterBuffer"

#include "../Include_i.h"

#define RT_MAX_FRAMES             2048
#define RT_PKT_RING_SIZE          4096
#define RT_PROCESSED_TS_RING_SIZE 512

typedef struct {
    UINT32 timestamp;
    UINT16 firstSeqNum;
    UINT16 lastSeqNum;
    UINT16 packetCount;
    BOOL hasStart;
    BOOL hasEnd;
} RtFrameEntry, *PRtFrameEntry;

typedef struct {
    JitterBuffer base; // MUST be first for vtable cast
    FrameReadyFunc onFrameReadyFn;
    FrameDroppedFunc onFrameDroppedFn;
    DepayRtpPayloadFunc depayPayloadFn;
    UINT64 customData;
    UINT64 maxLatency; // in RTP timestamp units
    BOOL started;
    BOOL sequenceNumberOverflowState;
    BOOL timestampOverFlowState;
    BOOL alwaysSinglePacketFrames;
    UINT16 headSequenceNumber;
    UINT16 tailSequenceNumber;
    UINT32 headTimestamp;
    BOOL hasDelivered; // whether any frame has been delivered
    UINT16 nextExpectedSeqNum; // first seq of next frame, inferred from previous frame's marker bit
    BOOL nextExpectedSeqValid; // whether nextExpectedSeqNum is set
    PRtpPacket pktRing[RT_PKT_RING_SIZE];
    UINT32 processedTimestamps[RT_PROCESSED_TS_RING_SIZE]; // ring buffer of delivered/dropped timestamps
    UINT32 processedTsHead;                                // next write index
    UINT32 processedTsCount;                               // number of valid entries
    RtFrameEntry frames[RT_MAX_FRAMES];
    UINT32 frameCount;
} RealTimeJitterBufferInternal, *PRealTimeJitterBufferInternal;

// Forward declarations
static STATUS rtPush(PJitterBuffer, PRtpPacket, PBOOL);
static STATUS rtDestroy(PJitterBuffer*);
static STATUS rtFillFrameData(PJitterBuffer, PBYTE, UINT32, PUINT32, UINT16, UINT16);
static STATUS rtFillPartialFrameData(PJitterBuffer, PBYTE, UINT32, PUINT32, UINT16, UINT16);
static STATUS rtGetPacket(PJitterBuffer, UINT16, PRtpPacket*);
static STATUS rtDropBufferData(PJitterBuffer, UINT16, UINT16, UINT32);

// Record a timestamp as processed (delivered or dropped)
static VOID rtMarkTimestampProcessed(PRealTimeJitterBufferInternal pInternal, UINT32 timestamp)
{
    pInternal->processedTimestamps[pInternal->processedTsHead] = timestamp;
    pInternal->processedTsHead = (pInternal->processedTsHead + 1) % RT_PROCESSED_TS_RING_SIZE;
    if (pInternal->processedTsCount < RT_PROCESSED_TS_RING_SIZE) {
        pInternal->processedTsCount++;
    }
}

// Check if a timestamp was recently processed
static BOOL rtIsTimestampProcessed(PRealTimeJitterBufferInternal pInternal, UINT32 timestamp)
{
    UINT32 i;
    for (i = 0; i < pInternal->processedTsCount; i++) {
        if (pInternal->processedTimestamps[i] == timestamp) {
            return TRUE;
        }
    }
    return FALSE;
}

// Find frame entry by timestamp, returns index or frameCount if not found
static UINT32 rtFindFrame(PRealTimeJitterBufferInternal pInternal, UINT32 timestamp)
{
    UINT32 i;
    for (i = 0; i < pInternal->frameCount; i++) {
        if (pInternal->frames[i].timestamp == timestamp) {
            return i;
        }
    }
    return pInternal->frameCount;
}

// Remove frame entry at index (swap with last)
static VOID rtRemoveFrame(PRealTimeJitterBufferInternal pInternal, UINT32 index)
{
    if (index < pInternal->frameCount - 1) {
        pInternal->frames[index] = pInternal->frames[pInternal->frameCount - 1];
    }
    pInternal->frameCount--;
}

// Compute the age of a timestamp relative to tailTimestamp, handling wraparound
static UINT32 rtTimestampAge(PRealTimeJitterBufferInternal pInternal, UINT32 timestamp)
{
    if (pInternal->timestampOverFlowState) {
        // Overflow: tail has wrapped, head hasn't yet
        BOOL tsWrapped = (timestamp < pInternal->headTimestamp);
        BOOL tailWrapped = (pInternal->base.tailTimestamp < pInternal->headTimestamp);
        if (tsWrapped == tailWrapped) {
            // Both on the same side
            if (pInternal->base.tailTimestamp >= timestamp) {
                return pInternal->base.tailTimestamp - timestamp;
            }
            return 0;
        } else {
            // timestamp is on the old side (before wrap), tail is on new side
            return (MAX_RTP_TIMESTAMP - timestamp) + pInternal->base.tailTimestamp + 1;
        }
    } else {
        if (pInternal->base.tailTimestamp >= timestamp) {
            return pInternal->base.tailTimestamp - timestamp;
        }
        // timestamp > tail shouldn't happen in normal flow, treat as 0 age
        return 0;
    }
}

// Compare two timestamps for ordering. Returns negative if a < b, 0 if equal, positive if a > b.
// Takes overflow state into account.
static INT32 rtTimestampCompare(PRealTimeJitterBufferInternal pInternal, UINT32 a, UINT32 b)
{
    if (a == b) {
        return 0;
    }
    if (pInternal->timestampOverFlowState) {
        // In overflow state, high timestamps are "older" (before wrap) and low are "newer" (after wrap).
        // A value is on the wrapped (new) side if it's less than headTimestamp.
        // headTimestamp is the earliest frame's timestamp, which is on the pre-wrap (old) side.
        BOOL aWrapped = (a < pInternal->headTimestamp);
        BOOL bWrapped = (b < pInternal->headTimestamp);
        if (aWrapped && !bWrapped) {
            return 1; // a is after wrap (newer)
        }
        if (!aWrapped && bWrapped) {
            return -1; // a is before wrap (older)
        }
    }
    // Same side of wrap, or no overflow
    if (a < b) {
        return -1;
    }
    return 1;
}

// Free all packets in the ring buffer for a given seq range [first, last] inclusive
static VOID rtFreePacketsInRange(PRealTimeJitterBufferInternal pInternal, UINT16 firstSeq, UINT16 lastSeq)
{
    UINT16 seq;
    PRtpPacket pPacket;

    for (seq = firstSeq;; seq++) {
        pPacket = pInternal->pktRing[seq % RT_PKT_RING_SIZE];
        if (pPacket != NULL && pPacket->header.sequenceNumber == seq) {
            freeRtpPacket(&pPacket);
            pInternal->pktRing[seq % RT_PKT_RING_SIZE] = NULL;
        }
        if (seq == lastSeq) {
            break;
        }
    }
}

// Calculate frame size by iterating packets and calling depay with NULL output
static STATUS rtCalcFrameSize(PRealTimeJitterBufferInternal pInternal, UINT16 firstSeq, UINT16 lastSeq, PUINT32 pFrameSize)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 seq;
    PRtpPacket pPacket;
    UINT32 totalSize = 0;
    UINT32 partialSize;
    BOOL isFirst = TRUE;

    for (seq = firstSeq;; seq++) {
        pPacket = pInternal->pktRing[seq % RT_PKT_RING_SIZE];
        if (pPacket != NULL && pPacket->header.sequenceNumber == seq) {
            partialSize = 0;
            BOOL depayIsFirst = isFirst;
            pInternal->depayPayloadFn(pPacket->payload, pPacket->payloadLength, NULL, &partialSize, &depayIsFirst);
            totalSize += partialSize;
            isFirst = FALSE;
        }
        if (seq == lastSeq) {
            break;
        }
    }

    *pFrameSize = totalSize;
    return retStatus;
}

// Check if a frame is complete
static BOOL rtFrameIsComplete(PRealTimeJitterBufferInternal pInternal, PRtFrameEntry pFrame)
{
    if (pInternal->alwaysSinglePacketFrames) {
        return pFrame->hasEnd;
    }
    if (!pFrame->hasStart || !pFrame->hasEnd) {
        return FALSE;
    }
    UINT16 expectedCount = (UINT16) (pFrame->lastSeqNum - pFrame->firstSeqNum + 1);
    return pFrame->packetCount == expectedCount;
}

// Find the index of the frame with the earliest timestamp. Caller must ensure frameCount > 0.
static UINT32 rtFindEarliestFrameIdx(PRealTimeJitterBufferInternal pInternal)
{
    UINT32 earliestIdx = 0;
    UINT32 i;
    for (i = 1; i < pInternal->frameCount; i++) {
        if (rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, pInternal->frames[earliestIdx].timestamp) < 0) {
            earliestIdx = i;
        }
    }
    return earliestIdx;
}

// Find the firstSeqNum of the next frame in timestamp order after currentTimestamp.
// Returns TRUE if found, storing the fence sequence number in *pFenceSeq.
static BOOL rtFindNextFrameFence(PRealTimeJitterBufferInternal pInternal, UINT32 currentTimestamp, PUINT16 pFenceSeq)
{
    UINT32 i;
    BOOL found = FALSE;
    UINT32 bestTs = 0;
    for (i = 0; i < pInternal->frameCount; i++) {
        if (pInternal->frames[i].timestamp != currentTimestamp &&
            rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, currentTimestamp) > 0) {
            if (!found || rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, bestTs) < 0) {
                *pFenceSeq = pInternal->frames[i].firstSeqNum;
                bestTs = pInternal->frames[i].timestamp;
                found = TRUE;
            }
        }
    }
    return found;
}

// Scan from startSeq to fenceSeq (exclusive), counting contiguous packets with expectedTimestamp.
// Sets *pPresentCount and *pGapFound.
static VOID rtFenceScan(PRealTimeJitterBufferInternal pInternal, UINT16 startSeq, UINT16 fenceSeq, UINT32 expectedTimestamp, PUINT16 pPresentCount,
                        PBOOL pGapFound)
{
    UINT16 seq;
    UINT16 present = 0;
    BOOL gapFound = FALSE;
    PRtpPacket pPkt = NULL;

    for (seq = startSeq; seq != fenceSeq; seq++) {
        pPkt = pInternal->pktRing[seq % RT_PKT_RING_SIZE];
        if (pPkt == NULL || pPkt->header.sequenceNumber != seq) {
            gapFound = TRUE;
            break;
        }
        if (pPkt->header.timestamp != expectedTimestamp) {
            break;
        }
        present++;
    }

    *pPresentCount = present;
    *pGapFound = gapFound;
}

// Try to mark a frame as complete using fence-scan. Returns TRUE if marked complete.
static BOOL rtTryMarkFrameComplete(PRealTimeJitterBufferInternal pInternal, PRtFrameEntry pFrame, UINT16 fenceSeq)
{
    UINT16 slotsInRange = (UINT16) (fenceSeq - pFrame->firstSeqNum);
    UINT16 present = 0;
    BOOL gapFound = FALSE;

    if (slotsInRange == 0 || slotsInRange > 1000) {
        return FALSE;
    }

    rtFenceScan(pInternal, pFrame->firstSeqNum, fenceSeq, pFrame->timestamp, &present, &gapFound);

    if (!gapFound && present == pFrame->packetCount) {
        pFrame->hasEnd = TRUE;
        pFrame->lastSeqNum = (UINT16) (fenceSeq - 1);
        DLOGD("rtFence: COMPLETE ts=%u seq=[%u..%u] fence=%u present=%u",
              pFrame->timestamp, pFrame->firstSeqNum, pFrame->lastSeqNum, fenceSeq, present);
        return TRUE;
    }
    DLOGD("rtFence: INCOMPLETE ts=%u firstSeq=%u fence=%u present=%u pktCount=%u gapFound=%d",
          pFrame->timestamp, pFrame->firstSeqNum, fenceSeq, present, pFrame->packetCount, gapFound);
    return FALSE;
}

// Update headTimestamp and headSequenceNumber to the earliest frame in the buffer
static VOID rtUpdateHead(PRealTimeJitterBufferInternal pInternal)
{
    if (pInternal->frameCount == 0) {
        // No frames in buffer: reset head to tail to prevent stale head values
        // from triggering false overflow detection when new packets arrive.
        pInternal->headTimestamp = pInternal->base.tailTimestamp;
        pInternal->headSequenceNumber = pInternal->tailSequenceNumber;
        pInternal->timestampOverFlowState = FALSE;
        pInternal->sequenceNumberOverflowState = FALSE;
        return;
    }

    UINT32 earliestIdx = rtFindEarliestFrameIdx(pInternal);
    pInternal->headTimestamp = pInternal->frames[earliestIdx].timestamp;
    pInternal->headSequenceNumber = pInternal->frames[earliestIdx].firstSeqNum;

    // Check if we can exit overflow states
    if (pInternal->timestampOverFlowState) {
        if (pInternal->headTimestamp <= pInternal->base.tailTimestamp) {
            pInternal->timestampOverFlowState = FALSE;
        }
    }
    if (pInternal->sequenceNumberOverflowState) {
        if (pInternal->headSequenceNumber <= pInternal->tailSequenceNumber) {
            pInternal->sequenceNumberOverflowState = FALSE;
        }
    }
}

//
// Constructor
//

STATUS createRealTimeJitterBuffer(FrameReadyFunc onFrameReadyFunc, FrameDroppedFunc onFrameDroppedFunc, DepayRtpPayloadFunc depayRtpPayloadFunc,
                                  UINT32 maxLatency, UINT32 clockRate, UINT64 customData, BOOL alwaysSinglePacketFrames,
                                  PJitterBuffer* ppJitterBuffer)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PRealTimeJitterBufferInternal pInternal = NULL;

    CHK(ppJitterBuffer != NULL && onFrameReadyFunc != NULL && onFrameDroppedFunc != NULL && depayRtpPayloadFunc != NULL, STATUS_NULL_ARG);
    CHK(clockRate != 0, STATUS_INVALID_ARG);

    pInternal = (PRealTimeJitterBufferInternal) MEMCALLOC(1, SIZEOF(RealTimeJitterBufferInternal));
    CHK(pInternal != NULL, STATUS_NOT_ENOUGH_MEMORY);

    // Wire vtable
    pInternal->base.pushFn = rtPush;
    pInternal->base.destroyFn = rtDestroy;
    pInternal->base.fillFrameDataFn = rtFillFrameData;
    pInternal->base.fillPartialFrameDataFn = rtFillPartialFrameData;
    pInternal->base.getPacketFn = rtGetPacket;
    pInternal->base.dropBufferDataFn = rtDropBufferData;

    // Public fields
    pInternal->base.clockRate = clockRate;
    pInternal->base.transit = 0;
    pInternal->base.jitter = 0;
    pInternal->base.tailTimestamp = 0;

    // Private fields
    pInternal->onFrameReadyFn = onFrameReadyFunc;
    pInternal->onFrameDroppedFn = onFrameDroppedFunc;
    pInternal->depayPayloadFn = depayRtpPayloadFunc;
    pInternal->customData = customData;

    pInternal->maxLatency = maxLatency;
    if (pInternal->maxLatency == 0) {
        pInternal->maxLatency = DEFAULT_JITTER_BUFFER_MAX_LATENCY;
    }
    pInternal->maxLatency = pInternal->maxLatency * pInternal->base.clockRate / HUNDREDS_OF_NANOS_IN_A_SECOND;
    CHK(pInternal->maxLatency < MAX_RTP_TIMESTAMP, STATUS_INVALID_ARG);

    pInternal->headTimestamp = MAX_UINT32;
    pInternal->hasDelivered = FALSE;
    pInternal->headSequenceNumber = MAX_RTP_SEQUENCE_NUM;
    pInternal->tailSequenceNumber = MAX_RTP_SEQUENCE_NUM;
    pInternal->started = FALSE;
    pInternal->timestampOverFlowState = FALSE;
    pInternal->sequenceNumberOverflowState = FALSE;
    pInternal->alwaysSinglePacketFrames = alwaysSinglePacketFrames;
    pInternal->frameCount = 0;
    pInternal->processedTsHead = 0;
    pInternal->processedTsCount = 0;
    pInternal->nextExpectedSeqNum = 0;
    pInternal->nextExpectedSeqValid = FALSE;

CleanUp:
    if (STATUS_FAILED(retStatus) && pInternal != NULL) {
        SAFE_MEMFREE(pInternal);
    }

    if (ppJitterBuffer != NULL) {
        *ppJitterBuffer = (PJitterBuffer) pInternal;
    }

    LEAVES();
    return retStatus;
}

//
// Push implementation
//

static STATUS rtPush(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket, PBOOL pPacketDiscarded)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PRealTimeJitterBufferInternal pInternal = (PRealTimeJitterBufferInternal) pJitterBuffer;
    UINT32 frameIdx;
    PRtFrameEntry pFrame;
    PRtpPacket pExisting;
    UINT32 partialSize;
    BOOL isStart;
    UINT32 age;
    UINT32 i;
    UINT32 frameSize;

    CHK(pInternal != NULL && pRtpPacket != NULL, STATUS_NULL_ARG);

    // Bootstrap on first packet
    if (!pInternal->started) {
        pInternal->started = TRUE;
        pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        pInternal->headTimestamp = pRtpPacket->header.timestamp;
        pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
    }

    // Sequence number overflow/underflow tracking
    if (!pInternal->sequenceNumberOverflowState) {
        UINT16 packetsUntilOverflow = MAX_RTP_SEQUENCE_NUM - pInternal->tailSequenceNumber;
        if (packetsUntilOverflow <= 512 && pRtpPacket->header.sequenceNumber < pInternal->tailSequenceNumber &&
            pRtpPacket->header.sequenceNumber <= 512 - packetsUntilOverflow) {
            // Overflow: tail wraps to small values
            pInternal->sequenceNumberOverflowState = TRUE;
            pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        } else if (pInternal->headSequenceNumber < 512 && pRtpPacket->header.sequenceNumber >= (MAX_UINT16 - 512) &&
                   pRtpPacket->header.sequenceNumber > pInternal->tailSequenceNumber) {
            // Underflow: new packet has a large seq (near MAX) but is actually older.
            // Head is near 0, new packet is near MAX — head moves to the large value.
            pInternal->sequenceNumberOverflowState = TRUE;
            pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        }
    }

    // Update tail sequence number
    if (pInternal->sequenceNumberOverflowState) {
        // In overflow state, tail is on the wrapped side (small values near 0),
        // head is on the pre-wrap side (large values near MAX).
        // Update tail if the new seq is ahead of current tail (on same side or further wrapped).
        UINT16 distFromTail = (UINT16) (pRtpPacket->header.sequenceNumber - pInternal->tailSequenceNumber);
        if (distFromTail > 0 && distFromTail < 512) {
            pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        }
    } else {
        if (pRtpPacket->header.sequenceNumber > pInternal->tailSequenceNumber) {
            pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        }
    }

    // Timestamp overflow/underflow tracking
    if (!pInternal->timestampOverFlowState) {
        if (pInternal->base.tailTimestamp > pRtpPacket->header.timestamp && pRtpPacket->header.timestamp < pInternal->headTimestamp) {
            // Overflow: new packet timestamp is much smaller than both head and tail.
            // Tail has wrapped to small values.
            if (pRtpPacket->header.sequenceNumber == pInternal->tailSequenceNumber) {
                pInternal->timestampOverFlowState = TRUE;
                pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
            }
        } else if (pRtpPacket->header.timestamp > pInternal->headTimestamp && pRtpPacket->header.timestamp > pInternal->base.tailTimestamp) {
            // Underflow: new packet timestamp is much larger than both head and tail.
            // This is an older packet from before the wrap (head moves to large value).
            UINT16 distFromHead = (UINT16) (pInternal->headSequenceNumber - pRtpPacket->header.sequenceNumber);
            if (distFromHead > 0 && distFromHead < 512) {
                pInternal->timestampOverFlowState = TRUE;
                pInternal->headTimestamp = pRtpPacket->header.timestamp;
                pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
            }
        }
    }

    // Update tail timestamp
    if (pInternal->timestampOverFlowState) {
        // In overflow state, tail is on the wrapped side (small values).
        // Update if the new timestamp is on the wrapped side and ahead of tail.
        // "On the wrapped side" means <= tail or slightly ahead of tail.
        // "Pre-wrap side" means close to headTimestamp (large values).
        // Use rtTimestampCompare: if new ts is newer than tail, update.
        if (rtTimestampCompare(pInternal, pRtpPacket->header.timestamp, pInternal->base.tailTimestamp) > 0) {
            pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
        }
    } else {
        if (pRtpPacket->header.timestamp > pInternal->base.tailTimestamp) {
            pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
        }
    }

    // Latency tolerance check - discard if too old
    age = rtTimestampAge(pInternal, pRtpPacket->header.timestamp);
    if (age > pInternal->maxLatency) {
        DLOGD("rtPush: DISCARD-TOO-OLD seq=%u ts=%u age=%u maxLatency=%llu tailTs=%u",
              pRtpPacket->header.sequenceNumber, pRtpPacket->header.timestamp,
              age, pInternal->maxLatency, pInternal->base.tailTimestamp);
        freeRtpPacket(&pRtpPacket);
        if (pPacketDiscarded != NULL) {
            *pPacketDiscarded = TRUE;
        }
        // Still run eviction/delivery before returning
        goto EvictAndDeliver;
    }

    // Check for duplicate - if exists, replace but don't increment packetCount
    pExisting = pInternal->pktRing[pRtpPacket->header.sequenceNumber % RT_PKT_RING_SIZE];
    if (pExisting != NULL && pExisting->header.sequenceNumber == pRtpPacket->header.sequenceNumber) {
        DLOGD("rtPush: DUPLICATE seq=%u ts=%u", pRtpPacket->header.sequenceNumber, pRtpPacket->header.timestamp);
        freeRtpPacket(&pExisting);
        pInternal->pktRing[pRtpPacket->header.sequenceNumber % RT_PKT_RING_SIZE] = pRtpPacket;
        // Don't update frame entry for duplicates
        goto EvictAndDeliver;
    }

    // Find or create frame entry
    frameIdx = rtFindFrame(pInternal, pRtpPacket->header.timestamp);
    if (frameIdx == pInternal->frameCount) {
        // No existing frame entry — check if this timestamp was already delivered/dropped
        BOOL alreadyProcessed = rtIsTimestampProcessed(pInternal, pRtpPacket->header.timestamp);
        if (alreadyProcessed) {
            DLOGD("rtPush: DISCARD-LATE seq=%u ts=%u (timestamp already delivered/dropped)",
                  pRtpPacket->header.sequenceNumber, pRtpPacket->header.timestamp);
            freeRtpPacket(&pRtpPacket);
            if (pPacketDiscarded != NULL) {
                *pPacketDiscarded = TRUE;
            }
            goto EvictAndDeliver;
        }
    }

    // Store packet in ring buffer
    pInternal->pktRing[pRtpPacket->header.sequenceNumber % RT_PKT_RING_SIZE] = pRtpPacket;

    if (frameIdx == pInternal->frameCount) {
        // Create new frame entry
        CHK(pInternal->frameCount < RT_MAX_FRAMES, STATUS_NOT_ENOUGH_MEMORY);
        frameIdx = pInternal->frameCount;
        pInternal->frameCount++;
        pFrame = &pInternal->frames[frameIdx];
        pFrame->timestamp = pRtpPacket->header.timestamp;
        pFrame->lastSeqNum = pRtpPacket->header.sequenceNumber;
        pFrame->packetCount = 0;
        pFrame->hasStart = FALSE;
        pFrame->hasEnd = FALSE;

        // If the previous frame ended with a marker bit we know exactly where
        // this frame must start. Use that as firstSeqNum so any gap between
        // nextExpectedSeqNum and the first arrived packet makes the frame appear
        // incomplete and prevents premature delivery.
        if (pInternal->nextExpectedSeqValid) {
            UINT16 dist = (UINT16) (pRtpPacket->header.sequenceNumber - pInternal->nextExpectedSeqNum);
            if (dist > 0 && dist < 512) {
                // Packet arrived after the expected start — gap exists at the front.
                DLOGD("rtPush: NEW-FRAME ts=%u firstSeq=%u (expected %u, gap of %u at front)",
                      pRtpPacket->header.timestamp, pInternal->nextExpectedSeqNum,
                      pInternal->nextExpectedSeqNum, (UINT32) dist);
                pFrame->firstSeqNum = pInternal->nextExpectedSeqNum;
            } else {
                pFrame->firstSeqNum = pRtpPacket->header.sequenceNumber;
                DLOGD("rtPush: NEW-FRAME ts=%u firstSeq=%u frameCount=%u",
                      pRtpPacket->header.timestamp, pRtpPacket->header.sequenceNumber, pInternal->frameCount);
            }
            pInternal->nextExpectedSeqValid = FALSE;
        } else {
            pFrame->firstSeqNum = pRtpPacket->header.sequenceNumber;
            DLOGD("rtPush: NEW-FRAME ts=%u firstSeq=%u frameCount=%u",
                  pRtpPacket->header.timestamp, pRtpPacket->header.sequenceNumber, pInternal->frameCount);
        }

        // Update head if this is earlier
        if (rtTimestampCompare(pInternal, pRtpPacket->header.timestamp, pInternal->headTimestamp) < 0) {
            pInternal->headTimestamp = pRtpPacket->header.timestamp;
            pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        }

        // A new timestamp means the sender moved on. For older frames without
        // a marker bit, use the new packet's sequence number as a fence to check completion.
        for (i = 0; i < pInternal->frameCount; i++) {
            PRtFrameEntry pOlder = &pInternal->frames[i];
            if (pOlder->timestamp != pRtpPacket->header.timestamp && !pOlder->hasEnd &&
                rtTimestampCompare(pInternal, pOlder->timestamp, pRtpPacket->header.timestamp) < 0) {
                rtTryMarkFrameComplete(pInternal, pOlder, pRtpPacket->header.sequenceNumber);
            }
        }
    }

    pFrame = &pInternal->frames[frameIdx];
    pFrame->packetCount++;

    // Update seq range
    // Use UINT16 arithmetic for wraparound-safe comparison
    if ((UINT16) (pRtpPacket->header.sequenceNumber - pFrame->firstSeqNum) > (UINT16) (pFrame->lastSeqNum - pFrame->firstSeqNum)) {
        // Check if it's before firstSeqNum or after lastSeqNum
        UINT16 distFromFirst = (UINT16) (pFrame->firstSeqNum - pRtpPacket->header.sequenceNumber);
        UINT16 distFromLast = (UINT16) (pRtpPacket->header.sequenceNumber - pFrame->lastSeqNum);
        if (distFromFirst < distFromLast) {
            pFrame->firstSeqNum = pRtpPacket->header.sequenceNumber;
        } else {
            pFrame->lastSeqNum = pRtpPacket->header.sequenceNumber;
        }
    }

    // Check start/end markers via depay
    partialSize = 0;
    isStart = TRUE;
    pInternal->depayPayloadFn(pRtpPacket->payload, pRtpPacket->payloadLength, NULL, &partialSize, &isStart);
    if (isStart) {
        pFrame->hasStart = TRUE;
    }
    if (pRtpPacket->header.marker) {
        pFrame->hasEnd = TRUE;
        DLOGD("rtPush: MARKER seq=%u ts=%u → frame hasEnd frame=[%u..%u] pktCount=%u",
              pRtpPacket->header.sequenceNumber, pRtpPacket->header.timestamp,
              pFrame->firstSeqNum, pFrame->lastSeqNum, pFrame->packetCount);
    }
    DLOGD("rtPush: PKT seq=%u ts=%u → frame=[%u..%u] pktCount=%u hasStart=%d hasEnd=%d",
          pRtpPacket->header.sequenceNumber, pRtpPacket->header.timestamp,
          pFrame->firstSeqNum, pFrame->lastSeqNum, pFrame->packetCount,
          pFrame->hasStart, pFrame->hasEnd);

    // Re-evaluate frame completion after adding a late packet to an existing frame.
    if (!pFrame->hasEnd && pFrame->hasStart && pInternal->frameCount > 1) {
        UINT16 fence = 0;
        if (rtFindNextFrameFence(pInternal, pFrame->timestamp, &fence)) {
            rtTryMarkFrameComplete(pInternal, pFrame, fence);
        }
    }

    // A late packet may have shifted this frame's firstSeqNum, changing the fence
    // for older frames. Re-scan older frames using this frame's firstSeqNum as fence.
    if (pInternal->frameCount > 1) {
        UINT16 myFirstSeq = pFrame->firstSeqNum;
        UINT32 myTimestamp = pFrame->timestamp;
        for (i = 0; i < pInternal->frameCount; i++) {
            PRtFrameEntry pOlder = &pInternal->frames[i];
            if (pOlder->timestamp == myTimestamp || pOlder->hasEnd) {
                continue;
            }
            if (rtTimestampCompare(pInternal, pOlder->timestamp, myTimestamp) >= 0) {
                continue; // not older
            }
            // Check if this frame is the immediate next after pOlder
            UINT16 nextFence = 0;
            if (!rtFindNextFrameFence(pInternal, pOlder->timestamp, &nextFence) || nextFence != myFirstSeq) {
                continue;
            }
            rtTryMarkFrameComplete(pInternal, pOlder, myFirstSeq);
        }
    }

    // Update head sequence number if this frame is the head
    if (pFrame->timestamp == pInternal->headTimestamp) {
        UINT16 distNew = (UINT16) (pInternal->headSequenceNumber - pRtpPacket->header.sequenceNumber);
        if (distNew > 0 && distNew < 512) {
            pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        }
    }

EvictAndDeliver:
    // === Eviction pass: evict only the HEAD (earliest) frame if it exceeds maxLatency ===
    // After evicting, cascade to deliver/evict the next frames in order.
    {
        BOOL evicted = TRUE;
        while (evicted && pInternal->frameCount > 0) {
            evicted = FALSE;
            UINT32 earliestEvictIdx = rtFindEarliestFrameIdx(pInternal);
            age = rtTimestampAge(pInternal, pInternal->frames[earliestEvictIdx].timestamp);
            if (age > pInternal->maxLatency && !rtFrameIsComplete(pInternal, &pInternal->frames[earliestEvictIdx])) {
                DLOGD("rtEvict: DROP-STALE ts=%u seq=[%u..%u] pktCount=%u age=%u maxLatency=%llu",
                      pInternal->frames[earliestEvictIdx].timestamp,
                      pInternal->frames[earliestEvictIdx].firstSeqNum,
                      pInternal->frames[earliestEvictIdx].lastSeqNum,
                      pInternal->frames[earliestEvictIdx].packetCount, age, pInternal->maxLatency);
                UINT32 evictedTs = pInternal->frames[earliestEvictIdx].timestamp;
                rtMarkTimestampProcessed(pInternal, evictedTs);
                pInternal->onFrameDroppedFn(pInternal->customData, pInternal->frames[earliestEvictIdx].firstSeqNum,
                                            pInternal->frames[earliestEvictIdx].lastSeqNum, pInternal->frames[earliestEvictIdx].timestamp);
                rtFreePacketsInRange(pInternal, pInternal->frames[earliestEvictIdx].firstSeqNum, pInternal->frames[earliestEvictIdx].lastSeqNum);
                rtRemoveFrame(pInternal, earliestEvictIdx);
                pInternal->nextExpectedSeqValid = FALSE; // can't trust boundary after a drop
                pInternal->hasDelivered = TRUE;
                evicted = TRUE;
                rtUpdateHead(pInternal);
            }
        }
    }

    // === Ordered delivery pass: deliver consecutive complete frames from head ===
    {
        BOOL delivered = TRUE;
        while (delivered && pInternal->frameCount > 0) {
            delivered = FALSE;
            UINT32 earliestIdx = rtFindEarliestFrameIdx(pInternal);
            pFrame = &pInternal->frames[earliestIdx];

            if (rtFrameIsComplete(pInternal, pFrame)) {
                // Don't deliver the first frame until we've seen a packet from a different timestamp.
                // A single NAL packet with marker bit looks "complete" but may be part of a larger frame
                // whose other packets haven't arrived yet. Single-packet codecs (audio) bypass this.
                if (!pInternal->hasDelivered && !pInternal->alwaysSinglePacketFrames && pInternal->frameCount <= 1) {
                    break;
                }
                DLOGD("rtDeliver: DELIVER ts=%u seq=[%u..%u] pktCount=%u",
                      pFrame->timestamp, pFrame->firstSeqNum, pFrame->lastSeqNum, pFrame->packetCount);
                // Calculate frame size
                UINT32 deliveredTs = pFrame->timestamp;
                UINT16 deliveredLastSeq = pFrame->lastSeqNum;
                BOOL deliveredHasEnd = pFrame->hasEnd;
                rtMarkTimestampProcessed(pInternal, deliveredTs);
                CHK_STATUS(rtCalcFrameSize(pInternal, pFrame->firstSeqNum, pFrame->lastSeqNum, &frameSize));
                CHK_STATUS(pInternal->onFrameReadyFn(pInternal->customData, pFrame->firstSeqNum, pFrame->lastSeqNum, frameSize));
                // Check if the frame is still present (dropBufferData may have already removed it)
                UINT32 checkIdx = rtFindFrame(pInternal, deliveredTs);
                if (checkIdx < pInternal->frameCount) {
                    rtFreePacketsInRange(pInternal, pInternal->frames[checkIdx].firstSeqNum, pInternal->frames[checkIdx].lastSeqNum);
                    rtRemoveFrame(pInternal, checkIdx);
                }
                // If this frame ended with a marker bit, the next frame must start at lastSeqNum+1.
                // Only arm the flag if no existing frame already starts there (it may have been
                // created before this delivery fired, e.g. when the triggering packet belonged to
                // the next frame and arrived before some reordered packets of this frame).
                if (deliveredHasEnd) {
                    UINT16 expectedNext = (UINT16) (deliveredLastSeq + 1);
                    BOOL alreadyCovered = FALSE;
                    for (i = 0; i < pInternal->frameCount; i++) {
                        if (pInternal->frames[i].firstSeqNum == expectedNext) {
                            alreadyCovered = TRUE;
                            break;
                        }
                    }
                    if (!alreadyCovered) {
                        pInternal->nextExpectedSeqNum = expectedNext;
                        pInternal->nextExpectedSeqValid = TRUE;
                    }
                }
                pInternal->hasDelivered = TRUE;
                rtUpdateHead(pInternal);
                delivered = TRUE;
            }
        }
    }

CleanUp:
    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}

//
// Fill frame data - iterate seq range and depay into output buffer
//

static STATUS rtFillFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex, UINT16 endIndex)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PRealTimeJitterBufferInternal pInternal = (PRealTimeJitterBufferInternal) pJitterBuffer;
    UINT16 index;
    PRtpPacket pCurPacket;
    PBYTE pCurPtr = pFrame;
    UINT32 remaining = frameSize;
    UINT32 partialSize;
    BOOL isFirst = TRUE;

    CHK(pInternal != NULL && pFrame != NULL && pFilledSize != NULL, STATUS_NULL_ARG);

    for (index = startIndex;; index++) {
        pCurPacket = pInternal->pktRing[index % RT_PKT_RING_SIZE];
        if (pCurPacket == NULL || pCurPacket->header.sequenceNumber != index) {
            CHK(FALSE, STATUS_HASH_KEY_NOT_PRESENT);
        }
        partialSize = remaining;
        CHK_STATUS(pInternal->depayPayloadFn(pCurPacket->payload, pCurPacket->payloadLength, pCurPtr, &partialSize, &isFirst));
        pCurPtr += partialSize;
        remaining -= partialSize;
        isFirst = FALSE;
        if (index == endIndex) {
            break;
        }
    }

CleanUp:
    if (pFilledSize != NULL) {
        *pFilledSize = frameSize - remaining;
    }
    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}

//
// Fill partial frame data - skip missing packets
//

static STATUS rtFillPartialFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex,
                                     UINT16 endIndex)
{
    STATUS retStatus = STATUS_SUCCESS;
    PRealTimeJitterBufferInternal pInternal = (PRealTimeJitterBufferInternal) pJitterBuffer;
    UINT16 index;
    PRtpPacket pPacket;
    PBYTE pCurPtr = pFrame;
    UINT32 filledSize = 0;
    UINT32 partialSize;
    BOOL isFirst = TRUE;

    CHK(pInternal != NULL && pFilledSize != NULL, STATUS_NULL_ARG);

    for (index = startIndex;; index++) {
        pPacket = pInternal->pktRing[index % RT_PKT_RING_SIZE];
        if (pPacket != NULL && pPacket->header.sequenceNumber == index) {
            if (pFrame != NULL) {
                partialSize = frameSize - filledSize;
            } else {
                partialSize = 0;
            }
            BOOL depayIsFirst = isFirst;
            if (STATUS_FAILED(pInternal->depayPayloadFn(pPacket->payload, pPacket->payloadLength, pCurPtr, &partialSize, &depayIsFirst))) {
                if (index == endIndex)
                    break;
                continue;
            }
            if (pCurPtr != NULL) {
                pCurPtr += partialSize;
            }
            filledSize += partialSize;
            isFirst = FALSE;
        }
        if (index == endIndex) {
            break;
        }
    }

CleanUp:
    if (pFilledSize != NULL) {
        *pFilledSize = filledSize;
    }
    return retStatus;
}

//
// Get single packet by sequence number
//

static STATUS rtGetPacket(PJitterBuffer pJitterBuffer, UINT16 seqNum, PRtpPacket* ppPacket)
{
    STATUS retStatus = STATUS_SUCCESS;
    PRealTimeJitterBufferInternal pInternal = (PRealTimeJitterBufferInternal) pJitterBuffer;
    PRtpPacket pPacket;

    CHK(pInternal != NULL && ppPacket != NULL, STATUS_NULL_ARG);

    pPacket = pInternal->pktRing[seqNum % RT_PKT_RING_SIZE];
    if (pPacket == NULL || pPacket->header.sequenceNumber != seqNum) {
        *ppPacket = NULL;
        CHK(FALSE, STATUS_HASH_KEY_NOT_PRESENT);
    }
    *ppPacket = pPacket;

CleanUp:
    return retStatus;
}

//
// Drop buffer data - remove packets in range, update head
//

static STATUS rtDropBufferData(PJitterBuffer pJitterBuffer, UINT16 startIndex, UINT16 endIndex, UINT32 nextTimestamp)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PRealTimeJitterBufferInternal pInternal = (PRealTimeJitterBufferInternal) pJitterBuffer;
    UINT16 index;
    PRtpPacket pPacket;
    UINT32 i;

    CHK(pInternal != NULL, STATUS_NULL_ARG);

    // Free packets in range and update frame packet counts
    for (index = startIndex;; index++) {
        pPacket = pInternal->pktRing[index % RT_PKT_RING_SIZE];
        if (pPacket != NULL && pPacket->header.sequenceNumber == index) {
            // Decrement packet count for the owning frame
            UINT32 fi = rtFindFrame(pInternal, pPacket->header.timestamp);
            if (fi < pInternal->frameCount && pInternal->frames[fi].packetCount > 0) {
                pInternal->frames[fi].packetCount--;
            }
            freeRtpPacket(&pPacket);
            pInternal->pktRing[index % RT_PKT_RING_SIZE] = NULL;
        }
        if (index == endIndex) {
            break;
        }
    }

    // Remove frame entries whose packets were entirely within the dropped range.
    for (i = 0; i < pInternal->frameCount;) {
        PRtFrameEntry pEntry = &pInternal->frames[i];
        UINT16 seq = pEntry->firstSeqNum;
        BOOL hasRemaining = FALSE;
        for (;; seq++) {
            PRtpPacket pPkt = pInternal->pktRing[seq % RT_PKT_RING_SIZE];
            if (pPkt != NULL && pPkt->header.sequenceNumber == seq) {
                hasRemaining = TRUE;
                break;
            }
            if (seq == pEntry->lastSeqNum) {
                break;
            }
        }
        if (!hasRemaining) {
            rtRemoveFrame(pInternal, i);
        } else {
            i++;
        }
    }

    // Update head
    pInternal->headSequenceNumber = endIndex + 1;
    if (nextTimestamp != 0) {
        pInternal->headTimestamp = nextTimestamp;
    }

    if (pInternal->frameCount > 0) {
        rtUpdateHead(pInternal);
    }

    // Check overflow state exits
    if (pInternal->timestampOverFlowState) {
        if (pInternal->headTimestamp <= pInternal->base.tailTimestamp) {
            pInternal->timestampOverFlowState = FALSE;
        }
    }
    if (pInternal->sequenceNumberOverflowState) {
        if (pInternal->headSequenceNumber <= pInternal->tailSequenceNumber) {
            pInternal->sequenceNumberOverflowState = FALSE;
        }
    }

CleanUp:
    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}

//
// Destroy - flush remaining frames, free resources
//

static STATUS rtDestroy(PJitterBuffer* ppJitterBuffer)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PRealTimeJitterBufferInternal pInternal;
    UINT32 i, frameSize;

    CHK(ppJitterBuffer != NULL, STATUS_NULL_ARG);
    CHK(*ppJitterBuffer != NULL, retStatus);

    pInternal = (PRealTimeJitterBufferInternal) *ppJitterBuffer;

    // Flush remaining frames: deliver complete ones, drop incomplete ones
    if (pInternal->started && pInternal->frameCount > 0) {
        // Buffer closing — no more packets. Use fence scan between consecutive
        // frames (by timestamp order) to mark complete ones as ended.
        // For the last frame, there's no fence so use packetCount == seq range.
        for (i = 0; i < pInternal->frameCount; i++) {
            UINT16 fence = 0;
            if (pInternal->frames[i].hasEnd) {
                continue;
            }
            if (rtFindNextFrameFence(pInternal, pInternal->frames[i].timestamp, &fence)) {
                rtTryMarkFrameComplete(pInternal, &pInternal->frames[i], fence);
            } else {
                // Last frame — no fence. Use packetCount == seq range.
                UINT16 expectedCount = (UINT16) (pInternal->frames[i].lastSeqNum - pInternal->frames[i].firstSeqNum + 1);
                if (pInternal->frames[i].packetCount == expectedCount) {
                    pInternal->frames[i].hasEnd = TRUE;
                }
            }
        }
        // Process in timestamp order
        while (pInternal->frameCount > 0) {
            UINT32 earliestIdx = rtFindEarliestFrameIdx(pInternal);

            PRtFrameEntry pFrame = &pInternal->frames[earliestIdx];
            if (rtFrameIsComplete(pInternal, pFrame)) {
                rtCalcFrameSize(pInternal, pFrame->firstSeqNum, pFrame->lastSeqNum, &frameSize);
                pInternal->onFrameReadyFn(pInternal->customData, pFrame->firstSeqNum, pFrame->lastSeqNum, frameSize);
                // Check if frame still exists (callback may have called dropBufferData)
                UINT32 checkIdx = rtFindFrame(pInternal, pFrame->timestamp);
                if (checkIdx < pInternal->frameCount) {
                    rtFreePacketsInRange(pInternal, pInternal->frames[checkIdx].firstSeqNum, pInternal->frames[checkIdx].lastSeqNum);
                    rtRemoveFrame(pInternal, checkIdx);
                }
            } else {
                pInternal->onFrameDroppedFn(pInternal->customData, pFrame->firstSeqNum, pFrame->lastSeqNum, pFrame->timestamp);
                rtFreePacketsInRange(pInternal, pFrame->firstSeqNum, pFrame->lastSeqNum);
                rtRemoveFrame(pInternal, earliestIdx);
            }
        }
    }

    SAFE_MEMFREE(*ppJitterBuffer);

CleanUp:
    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}
