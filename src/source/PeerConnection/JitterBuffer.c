#define LOG_CLASS "JitterBuffer"

#include "../Include_i.h"

// Applies only to the case where the very first frame has its first packets out of order
#define MAX_OUT_OF_ORDER_PACKET_DIFFERENCE 512

#define JITTER_BUFFER_HASH_TABLE_BUCKET_COUNT  3000
#define JITTER_BUFFER_HASH_TABLE_BUCKET_LENGTH 2

// Internal struct for the default jitter buffer implementation.
// The base JitterBuffer must be the first member so that a PJitterBuffer
// pointer can be safely cast to PJitterBufferInternal.
typedef struct {
    JitterBuffer base;
    FrameReadyFunc onFrameReadyFn;
    FrameDroppedFunc onFrameDroppedFn;
    DepayRtpPayloadFunc depayPayloadFn;
    UINT16 headSequenceNumber;
    UINT16 tailSequenceNumber;
    UINT32 headTimestamp;
    UINT64 maxLatency;
    UINT64 customData;
    BOOL started;
    BOOL firstFrameProcessed;
    BOOL sequenceNumberOverflowState;
    BOOL timestampOverFlowState;
    BOOL alwaysSinglePacketFrames;
    PHashTable pPkgBufferHashTable;
} JitterBufferInternal, *PJitterBufferInternal;

// forward declarations of default implementations
static STATUS defaultPush(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket, PBOOL pPacketDiscarded);
static STATUS defaultDestroy(PJitterBuffer* ppJitterBuffer);
static STATUS defaultFillFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex,
                                   UINT16 endIndex);
static STATUS defaultFillPartialFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex,
                                          UINT16 endIndex);
static STATUS defaultGetPacket(PJitterBuffer pJitterBuffer, UINT16 seqNum, PRtpPacket* ppPacket);
static STATUS defaultDropBufferData(PJitterBuffer pJitterBuffer, UINT16 startIndex, UINT16 endIndex, UINT32 nextTimestamp);
static STATUS jitterBufferInternalParse(PJitterBufferInternal pInternal, BOOL bufferClosed);

//
// Public dispatcher functions — these call through the vtable
//

STATUS jitterBufferPush(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket, PBOOL pPacketDiscarded)
{
    if (pJitterBuffer == NULL || pJitterBuffer->pushFn == NULL) {
        return STATUS_NULL_ARG;
    }
    return pJitterBuffer->pushFn(pJitterBuffer, pRtpPacket, pPacketDiscarded);
}

STATUS freeJitterBuffer(PJitterBuffer* ppJitterBuffer)
{
    if (ppJitterBuffer == NULL) {
        return STATUS_NULL_ARG;
    }
    if (*ppJitterBuffer == NULL) {
        return STATUS_SUCCESS;
    }
    if ((*ppJitterBuffer)->destroyFn == NULL) {
        return STATUS_NULL_ARG;
    }
    return (*ppJitterBuffer)->destroyFn(ppJitterBuffer);
}

STATUS jitterBufferFillFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex, UINT16 endIndex)
{
    if (pJitterBuffer == NULL || pJitterBuffer->fillFrameDataFn == NULL) {
        return STATUS_NULL_ARG;
    }
    return pJitterBuffer->fillFrameDataFn(pJitterBuffer, pFrame, frameSize, pFilledSize, startIndex, endIndex);
}

STATUS jitterBufferFillPartialFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex,
                                        UINT16 endIndex)
{
    if (pJitterBuffer == NULL || pJitterBuffer->fillPartialFrameDataFn == NULL) {
        return STATUS_NULL_ARG;
    }
    return pJitterBuffer->fillPartialFrameDataFn(pJitterBuffer, pFrame, frameSize, pFilledSize, startIndex, endIndex);
}

STATUS jitterBufferGetPacket(PJitterBuffer pJitterBuffer, UINT16 seqNum, PRtpPacket* ppPacket)
{
    if (pJitterBuffer == NULL || pJitterBuffer->getPacketFn == NULL) {
        return STATUS_NULL_ARG;
    }
    return pJitterBuffer->getPacketFn(pJitterBuffer, seqNum, ppPacket);
}

