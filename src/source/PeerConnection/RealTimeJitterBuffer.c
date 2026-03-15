#define LOG_CLASS "RealTimeJitterBuffer"

#include "../Include_i.h"

#define RT_MAX_FRAMES               2048
#define RT_HASH_TABLE_BUCKET_COUNT  3000
#define RT_HASH_TABLE_BUCKET_LENGTH 2
#define RT_PROCESSED_TS_RING_SIZE   512

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
    PHashTable pPkgBufferHashTable;
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
        if (timestamp <= pInternal->base.tailTimestamp) {
            // Both on the wrapped side
            return pInternal->base.tailTimestamp - timestamp;
        } else {
            // timestamp is on the old side (before wrap)
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
        // In overflow state, high timestamps are "older" (before wrap) and low are "newer" (after wrap)
        BOOL aWrapped = (a <= pInternal->base.tailTimestamp);
        BOOL bWrapped = (b <= pInternal->base.tailTimestamp);
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

// Free all packets in the hash table for a given seq range [first, last] inclusive
static STATUS rtFreePacketsInRange(PRealTimeJitterBufferInternal pInternal, UINT16 firstSeq, UINT16 lastSeq)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 seq;
    UINT64 hashValue;
    PRtpPacket pPacket;
    BOOL hasEntry;

    for (seq = firstSeq;; seq++) {
        if (STATUS_SUCCEEDED(hashTableContains(pInternal->pPkgBufferHashTable, seq, &hasEntry)) && hasEntry) {
            if (STATUS_SUCCEEDED(hashTableGet(pInternal->pPkgBufferHashTable, seq, &hashValue))) {
                pPacket = (PRtpPacket) hashValue;
                if (pPacket != NULL) {
                    freeRtpPacket(&pPacket);
                }
            }
            hashTableRemove(pInternal->pPkgBufferHashTable, seq);
        }
        if (seq == lastSeq) {
            break;
        }
    }

    return retStatus;
}

