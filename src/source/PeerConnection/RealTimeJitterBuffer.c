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

        // A new timestamp means the sender moved on. For older frames without
        // a marker bit, use the new packet's sequence number as a fence: all
        // sequence numbers from firstSeqNum to fence-1 should belong to the
        // older frame. If every slot in that range is present and matches the
        // older frame's timestamp, the frame is complete.
        for (i = 0; i < pInternal->frameCount; i++) {
            PRtFrameEntry pOlder = &pInternal->frames[i];
            if (pOlder->timestamp != pRtpPacket->header.timestamp && !pOlder->hasEnd &&
                rtTimestampCompare(pInternal, pOlder->timestamp, pRtpPacket->header.timestamp) < 0) {
                UINT16 fence = pRtpPacket->header.sequenceNumber;
                UINT16 slotsInRange = (UINT16) (fence - pOlder->firstSeqNum); // UINT16 wraparound-safe
                if (slotsInRange == 0 || slotsInRange > 1000) {
                    continue; // degenerate — skip
                }
                // Scan from firstSeqNum toward fence. Stop at first missing packet or
                // foreign-timestamp packet — that's the actual frame boundary.
                // A foreign-timestamp packet is NOT an error; it means the frame ended there.
                UINT16 seq = pOlder->firstSeqNum;
                UINT16 present = 0;
                BOOL gapFound = FALSE;
                BOOL hasEntry = FALSE;
                UINT64 hashVal = 0;
                PRtpPacket pPkt = NULL;
                for (; seq != fence; seq++) {
                    if (STATUS_FAILED(hashTableContains(pInternal->pPkgBufferHashTable, seq, &hasEntry)) || !hasEntry) {
                        gapFound = TRUE;
                        break;
                    }
                    if (STATUS_FAILED(hashTableGet(pInternal->pPkgBufferHashTable, seq, &hashVal))) {
                        gapFound = TRUE;
                        break;
                    }
                    pPkt = (PRtpPacket) hashVal;
                    if (pPkt == NULL) {
                        gapFound = TRUE;
                        break;
                    }
                    if (pPkt->header.timestamp != pOlder->timestamp) {
                        break; // hit next frame's packet — frame boundary found, not a gap
                    }
                    present++;
                }
                if (!gapFound && present == pOlder->packetCount) {
                    pOlder->hasEnd = TRUE;
                    pOlder->lastSeqNum = (UINT16) (fence - 1);
                }
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
    }

    // Re-evaluate frame completion after adding a late packet to an existing frame.
    // Find the next frame in timestamp order and use its firstSeqNum as fence.
    if (!pFrame->hasEnd && pFrame->hasStart && pInternal->frameCount > 1) {
        UINT16 fence = 0;
        BOOL foundFence = FALSE;
        UINT32 bestTs = 0;
        // Find the frame with the smallest timestamp that is still greater than pFrame->timestamp
        for (i = 0; i < pInternal->frameCount; i++) {
            if (pInternal->frames[i].timestamp != pFrame->timestamp &&
                rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, pFrame->timestamp) > 0) {
                if (!foundFence || rtTimestampCompare(pInternal, pInternal->frames[i].timestamp, bestTs) < 0) {
                    fence = pInternal->frames[i].firstSeqNum;
                    bestTs = pInternal->frames[i].timestamp;
                    foundFence = TRUE;
                }
            }
        }
        if (foundFence) {
            UINT16 slotsInRange = (UINT16) (fence - pFrame->firstSeqNum);
            if (slotsInRange > 0 && slotsInRange <= 1000) {
                UINT16 seq = pFrame->firstSeqNum;
                UINT16 present = 0;
                BOOL gapFound = FALSE;
                BOOL hasEntry = FALSE;
                UINT64 hashVal = 0;
                PRtpPacket pPkt = NULL;
                for (; seq != fence; seq++) {
                    if (STATUS_FAILED(hashTableContains(pInternal->pPkgBufferHashTable, seq, &hasEntry)) || !hasEntry) {
                        gapFound = TRUE;
                        break;
                    }
                    if (STATUS_FAILED(hashTableGet(pInternal->pPkgBufferHashTable, seq, &hashVal))) {
                        gapFound = TRUE;
                        break;
                    }
                    pPkt = (PRtpPacket) hashVal;
                    if (pPkt == NULL) {
                        gapFound = TRUE;
                        break;
                    }
                    if (pPkt->header.timestamp != pFrame->timestamp) {
                        break;
                    }
                    present++;
                }
                if (!gapFound && present == pFrame->packetCount) {
                    pFrame->hasEnd = TRUE;
                    pFrame->lastSeqNum = (UINT16) (fence - 1);
                }
            }
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
            // (i.e., no other frame sits between pOlder and pFrame in timestamp order)
            BOOL isNextFrame = TRUE;
            UINT32 k;
            for (k = 0; k < pInternal->frameCount; k++) {
                if (k == i || pInternal->frames[k].timestamp == myTimestamp) {
                    continue;
                }
                if (rtTimestampCompare(pInternal, pInternal->frames[k].timestamp, pOlder->timestamp) > 0 &&
                    rtTimestampCompare(pInternal, pInternal->frames[k].timestamp, myTimestamp) < 0) {
                    isNextFrame = FALSE;
                    break;
                }
            }
            if (!isNextFrame) {
                continue;
            }
            UINT16 fence = myFirstSeq;
            UINT16 slotsInRange = (UINT16) (fence - pOlder->firstSeqNum);
            if (slotsInRange == 0 || slotsInRange > 1000) {
                continue;
            }
            UINT16 seq = pOlder->firstSeqNum;
            UINT16 present = 0;
            BOOL gapFound = FALSE;
            BOOL hasEntry = FALSE;
            UINT64 hashVal = 0;
            PRtpPacket pPkt = NULL;
            for (; seq != fence; seq++) {
                if (STATUS_FAILED(hashTableContains(pInternal->pPkgBufferHashTable, seq, &hasEntry)) || !hasEntry) {
                    gapFound = TRUE;
                    break;
                }
                if (STATUS_FAILED(hashTableGet(pInternal->pPkgBufferHashTable, seq, &hashVal))) {
                    gapFound = TRUE;
                    break;
                }
                pPkt = (PRtpPacket) hashVal;
                if (pPkt == NULL) {
                    gapFound = TRUE;
                    break;
                }
                if (pPkt->header.timestamp != pOlder->timestamp) {
                    break;
                }
                present++;
            }
            if (!gapFound && present == pOlder->packetCount) {
                pOlder->hasEnd = TRUE;
                pOlder->lastSeqNum = (UINT16) (fence - 1);
            }
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

    // Free packets in range and update frame packet counts
    for (index = startIndex;; index++) {
        if (STATUS_SUCCEEDED(hashTableContains(pInternal->pPkgBufferHashTable, index, &hasEntry)) && hasEntry) {
            if (STATUS_SUCCEEDED(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue))) {
                pPacket = (PRtpPacket) hashValue;
                if (pPacket != NULL) {
                    // Decrement packet count for the owning frame
                    UINT32 fi = rtFindFrame(pInternal, pPacket->header.timestamp);
                    if (fi < pInternal->frameCount && pInternal->frames[fi].packetCount > 0) {
                        pInternal->frames[fi].packetCount--;
                    }
                    freeRtpPacket(&pPacket);
                }
            }
            hashTableRemove(pInternal->pPkgBufferHashTable, index);
        }
        if (index == endIndex) {
            break;
        }
    }

    // Remove frame entries whose packets were entirely within the dropped range.
    // Frames with remaining packets outside the range are kept for destroy to handle.
    for (i = 0; i < pInternal->frameCount;) {
        PRtFrameEntry pEntry = &pInternal->frames[i];
        // Check if ANY of this frame's packets still exist in the hash table
        UINT16 seq = pEntry->firstSeqNum;
        BOOL hasRemaining = FALSE;
        for (;; seq++) {
            BOOL hasEntry = FALSE;
            if (STATUS_SUCCEEDED(hashTableContains(pInternal->pPkgBufferHashTable, seq, &hasEntry)) && hasEntry) {
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
            if (pInternal->frames[i].hasEnd) {
                continue;
            }
            // Find the next frame in timestamp order to use as fence
            UINT16 fence = 0;
            BOOL foundFence = FALSE;
            UINT32 bestTs = 0;
            UINT32 j;
            for (j = 0; j < pInternal->frameCount; j++) {
                if (j != i && rtTimestampCompare(pInternal, pInternal->frames[j].timestamp, pInternal->frames[i].timestamp) > 0) {
                    if (!foundFence || rtTimestampCompare(pInternal, pInternal->frames[j].timestamp, bestTs) < 0) {
                        fence = pInternal->frames[j].firstSeqNum;
                        bestTs = pInternal->frames[j].timestamp;
                        foundFence = TRUE;
                    }
                }
            }
            if (foundFence) {
                // Fence scan: check contiguous same-timestamp packets from firstSeqNum to fence
                UINT16 seq = pInternal->frames[i].firstSeqNum;
                UINT16 present = 0;
                BOOL gapFound = FALSE;
                BOOL hasEntry = FALSE;
                UINT64 hashVal = 0;
                PRtpPacket pPkt = NULL;
                for (; seq != fence; seq++) {
                    if (STATUS_FAILED(hashTableContains(pInternal->pPkgBufferHashTable, seq, &hasEntry)) || !hasEntry) {
                        gapFound = TRUE;
                        break;
                    }
                    if (STATUS_FAILED(hashTableGet(pInternal->pPkgBufferHashTable, seq, &hashVal))) {
                        gapFound = TRUE;
                        break;
                    }
                    pPkt = (PRtpPacket) hashVal;
                    if (pPkt == NULL) {
                        gapFound = TRUE;
                        break;
                    }
                    if (pPkt->header.timestamp != pInternal->frames[i].timestamp) {
                        break;
                    }
                    present++;
                }
                if (!gapFound && present == pInternal->frames[i].packetCount) {
                    pInternal->frames[i].hasEnd = TRUE;
                    pInternal->frames[i].lastSeqNum = (UINT16) (pInternal->frames[i].firstSeqNum + present - 1);
                }
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