STATUS jitterBufferDropBufferData(PJitterBuffer pJitterBuffer, UINT16 startIndex, UINT16 endIndex, UINT32 nextTimestamp)
{
    if (pJitterBuffer == NULL || pJitterBuffer->dropBufferDataFn == NULL) {
        return STATUS_NULL_ARG;
    }
    return pJitterBuffer->dropBufferDataFn(pJitterBuffer, startIndex, endIndex, nextTimestamp);
}

//
// Constructor — allocates JitterBufferInternal and wires vtable
//

STATUS createJitterBuffer(FrameReadyFunc onFrameReadyFunc, FrameDroppedFunc onFrameDroppedFunc, DepayRtpPayloadFunc depayRtpPayloadFunc,
                          UINT32 maxLatency, UINT32 clockRate, UINT64 customData, BOOL alwaysSinglePacketFrames, PJitterBuffer* ppJitterBuffer)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PJitterBufferInternal pInternal = NULL;

    CHK(ppJitterBuffer != NULL && onFrameReadyFunc != NULL && onFrameDroppedFunc != NULL && depayRtpPayloadFunc != NULL, STATUS_NULL_ARG);
    CHK(clockRate != 0, STATUS_INVALID_ARG);

    pInternal = (PJitterBufferInternal) MEMALLOC(SIZEOF(JitterBufferInternal));
    CHK(pInternal != NULL, STATUS_NOT_ENOUGH_MEMORY);

    // Wire vtable
    pInternal->base.pushFn = defaultPush;
    pInternal->base.destroyFn = defaultDestroy;
    pInternal->base.fillFrameDataFn = defaultFillFrameData;
    pInternal->base.fillPartialFrameDataFn = defaultFillPartialFrameData;
    pInternal->base.getPacketFn = defaultGetPacket;
    pInternal->base.dropBufferDataFn = defaultDropBufferData;

    // Public fields
    pInternal->base.clockRate = clockRate;
    pInternal->base.transit = 0;
    pInternal->base.jitter = 0;
    pInternal->base.tailTimestamp = 0;

    // Private fields
    pInternal->onFrameReadyFn = onFrameReadyFunc;
    pInternal->onFrameDroppedFn = onFrameDroppedFunc;
    pInternal->depayPayloadFn = depayRtpPayloadFunc;

    pInternal->maxLatency = maxLatency;
    if (pInternal->maxLatency == 0) {
        pInternal->maxLatency = DEFAULT_JITTER_BUFFER_MAX_LATENCY;
    }
    pInternal->maxLatency = pInternal->maxLatency * pInternal->base.clockRate / HUNDREDS_OF_NANOS_IN_A_SECOND;

    CHK(pInternal->maxLatency < MAX_RTP_TIMESTAMP, STATUS_INVALID_ARG);

    pInternal->headTimestamp = MAX_UINT32;
    pInternal->headSequenceNumber = MAX_RTP_SEQUENCE_NUM;
    pInternal->tailSequenceNumber = MAX_RTP_SEQUENCE_NUM;
    pInternal->started = FALSE;
    pInternal->firstFrameProcessed = FALSE;
    pInternal->timestampOverFlowState = FALSE;
    pInternal->sequenceNumberOverflowState = FALSE;
    pInternal->alwaysSinglePacketFrames = alwaysSinglePacketFrames;

    pInternal->customData = customData;
    CHK_STATUS(
        hashTableCreateWithParams(JITTER_BUFFER_HASH_TABLE_BUCKET_COUNT, JITTER_BUFFER_HASH_TABLE_BUCKET_LENGTH, &pInternal->pPkgBufferHashTable));

CleanUp:
    if (STATUS_FAILED(retStatus) && pInternal != NULL) {
        PJitterBuffer pTemp = (PJitterBuffer) pInternal;
        freeJitterBuffer(&pTemp);
        pInternal = NULL;
    }

    if (ppJitterBuffer != NULL) {
        *ppJitterBuffer = (PJitterBuffer) pInternal;
    }

    LEAVES();
    return retStatus;
}

//
// Default implementation — internal helpers
//