// Calculate frame size by iterating packets and calling depay with NULL output
static STATUS rtCalcFrameSize(PRealTimeJitterBufferInternal pInternal, UINT16 firstSeq, UINT16 lastSeq, PUINT32 pFrameSize)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 seq;
    UINT64 hashValue;
    PRtpPacket pPacket;
    BOOL hasEntry;
    UINT32 totalSize = 0;
    UINT32 partialSize;
    BOOL isFirst = TRUE;

    for (seq = firstSeq;; seq++) {
        if (STATUS_SUCCEEDED(hashTableContains(pInternal->pPkgBufferHashTable, seq, &hasEntry)) && hasEntry) {
            if (STATUS_SUCCEEDED(hashTableGet(pInternal->pPkgBufferHashTable, seq, &hashValue))) {
                pPacket = (PRtpPacket) hashValue;
                if (pPacket != NULL) {
                    partialSize = 0;
                    BOOL depayIsFirst = isFirst;
                    pInternal->depayPayloadFn(pPacket->payload, pPacket->payloadLength, NULL, &partialSize, &depayIsFirst);
                    totalSize += partialSize;
                    isFirst = FALSE;
                }
            }
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

// Update headTimestamp and headSequenceNumber to the earliest frame in the buffer
static VOID rtUpdateHead(PRealTimeJitterBufferInternal pInternal)
{
    if (pInternal->frameCount == 0) {
        return;
    }

    UINT32 earliestIdx = 0;
    UINT32 i;
    for (i = 1; i < pInternal->frameCount; i++) {
        if (rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, pInternal->frames[earliestIdx].timestamp) < 0) {
            earliestIdx = i;
        }
    }
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

    CHK_STATUS(hashTableCreateWithParams(RT_HASH_TABLE_BUCKET_COUNT, RT_HASH_TABLE_BUCKET_LENGTH, &pInternal->pPkgBufferHashTable));
    pInternal->processedTsHead = 0;
    pInternal->processedTsCount = 0;

CleanUp:
    if (STATUS_FAILED(retStatus) && pInternal != NULL) {
        if (pInternal->pPkgBufferHashTable != NULL) {
            hashTableFree(pInternal->pPkgBufferHashTable);
        }
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
    UINT64 hashValue;
    PRtpPacket pExisting;
    BOOL hasEntry = FALSE;
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

    // Sequence number overflow tracking
    if (!pInternal->sequenceNumberOverflowState) {
        UINT16 packetsUntilOverflow = MAX_RTP_SEQUENCE_NUM - pInternal->tailSequenceNumber;
        if (packetsUntilOverflow <= 512 && pRtpPacket->header.sequenceNumber < pInternal->tailSequenceNumber &&
            pRtpPacket->header.sequenceNumber <= 512 - packetsUntilOverflow) {
            pInternal->sequenceNumberOverflowState = TRUE;
            pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        }
    }

    // Update tail sequence number
    if (pInternal->sequenceNumberOverflowState) {
        if (pRtpPacket->header.sequenceNumber <= pInternal->tailSequenceNumber || pRtpPacket->header.sequenceNumber > pInternal->headSequenceNumber) {
            // Could be new tail in wrapped space
            // Use simple distance check
            UINT16 distFromTail = pRtpPacket->header.sequenceNumber - pInternal->tailSequenceNumber;
            if (distFromTail > 0 && distFromTail < 512) {
                pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
            }
        }
    } else {
        if (pRtpPacket->header.sequenceNumber > pInternal->tailSequenceNumber) {
            pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        }
    }

    // Timestamp overflow tracking
    if (!pInternal->timestampOverFlowState) {
        if (pInternal->base.tailTimestamp > pRtpPacket->header.timestamp && pRtpPacket->header.timestamp < pInternal->headTimestamp) {
            // Potential overflow: new packet timestamp is much smaller than tail
            // Only if this packet advances the tail sequence
            if (pRtpPacket->header.sequenceNumber == pInternal->tailSequenceNumber) {
                pInternal->timestampOverFlowState = TRUE;
                pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
            }
        }
    }

    // Update tail timestamp
    if (pInternal->timestampOverFlowState) {
        if (pRtpPacket->header.timestamp <= pInternal->base.tailTimestamp) {
            UINT32 dist = pRtpPacket->header.timestamp - pInternal->base.tailTimestamp;
            // Unsigned wraparound: small positive means just ahead
            if (dist > 0 && dist < pInternal->maxLatency) {
                pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
            }
        }
    } else {
        if (pRtpPacket->header.timestamp > pInternal->base.tailTimestamp) {
            pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
        }
    }

    // Latency tolerance check - discard if too old
    age = rtTimestampAge(pInternal, pRtpPacket->header.timestamp);
    if (age > pInternal->maxLatency) {
        DLOGS("RealTimeJitterBuffer: discarding packet seq %u ts %u (age %u > maxLatency %llu)", pRtpPacket->header.sequenceNumber,
              pRtpPacket->header.timestamp, age, pInternal->maxLatency);
        freeRtpPacket(&pRtpPacket);
        if (pPacketDiscarded != NULL) {
            *pPacketDiscarded = TRUE;
        }
        // Still run eviction/delivery before returning
        goto EvictAndDeliver;
    }

    // Check for duplicate - if exists, replace but don't increment packetCount
    CHK_STATUS(hashTableContains(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, &hasEntry));
    if (hasEntry) {
        hashTableGet(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, &hashValue);
        pExisting = (PRtpPacket) hashValue;
        if (pExisting != NULL) {
            freeRtpPacket(&pExisting);
        }
        hashTableRemove(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber);
        CHK_STATUS(hashTablePut(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, (UINT64) pRtpPacket));
        // Don't update frame entry for duplicates
        goto EvictAndDeliver;
    }

    // Find or create frame entry
    frameIdx = rtFindFrame(pInternal, pRtpPacket->header.timestamp);
    if (frameIdx == pInternal->frameCount) {
        // No existing frame entry — check if this timestamp was already delivered/dropped
        BOOL alreadyProcessed = rtIsTimestampProcessed(pInternal, pRtpPacket->header.timestamp);
        if (alreadyProcessed) {
            freeRtpPacket(&pRtpPacket);
            if (pPacketDiscarded != NULL) {
                *pPacketDiscarded = TRUE;
            }
            goto EvictAndDeliver;
        }
    }

    // Store packet
    CHK_STATUS(hashTablePut(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, (UINT64) pRtpPacket));

    if (frameIdx == pInternal->frameCount) {
        // Create new frame entry
        CHK(pInternal->frameCount < RT_MAX_FRAMES, STATUS_NOT_ENOUGH_MEMORY);
        frameIdx = pInternal->frameCount;
        pInternal->frameCount++;
        pFrame = &pInternal->frames[frameIdx];
        pFrame->timestamp = pRtpPacket->header.timestamp;
        pFrame->firstSeqNum = pRtpPacket->header.sequenceNumber;
        pFrame->lastSeqNum = pRtpPacket->header.sequenceNumber;
        pFrame->packetCount = 0;
        pFrame->hasStart = FALSE;
        pFrame->hasEnd = FALSE;

        // Update head if this is earlier
        if (rtTimestampCompare(pInternal, pRtpPacket->header.timestamp, pInternal->headTimestamp) < 0) {
            pInternal->headTimestamp = pRtpPacket->header.timestamp;
            pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
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
            // Find the earliest frame
            UINT32 earliestEvictIdx = 0;
            for (i = 1; i < pInternal->frameCount; i++) {
                if (rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, pInternal->frames[earliestEvictIdx].timestamp) < 0) {
                    earliestEvictIdx = i;
                }
            }
            age = rtTimestampAge(pInternal, pInternal->frames[earliestEvictIdx].timestamp);
            if (age > pInternal->maxLatency && !rtFrameIsComplete(pInternal, &pInternal->frames[earliestEvictIdx])) {
                DLOGS("RealTimeJitterBuffer: evicting stale frame ts %u (age %u)", pInternal->frames[earliestEvictIdx].timestamp, age);
                UINT32 evictedTs = pInternal->frames[earliestEvictIdx].timestamp;
                rtMarkTimestampProcessed(pInternal, evictedTs);
                pInternal->onFrameDroppedFn(pInternal->customData, pInternal->frames[earliestEvictIdx].firstSeqNum,
                                            pInternal->frames[earliestEvictIdx].lastSeqNum, pInternal->frames[earliestEvictIdx].timestamp);
                rtFreePacketsInRange(pInternal, pInternal->frames[earliestEvictIdx].firstSeqNum, pInternal->frames[earliestEvictIdx].lastSeqNum);
                rtRemoveFrame(pInternal, earliestEvictIdx);
                pInternal->hasDelivered = TRUE;
                evicted = TRUE;
                if (pInternal->frameCount > 0) {
                    rtUpdateHead(pInternal);
                }
            }
        }
    }

    // === Ordered delivery pass: deliver consecutive complete frames from head ===
    {
        BOOL delivered = TRUE;
        while (delivered && pInternal->frameCount > 0) {
            delivered = FALSE;
            // Find the frame with the earliest timestamp
            UINT32 earliestIdx = 0;
            for (i = 1; i < pInternal->frameCount; i++) {
                if (rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, pInternal->frames[earliestIdx].timestamp) < 0) {
                    earliestIdx = i;
                }
            }
            pFrame = &pInternal->frames[earliestIdx];

            if (rtFrameIsComplete(pInternal, pFrame)) {
                // Don't deliver the first frame until we've seen a packet from a different timestamp.
                // A single NAL packet with marker bit looks "complete" but may be part of a larger frame
                // whose other packets haven't arrived yet. Single-packet codecs (audio) bypass this.
                if (!pInternal->hasDelivered && !pInternal->alwaysSinglePacketFrames && pInternal->frameCount <= 1) {
                    break;
                }
                // Calculate frame size
                UINT32 deliveredTs = pFrame->timestamp;
                rtMarkTimestampProcessed(pInternal, deliveredTs);
                CHK_STATUS(rtCalcFrameSize(pInternal, pFrame->firstSeqNum, pFrame->lastSeqNum, &frameSize));
                CHK_STATUS(pInternal->onFrameReadyFn(pInternal->customData, pFrame->firstSeqNum, pFrame->lastSeqNum, frameSize));
                // Check if the frame is still present (dropBufferData may have already removed it)
                UINT32 checkIdx = rtFindFrame(pInternal, deliveredTs);
                if (checkIdx < pInternal->frameCount) {
                    rtFreePacketsInRange(pInternal, pInternal->frames[checkIdx].firstSeqNum, pInternal->frames[checkIdx].lastSeqNum);
                    rtRemoveFrame(pInternal, checkIdx);
                }
                pInternal->hasDelivered = TRUE;
                if (pInternal->frameCount > 0) {
                    rtUpdateHead(pInternal);
                }
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
    UINT64 hashValue;
    PRtpPacket pCurPacket;
    PBYTE pCurPtr = pFrame;
    UINT32 remaining = frameSize;
    UINT32 partialSize;
    BOOL isFirst = TRUE;

    CHK(pInternal != NULL && pFrame != NULL && pFilledSize != NULL, STATUS_NULL_ARG);

    for (index = startIndex;; index++) {
        hashValue = 0;
        CHK_STATUS(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue));
        pCurPacket = (PRtpPacket) hashValue;
        CHK(pCurPacket != NULL, STATUS_NULL_ARG);
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
    UINT64 hashValue;
    PRtpPacket pPacket;
    PBYTE pCurPtr = pFrame;
    UINT32 filledSize = 0;
    UINT32 partialSize;
    BOOL hasEntry;
    BOOL isFirst = TRUE;

    CHK(pInternal != NULL && pFilledSize != NULL, STATUS_NULL_ARG);

    for (index = startIndex;; index++) {
        if (STATUS_FAILED(hashTableContains(pInternal->pPkgBufferHashTable, index, &hasEntry))) {
            if (index == endIndex)
                break;
            continue;
        }
        if (hasEntry) {
            if (STATUS_FAILED(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue))) {
                if (index == endIndex)
                    break;
                continue;
            }
            pPacket = (PRtpPacket) hashValue;
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
    UINT64 hashValue = 0;

    CHK(pInternal != NULL && ppPacket != NULL, STATUS_NULL_ARG);

    retStatus = hashTableGet(pInternal->pPkgBufferHashTable, seqNum, &hashValue);
    if (retStatus == STATUS_HASH_KEY_NOT_PRESENT) {
        *ppPacket = NULL;
        CHK(FALSE, retStatus);
    }
    CHK_STATUS(retStatus);
    *ppPacket = (PRtpPacket) hashValue;

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
    UINT64 hashValue;
    PRtpPacket pPacket;
    BOOL hasEntry;
    UINT32 i;

    CHK(pInternal != NULL, STATUS_NULL_ARG);

    // Free packets in range
    for (index = startIndex;; index++) {
        if (STATUS_SUCCEEDED(hashTableContains(pInternal->pPkgBufferHashTable, index, &hasEntry)) && hasEntry) {
            if (STATUS_SUCCEEDED(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue))) {
                pPacket = (PRtpPacket) hashValue;
                if (pPacket != NULL) {
                    freeRtpPacket(&pPacket);
                }
            }
            hashTableRemove(pInternal->pPkgBufferHashTable, index);
        }
        if (index == endIndex) {
            break;
        }
    }

    // Remove any frame entries that overlap this seq range
    // We identify by checking if the frame's seq range overlaps [startIndex, endIndex]
    for (i = 0; i < pInternal->frameCount;) {
        PRtFrameEntry pEntry = &pInternal->frames[i];
        // Check if frame's seq range overlaps the dropped range
        // Simple check: if the frame's firstSeqNum is in the dropped range
        BOOL overlaps = FALSE;
        UINT16 seq;
        for (seq = startIndex;; seq++) {
            if (seq == pEntry->firstSeqNum || seq == pEntry->lastSeqNum) {
                overlaps = TRUE;
                break;
            }
            if (seq == endIndex) {
                break;
            }
        }
        if (overlaps) {
            rtRemoveFrame(pInternal, i);
            // don't increment i, the swapped entry is now at i
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
        // Buffer closing — no more packets. Mark frames as ended if they
        // have all packets in their sequence range.
        for (i = 0; i < pInternal->frameCount; i++) {
            if (!pInternal->frames[i].hasEnd) {
                UINT16 expectedCount = (UINT16) (pInternal->frames[i].lastSeqNum - pInternal->frames[i].firstSeqNum + 1);
                if (pInternal->frames[i].packetCount == expectedCount) {
                    pInternal->frames[i].hasEnd = TRUE;
                }
            }
        }
        // Process in timestamp order
        while (pInternal->frameCount > 0) {
            // Find earliest frame
            UINT32 earliestIdx = 0;
            for (i = 1; i < pInternal->frameCount; i++) {
                if (rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, pInternal->frames[earliestIdx].timestamp) < 0) {
                    earliestIdx = i;
                }
            }

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

    hashTableFree(pInternal->pPkgBufferHashTable);
    SAFE_MEMFREE(*ppJitterBuffer);

CleanUp:
    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}