static BOOL underflowPossible(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    UINT32 seqNoDifference = 0;
    UINT64 timestampDifference = 0;
    UINT64 maxTimePassed = 0;
    if (pInternal->headTimestamp == pRtpPacket->header.timestamp) {
        retVal = TRUE;
    } else {
        seqNoDifference = (MAX_RTP_SEQUENCE_NUM - pRtpPacket->header.sequenceNumber) + pInternal->headSequenceNumber;
        if (pInternal->headTimestamp > pRtpPacket->header.timestamp) {
            timestampDifference = pInternal->headTimestamp - pRtpPacket->header.timestamp;
        } else {
            timestampDifference = (MAX_RTP_TIMESTAMP - pRtpPacket->header.timestamp) + pInternal->headTimestamp;
        }

        // 1 frame per second, and 1 packet per frame, the most charitable case we can consider
        // TODO track most recent FPS to improve this metric
        if ((MAX_RTP_TIMESTAMP / pInternal->base.clockRate) <= seqNoDifference) {
            maxTimePassed = MAX_RTP_TIMESTAMP;
        } else {
            maxTimePassed = pInternal->base.clockRate * seqNoDifference;
        }

        if (maxTimePassed >= timestampDifference) {
            retVal = TRUE;
        }
    }
    return retVal;
}

static BOOL headCheckingAllowed(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    if (!(pInternal->firstFrameProcessed) || pInternal->headTimestamp == pRtpPacket->header.timestamp) {
        retVal = TRUE;
    }
    return retVal;
}

// return true if pRtpPacket contains the head sequence number
static BOOL headSequenceNumberCheck(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    UINT16 minimumHead = 0;
    if (pInternal->headSequenceNumber >= MAX_OUT_OF_ORDER_PACKET_DIFFERENCE) {
        minimumHead = pInternal->headSequenceNumber - MAX_OUT_OF_ORDER_PACKET_DIFFERENCE;
    }

    if (pInternal->headSequenceNumber == pRtpPacket->header.sequenceNumber) {
        retVal = TRUE;
    } else if (headCheckingAllowed(pInternal, pRtpPacket)) {
        if (pInternal->sequenceNumberOverflowState) {
            if (pInternal->tailSequenceNumber < pRtpPacket->header.sequenceNumber &&
                pInternal->headSequenceNumber > pRtpPacket->header.sequenceNumber && pRtpPacket->header.sequenceNumber >= minimumHead) {
                pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
                retVal = TRUE;
            }
        } else {
            if (pRtpPacket->header.sequenceNumber < pInternal->headSequenceNumber) {
                if (pRtpPacket->header.sequenceNumber >= minimumHead) {
                    pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
                    retVal = TRUE;
                }
            }
        }
    }
    return retVal;
}

// return true if pRtpPacket contains a new tail sequence number
static BOOL tailSequenceNumberCheck(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    if (pInternal->tailSequenceNumber == pRtpPacket->header.sequenceNumber) {
        retVal = TRUE;
    } else if (pRtpPacket->header.sequenceNumber > pInternal->tailSequenceNumber &&
               (!pInternal->sequenceNumberOverflowState || pInternal->headSequenceNumber > pRtpPacket->header.sequenceNumber)) {
        retVal = TRUE;
        pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
    }
    return retVal;
}

// return true if sequence numbers are now overflowing
static BOOL enterSequenceNumberOverflowCheck(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL overflow = FALSE;
    BOOL underflow = FALSE;
    UINT16 packetsUntilOverflow = MAX_RTP_SEQUENCE_NUM - pInternal->tailSequenceNumber;

    if (!pInternal->sequenceNumberOverflowState) {
        // overflow case
        if (MAX_OUT_OF_ORDER_PACKET_DIFFERENCE >= packetsUntilOverflow) {
            if (pRtpPacket->header.sequenceNumber < pInternal->tailSequenceNumber &&
                pRtpPacket->header.sequenceNumber <= MAX_OUT_OF_ORDER_PACKET_DIFFERENCE - packetsUntilOverflow) {
                overflow = TRUE;
            }
        }
        // underflow case
        else if (headCheckingAllowed(pInternal, pRtpPacket)) {
            if (pInternal->headSequenceNumber < MAX_OUT_OF_ORDER_PACKET_DIFFERENCE) {
                if (pRtpPacket->header.sequenceNumber >= (MAX_UINT16 - (MAX_OUT_OF_ORDER_PACKET_DIFFERENCE - pInternal->headSequenceNumber))) {
                    if (underflowPossible(pInternal, pRtpPacket)) {
                        underflow = TRUE;
                    }
                }
            }
        }
    }
    if (overflow && underflow) {
        DLOGE("Critical underflow/overflow error in jitterbuffer");
    }
    if (overflow) {
        pInternal->sequenceNumberOverflowState = TRUE;
        pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
    }
    if (underflow) {
        pInternal->sequenceNumberOverflowState = TRUE;
        pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        pInternal->headTimestamp = pRtpPacket->header.timestamp;
    }
    return (overflow || underflow);
}

static BOOL enterTimestampOverflowCheck(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL underflow = FALSE;
    BOOL overflow = FALSE;
    if (!pInternal->timestampOverFlowState) {
        // overflow check
        if (pInternal->headTimestamp > pRtpPacket->header.timestamp && pInternal->base.tailTimestamp > pRtpPacket->header.timestamp) {
            if (tailSequenceNumberCheck(pInternal, pRtpPacket)) {
                overflow = TRUE;
            }
        }
        // underflow check
        else if (pInternal->headTimestamp < pRtpPacket->header.timestamp && pInternal->base.tailTimestamp < pRtpPacket->header.timestamp) {
            UINT16 prevHead = pInternal->headSequenceNumber;
            if (headSequenceNumberCheck(pInternal, pRtpPacket) && pInternal->headSequenceNumber != prevHead) {
                underflow = TRUE;
            }
        }
    }
    if (overflow && underflow) {
        DLOGE("Critical underflow/overflow error in jitterbuffer");
    }
    if (overflow) {
        pInternal->timestampOverFlowState = TRUE;
        pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
    } else if (underflow) {
        pInternal->timestampOverFlowState = TRUE;
        pInternal->headTimestamp = pRtpPacket->header.timestamp;
    }
    return (underflow || overflow);
}

static BOOL exitSequenceNumberOverflowCheck(PJitterBufferInternal pInternal)
{
    BOOL retVal = FALSE;
    if (pInternal->sequenceNumberOverflowState) {
        if (pInternal->headSequenceNumber <= pInternal->tailSequenceNumber) {
            pInternal->sequenceNumberOverflowState = FALSE;
            retVal = TRUE;
        }
    }
    return retVal;
}

static BOOL exitTimestampOverflowCheck(PJitterBufferInternal pInternal)
{
    BOOL retVal = FALSE;
    if (pInternal->timestampOverFlowState) {
        if (pInternal->headTimestamp <= pInternal->base.tailTimestamp) {
            pInternal->timestampOverFlowState = FALSE;
            retVal = TRUE;
        }
    }
    return retVal;
}

// return true if pRtpPacket contains a new head timestamp
static BOOL headTimestampCheck(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;

    if (headCheckingAllowed(pInternal, pRtpPacket)) {
        if (pInternal->timestampOverFlowState) {
            if (pInternal->headTimestamp > pRtpPacket->header.timestamp && pInternal->base.tailTimestamp < pRtpPacket->header.timestamp) {
                if (headSequenceNumberCheck(pInternal, pRtpPacket)) {
                    pInternal->headTimestamp = pRtpPacket->header.timestamp;
                    retVal = TRUE;
                }
            }
        } else {
            if (pInternal->headTimestamp > pRtpPacket->header.timestamp || pInternal->base.tailTimestamp < pRtpPacket->header.timestamp) {
                if (headSequenceNumberCheck(pInternal, pRtpPacket)) {
                    pInternal->headTimestamp = pRtpPacket->header.timestamp;
                    retVal = TRUE;
                }
            }
        }
    }
    return retVal;
}

// return true if pRtpPacket contains a new tail timestamp
static BOOL tailTimestampCheck(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;

    if (pInternal->base.tailTimestamp < pRtpPacket->header.timestamp) {
        if (!pInternal->timestampOverFlowState || pInternal->headTimestamp > pRtpPacket->header.timestamp) {
            if (tailSequenceNumberCheck(pInternal, pRtpPacket)) {
                pInternal->base.tailTimestamp = pRtpPacket->header.timestamp;
                retVal = TRUE;
            }
        }
    }
    return retVal;
}

// return true if pRtpPacket is within the latency tolerance
static BOOL withinLatencyTolerance(PJitterBufferInternal pInternal, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    UINT32 minimumTimestamp = 0;

    if (tailTimestampCheck(pInternal, pRtpPacket) || pInternal->base.tailTimestamp == pRtpPacket->header.timestamp) {
        retVal = TRUE;
    } else {
        if (pInternal->timestampOverFlowState) {
            if (pInternal->base.tailTimestamp < pInternal->maxLatency) {
                minimumTimestamp = MAX_RTP_TIMESTAMP - (pInternal->maxLatency - pInternal->base.tailTimestamp);
            }
            if (pRtpPacket->header.timestamp < pInternal->base.tailTimestamp || pRtpPacket->header.timestamp > pInternal->headTimestamp) {
                retVal = TRUE;
            } else if (pRtpPacket->header.timestamp >= minimumTimestamp) {
                retVal = TRUE;
            }
        } else {
            if ((pRtpPacket->header.timestamp < pInternal->maxLatency && pInternal->base.tailTimestamp <= pInternal->maxLatency) ||
                pRtpPacket->header.timestamp >= pInternal->base.tailTimestamp - pInternal->maxLatency) {
                retVal = TRUE;
            }
        }
    }
    return retVal;
}

//
// Default vtable implementations
//

static STATUS defaultGetPacket(PJitterBuffer pJitterBuffer, UINT16 seqNum, PRtpPacket* ppPacket)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 hashValue = 0;
    PJitterBufferInternal pInternal = (PJitterBufferInternal) pJitterBuffer;

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

static STATUS defaultFillPartialFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex,
                                          UINT16 endIndex)
{
    STATUS retStatus = STATUS_SUCCESS;
    PJitterBufferInternal pInternal = (PJitterBufferInternal) pJitterBuffer;
    UINT16 index;
    UINT64 hashValue = 0;
    PRtpPacket pPacket = NULL;
    PBYTE pCurPtrInFrame = pFrame;
    UINT32 partialFrameSize = 0;
    UINT32 filledSize = 0;
    BOOL hasEntry = FALSE;
    BOOL isFirstInFrame = TRUE;

    CHK(pInternal != NULL && pFilledSize != NULL, STATUS_NULL_ARG);

    for (index = startIndex; UINT16_DEC(index) != endIndex; index++) {
        if (STATUS_FAILED(hashTableContains(pInternal->pPkgBufferHashTable, index, &hasEntry))) {
            continue;
        }
        if (hasEntry) {
            if (STATUS_FAILED(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue))) {
                continue;
            }
            pPacket = (PRtpPacket) hashValue;
            if (pFrame != NULL) {
                partialFrameSize = frameSize - filledSize;
            } else {
                partialFrameSize = 0;
            }
            BOOL depayIsFirst = isFirstInFrame;
            if (STATUS_FAILED(
                    pInternal->depayPayloadFn(pPacket->payload, pPacket->payloadLength, pCurPtrInFrame, &partialFrameSize, &depayIsFirst))) {
                DLOGW("depayPayloadFn failed for packet index %u, skipping", index);
                continue;
            }
            if (pCurPtrInFrame != NULL) {
                pCurPtrInFrame += partialFrameSize;
            }
            filledSize += partialFrameSize;
            isFirstInFrame = FALSE;
        }
    }

CleanUp:
    if (pFilledSize != NULL) {
        *pFilledSize = filledSize;
    }
    return retStatus;
}

static STATUS defaultPush(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket, PBOOL pPacketDiscarded)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS, status = STATUS_SUCCESS;
    UINT64 hashValue = 0;
    PRtpPacket pCurPacket = NULL;
    PJitterBufferInternal pInternal = (PJitterBufferInternal) pJitterBuffer;

    CHK(pInternal != NULL && pRtpPacket != NULL, STATUS_NULL_ARG);

    if (!pInternal->started) {
        pInternal->started = TRUE;
        pInternal->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        pInternal->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        pInternal->headTimestamp = pRtpPacket->header.timestamp;
    }

    if (!enterSequenceNumberOverflowCheck(pInternal, pRtpPacket)) {
        tailSequenceNumberCheck(pInternal, pRtpPacket);
    } else {
        DLOGS("Entered sequenceNumber overflow state");
    }

    if (!enterTimestampOverflowCheck(pInternal, pRtpPacket)) {
        tailTimestampCheck(pInternal, pRtpPacket);
    } else {
        DLOGS("Entered timestamp overflow state");
    }

    if (withinLatencyTolerance(pInternal, pRtpPacket)) {
        status = hashTableGet(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, &hashValue);
        pCurPacket = (PRtpPacket) hashValue;
        if (STATUS_SUCCEEDED(status) && pCurPacket != NULL) {
            freeRtpPacket(&pCurPacket);
            CHK_STATUS(hashTableRemove(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber));
        }

        CHK_STATUS(hashTablePut(pInternal->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, (UINT64) pRtpPacket));

        if (headCheckingAllowed(pInternal, pRtpPacket)) {
            if (headTimestampCheck(pInternal, pRtpPacket)) {
                DLOGS("New jitterbuffer head timestamp");
            }
            if (headSequenceNumberCheck(pInternal, pRtpPacket)) {
                DLOGS("New jitterbuffer head sequenceNumber");
            }
        }

        DLOGS("jitterBufferPush get packet timestamp %lu seqNum %lu", pRtpPacket->header.timestamp, pRtpPacket->header.sequenceNumber);
    } else {
        freeRtpPacket(&pRtpPacket);
        if (pPacketDiscarded != NULL) {
            *pPacketDiscarded = TRUE;
        }
    }

    CHK_STATUS(jitterBufferInternalParse(pInternal, FALSE));

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

static STATUS jitterBufferInternalParse(PJitterBufferInternal pInternal, BOOL bufferClosed)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 index;
    UINT16 lastIndex;
    UINT32 earliestAllowedTimestamp = 0;
    BOOL isFrameDataContinuous = TRUE;
    BOOL headFrameIsContiguous = TRUE;
    UINT32 curTimestamp = 0;
    UINT16 startDropIndex = 0;
    UINT32 curFrameSize = 0;
    BOOL sizeCalcIsFirst = TRUE;
    UINT32 partialFrameSize = 0;
    UINT64 hashValue = 0;
    BOOL isStart = FALSE, containStartForEarliestFrame = FALSE, hasEntry = FALSE;
    UINT16 lastNonNullIndex = 0;
    UINT16 lastHeadFrameSeqNum = 0;
    BOOL seenHeadFramePacket = FALSE;
    UINT16 firstGapIndex = 0;
    BOOL sawGapSinceLastFrame = FALSE;
    BOOL headFrameEnded = FALSE;
    PRtpPacket pCurPacket = NULL;

    CHK(pInternal != NULL && pInternal->onFrameDroppedFn != NULL && pInternal->onFrameReadyFn != NULL, STATUS_NULL_ARG);
    CHK(pInternal->started, retStatus);

    if (pInternal->base.tailTimestamp > pInternal->maxLatency) {
        earliestAllowedTimestamp = pInternal->base.tailTimestamp - pInternal->maxLatency;
    }

    lastIndex = pInternal->tailSequenceNumber + 1;
    index = pInternal->headSequenceNumber;
    startDropIndex = index;
    for (; index != lastIndex; index++) {
        CHK_STATUS(hashTableContains(pInternal->pPkgBufferHashTable, index, &hasEntry));
        if (!hasEntry) {
            isFrameDataContinuous = FALSE;
            if (!sawGapSinceLastFrame) {
                firstGapIndex = index;
                sawGapSinceLastFrame = TRUE;
            }
            if (seenHeadFramePacket && !headFrameEnded) {
                headFrameIsContiguous = FALSE;
            }
            CHK(pInternal->headTimestamp < earliestAllowedTimestamp || bufferClosed, retStatus);
        } else {
            lastNonNullIndex = index;
            retStatus = hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue);
            if (retStatus == STATUS_HASH_KEY_NOT_PRESENT) {
                continue;
            } else {
                CHK_STATUS(retStatus);
            }
            pCurPacket = (PRtpPacket) hashValue;
            CHK(pCurPacket != NULL, STATUS_NULL_ARG);
            curTimestamp = pCurPacket->header.timestamp;

            if (curTimestamp == pInternal->headTimestamp) {
                lastHeadFrameSeqNum = index;
                seenHeadFramePacket = TRUE;
                if (pCurPacket->header.marker) {
                    headFrameEnded = TRUE;
                }
            }

            if (curTimestamp != pInternal->headTimestamp) {
                if (sawGapSinceLastFrame && seenHeadFramePacket) {
                    if (firstGapIndex <= lastHeadFrameSeqNum) {
                        headFrameIsContiguous = FALSE;
                    } else if (!headFrameEnded) {
                        headFrameIsContiguous = FALSE;
                    }
                }
                if (containStartForEarliestFrame && headFrameIsContiguous) {
                    CHK_STATUS(pInternal->onFrameReadyFn(pInternal->customData, startDropIndex, UINT16_DEC(index), curFrameSize));
                    CHK_STATUS(defaultDropBufferData((PJitterBuffer) pInternal, startDropIndex, UINT16_DEC(index), curTimestamp));
                    pInternal->firstFrameProcessed = TRUE;
                    startDropIndex = index;
                    containStartForEarliestFrame = FALSE;
                    headFrameIsContiguous = TRUE;
                    sawGapSinceLastFrame = FALSE;
                    lastHeadFrameSeqNum = index;
                    seenHeadFramePacket = TRUE;
                    headFrameEnded = pCurPacket->header.marker;
                } else if (seenHeadFramePacket && startDropIndex != index && (pInternal->headTimestamp < earliestAllowedTimestamp || bufferClosed)) {
                    pInternal->onFrameDroppedFn(pInternal->customData, startDropIndex, UINT16_DEC(index), pInternal->headTimestamp);
                    CHK_STATUS(defaultDropBufferData((PJitterBuffer) pInternal, startDropIndex, UINT16_DEC(index), curTimestamp));
                    pInternal->firstFrameProcessed = TRUE;
                    isFrameDataContinuous = TRUE;
                    headFrameIsContiguous = TRUE;
                    sawGapSinceLastFrame = FALSE;
                    lastHeadFrameSeqNum = index;
                    seenHeadFramePacket = TRUE;
                    headFrameEnded = pCurPacket->header.marker;
                    startDropIndex = index;
                } else if (seenHeadFramePacket) {
                    break;
                } else {
                    pInternal->headTimestamp = curTimestamp;
                    startDropIndex = index;
                    lastHeadFrameSeqNum = index;
                    seenHeadFramePacket = TRUE;
                    headFrameEnded = pCurPacket->header.marker;
                    headFrameIsContiguous = TRUE;
                    sawGapSinceLastFrame = FALSE;
                }
                curFrameSize = 0;
                sizeCalcIsFirst = TRUE;
            }

            isStart = sizeCalcIsFirst;
            CHK_STATUS(pInternal->depayPayloadFn(pCurPacket->payload, pCurPacket->payloadLength, NULL, &partialFrameSize, &isStart));
            curFrameSize += partialFrameSize;
            sizeCalcIsFirst = FALSE;
            if (isStart && pInternal->headTimestamp == curTimestamp) {
                containStartForEarliestFrame = TRUE;
            }

            if ((pInternal->firstFrameProcessed || pInternal->alwaysSinglePacketFrames) && curTimestamp == pInternal->headTimestamp &&
                pCurPacket->header.marker && containStartForEarliestFrame && headFrameIsContiguous) {
                CHK_STATUS(pInternal->onFrameReadyFn(pInternal->customData, startDropIndex, index, curFrameSize));
                CHK_STATUS(defaultDropBufferData((PJitterBuffer) pInternal, startDropIndex, index, curTimestamp));
                pInternal->firstFrameProcessed = TRUE;
                startDropIndex = index + 1;
                curFrameSize = 0;
                sizeCalcIsFirst = TRUE;
                break;
            }
        }
    }

    // Deal with last frame, we're force clearing the entire buffer.
    if (bufferClosed && curFrameSize > 0) {
        curFrameSize = 0;
        sizeCalcIsFirst = TRUE;
        hasEntry = TRUE;
        for (index = startDropIndex; UINT16_DEC(index) != lastNonNullIndex && hasEntry; index++) {
            CHK_STATUS(hashTableContains(pInternal->pPkgBufferHashTable, index, &hasEntry));
            if (hasEntry) {
                CHK_STATUS(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue));
                pCurPacket = (PRtpPacket) hashValue;
                isStart = sizeCalcIsFirst;
                CHK_STATUS(pInternal->depayPayloadFn(pCurPacket->payload, pCurPacket->payloadLength, NULL, &partialFrameSize, &isStart));
                curFrameSize += partialFrameSize;
                sizeCalcIsFirst = FALSE;
            }
        }

        if (UINT16_DEC(index) == lastNonNullIndex) {
            CHK_STATUS(pInternal->onFrameReadyFn(pInternal->customData, startDropIndex, lastNonNullIndex, curFrameSize));
            CHK_STATUS(defaultDropBufferData((PJitterBuffer) pInternal, startDropIndex, lastNonNullIndex, pInternal->headTimestamp));
        } else {
            CHK_STATUS(pInternal->onFrameDroppedFn(pInternal->customData, startDropIndex, UINT16_DEC(index), pInternal->headTimestamp));
            CHK_STATUS(defaultDropBufferData((PJitterBuffer) pInternal, startDropIndex, lastNonNullIndex, pInternal->headTimestamp));
        }
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

// Remove all packets containing sequence numbers between and including the startIndex and endIndex for the JitterBuffer.
static STATUS defaultDropBufferData(PJitterBuffer pJitterBuffer, UINT16 startIndex, UINT16 endIndex, UINT32 nextTimestamp)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 index = startIndex;
    UINT64 hashValue;
    PRtpPacket pCurPacket = NULL;
    BOOL hasEntry = FALSE;
    PJitterBufferInternal pInternal = (PJitterBufferInternal) pJitterBuffer;

    CHK(pInternal != NULL, STATUS_NULL_ARG);
    for (; UINT16_DEC(index) != endIndex; index++) {
        CHK_STATUS(hashTableContains(pInternal->pPkgBufferHashTable, index, &hasEntry));
        if (hasEntry) {
            CHK_STATUS(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue));
            pCurPacket = (PRtpPacket) hashValue;
            if (pCurPacket) {
                freeRtpPacket(&pCurPacket);
            }
            CHK_STATUS(hashTableRemove(pInternal->pPkgBufferHashTable, index));
        }
    }
    pInternal->headTimestamp = nextTimestamp;
    pInternal->headSequenceNumber = endIndex + 1;
    if (exitTimestampOverflowCheck(pInternal)) {
        DLOGS("Exited timestamp overflow state");
    }
    if (exitSequenceNumberOverflowCheck(pInternal)) {
        DLOGS("Exited sequenceNumber overflow state");
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

// Depay all packets containing sequence numbers between and including the startIndex and endIndex for the JitterBuffer.
static STATUS defaultFillFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex,
                                   UINT16 endIndex)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 index = startIndex;
    UINT64 hashValue;
    PRtpPacket pCurPacket = NULL;
    PBYTE pCurPtrInFrame = pFrame;
    UINT32 remainingFrameSize = frameSize;
    UINT32 partialFrameSize = 0;
    PJitterBufferInternal pInternal = (PJitterBufferInternal) pJitterBuffer;

    CHK(pInternal != NULL && pFrame != NULL && pFilledSize != NULL, STATUS_NULL_ARG);
    BOOL isFirstInFrame = TRUE;
    for (; UINT16_DEC(index) != endIndex; index++) {
        hashValue = 0;
        CHK_STATUS(hashTableGet(pInternal->pPkgBufferHashTable, index, &hashValue));
        pCurPacket = (PRtpPacket) hashValue;
        CHK(pCurPacket != NULL, STATUS_NULL_ARG);
        partialFrameSize = remainingFrameSize;
        CHK_STATUS(pInternal->depayPayloadFn(pCurPacket->payload, pCurPacket->payloadLength, pCurPtrInFrame, &partialFrameSize, &isFirstInFrame));
        pCurPtrInFrame += partialFrameSize;
        remainingFrameSize -= partialFrameSize;
        isFirstInFrame = FALSE;
    }

CleanUp:
    if (pFilledSize != NULL) {
        *pFilledSize = frameSize - remainingFrameSize;
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

static STATUS defaultDestroy(PJitterBuffer* ppJitterBuffer)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PJitterBufferInternal pInternal = NULL;

    CHK(ppJitterBuffer != NULL, STATUS_NULL_ARG);
    CHK(*ppJitterBuffer != NULL, retStatus);

    pInternal = (PJitterBufferInternal) *ppJitterBuffer;

    // Parse repeatedly until all frames are processed.
    if (pInternal->started) {
        UINT16 prevHead = pInternal->headSequenceNumber;
        UINT32 maxIterations = 65536;
        while (maxIterations-- > 0) {
            jitterBufferInternalParse(pInternal, TRUE);
            if (pInternal->headSequenceNumber == prevHead) {
                break;
            }
            INT32 remaining = (INT32) ((UINT16) (pInternal->tailSequenceNumber - pInternal->headSequenceNumber + 1));
            if (remaining <= 0) {
                break;
            }
            prevHead = pInternal->headSequenceNumber;
        }
        defaultDropBufferData((PJitterBuffer) pInternal, pInternal->headSequenceNumber, pInternal->tailSequenceNumber, 0);
    }
    hashTableFree(pInternal->pPkgBufferHashTable);

    SAFE_MEMFREE(*ppJitterBuffer);

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}
