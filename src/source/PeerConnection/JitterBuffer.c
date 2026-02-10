#define LOG_CLASS "JitterBuffer"

#include "../Include_i.h"

// Applies only to the case where the very first frame has its first packets out of order
#define MAX_OUT_OF_ORDER_PACKET_DIFFERENCE 512

// forward declaration
STATUS jitterBufferInternalParse(PJitterBuffer pJitterBuffer, BOOL bufferClosed);

STATUS createJitterBuffer(FrameReadyFunc onFrameReadyFunc, FrameDroppedFunc onFrameDroppedFunc, DepayRtpPayloadFunc depayRtpPayloadFunc,
                          UINT32 maxLatency, UINT32 clockRate, UINT64 customData, PJitterBuffer* ppJitterBuffer)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PJitterBuffer pJitterBuffer = NULL;

    CHK(ppJitterBuffer != NULL && onFrameReadyFunc != NULL && onFrameDroppedFunc != NULL && depayRtpPayloadFunc != NULL, STATUS_NULL_ARG);
    CHK(clockRate != 0, STATUS_INVALID_ARG);

    pJitterBuffer = (PJitterBuffer) MEMALLOC(SIZEOF(JitterBuffer));
    CHK(pJitterBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pJitterBuffer->onFrameReadyFn = onFrameReadyFunc;
    pJitterBuffer->onFrameDroppedFn = onFrameDroppedFunc;
    pJitterBuffer->depayPayloadFn = depayRtpPayloadFunc;
    pJitterBuffer->clockRate = clockRate;

    pJitterBuffer->maxLatency = maxLatency;
    if (pJitterBuffer->maxLatency == 0) {
        pJitterBuffer->maxLatency = DEFAULT_JITTER_BUFFER_MAX_LATENCY;
    }
    pJitterBuffer->maxLatency = pJitterBuffer->maxLatency * pJitterBuffer->clockRate / HUNDREDS_OF_NANOS_IN_A_SECOND;

    CHK(pJitterBuffer->maxLatency < MAX_RTP_TIMESTAMP, STATUS_INVALID_ARG);

    pJitterBuffer->tailTimestamp = 0;
    pJitterBuffer->headTimestamp = MAX_UINT32;
    pJitterBuffer->headSequenceNumber = MAX_RTP_SEQUENCE_NUM;
    pJitterBuffer->tailSequenceNumber = MAX_RTP_SEQUENCE_NUM;
    pJitterBuffer->started = FALSE;
    pJitterBuffer->firstFrameProcessed = FALSE;
    pJitterBuffer->timestampOverFlowState = FALSE;
    pJitterBuffer->sequenceNumberOverflowState = FALSE;

    pJitterBuffer->customData = customData;
    CHK_STATUS(hashTableCreateWithParams(JITTER_BUFFER_HASH_TABLE_BUCKET_COUNT, JITTER_BUFFER_HASH_TABLE_BUCKET_LENGTH,
                                         &pJitterBuffer->pPkgBufferHashTable));

CleanUp:
    if (STATUS_FAILED(retStatus) && pJitterBuffer != NULL) {
        freeJitterBuffer(&pJitterBuffer);
        pJitterBuffer = NULL;
    }

    if (ppJitterBuffer != NULL) {
        *ppJitterBuffer = pJitterBuffer;
    }

    LEAVES();
    return retStatus;
}

STATUS freeJitterBuffer(PJitterBuffer* ppJitterBuffer)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PJitterBuffer pJitterBuffer = NULL;

    CHK(ppJitterBuffer != NULL, STATUS_NULL_ARG);
    // freeJitterBuffer is idempotent
    CHK(*ppJitterBuffer != NULL, retStatus);

    pJitterBuffer = *ppJitterBuffer;

    // Parse repeatedly until all frames are processed.
    // After marker bit delivery, the parse breaks early, so we need to loop
    // to ensure all remaining frames are delivered/dropped.
    if (pJitterBuffer->started) {
        UINT16 prevHead = pJitterBuffer->headSequenceNumber;
        UINT32 maxIterations = 65536; // Safety limit to prevent infinite loops
        while (maxIterations-- > 0) {
            jitterBufferInternalParse(pJitterBuffer, TRUE);
            // If head didn't advance, we're done or stuck
            if (pJitterBuffer->headSequenceNumber == prevHead) {
                break;
            }
            // If we've processed everything, we're done
            // Use signed comparison to handle wraparound
            INT32 remaining = (INT32) ((UINT16) (pJitterBuffer->tailSequenceNumber - pJitterBuffer->headSequenceNumber + 1));
            if (remaining <= 0) {
                break;
            }
            prevHead = pJitterBuffer->headSequenceNumber;
        }
        // Clean up any remaining packets that weren't delivered/dropped
        jitterBufferDropBufferData(pJitterBuffer, pJitterBuffer->headSequenceNumber, pJitterBuffer->tailSequenceNumber, 0);
    }
    hashTableFree(pJitterBuffer->pPkgBufferHashTable);

    SAFE_MEMFREE(*ppJitterBuffer);

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

BOOL underflowPossible(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    UINT32 seqNoDifference = 0;
    UINT64 timestampDifference = 0;
    UINT64 maxTimePassed = 0;
    if (pJitterBuffer->headTimestamp == pRtpPacket->header.timestamp) {
        retVal = TRUE;
    } else {
        seqNoDifference = (MAX_RTP_SEQUENCE_NUM - pRtpPacket->header.sequenceNumber) + pJitterBuffer->headSequenceNumber;
        if (pJitterBuffer->headTimestamp > pRtpPacket->header.timestamp) {
            timestampDifference = pJitterBuffer->headTimestamp - pRtpPacket->header.timestamp;
        } else {
            timestampDifference = (MAX_RTP_TIMESTAMP - pRtpPacket->header.timestamp) + pJitterBuffer->headTimestamp;
        }

        // 1 frame per second, and 1 packet per frame, the most charitable case we can consider
        // TODO track most recent FPS to improve this metric
        if ((MAX_RTP_TIMESTAMP / pJitterBuffer->clockRate) <= seqNoDifference) {
            maxTimePassed = MAX_RTP_TIMESTAMP;
        } else {
            maxTimePassed = pJitterBuffer->clockRate * seqNoDifference;
        }

        if (maxTimePassed >= timestampDifference) {
            retVal = TRUE;
        }
    }
    return retVal;
}

BOOL headCheckingAllowed(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    /*If we haven't yet processed a frame yet, then we don't have a definitive way of knowing if
     *the first packet we receive is actually the earliest packet we'll ever receive. Since sequence numbers
     *can start anywhere from 0 - 65535, we need to incorporate some checks to determine if a newly received packet
     *should be considered the new head. Part of how we determine this is by setting a limit to how many packets off we allow
     *this out of order case to be. Without setting a limit, then we could run into an odd scenario.
     * Example:
     * Push->Packet->SeqNumber == 0. //FIRST PACKET! new head of buffer!
     * Push->Packet->SeqNumber == 3. //... new head of 65532 packet sized frame? maybe? was 0 the tail?
     *
     * To resolve that insanity we set a MAX, and will use that MAX for the range.
     * This logic is present in headSequenceNumberCheck()
     *
     *After the first frame has been processed we don't need or want to make this consideration, since if our parser has
     *dropped a frame for a good reason then we want to ignore any packets from that dropped frame that may come later.
     *
     *However if the packet's timestamp is the same as the head timestamp, then it's possible this is simply an earlier
     *sequence number of the same packet.
     */
    if (!(pJitterBuffer->firstFrameProcessed) || pJitterBuffer->headTimestamp == pRtpPacket->header.timestamp) {
        retVal = TRUE;
    }
    return retVal;
}

// return true if pRtpPacket contains the head sequence number
BOOL headSequenceNumberCheck(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    UINT16 minimumHead = 0;
    if (pJitterBuffer->headSequenceNumber >= MAX_OUT_OF_ORDER_PACKET_DIFFERENCE) {
        minimumHead = pJitterBuffer->headSequenceNumber - MAX_OUT_OF_ORDER_PACKET_DIFFERENCE;
    }

    // If we've already done this check and it was true
    if (pJitterBuffer->headSequenceNumber == pRtpPacket->header.sequenceNumber) {
        retVal = TRUE;
    } else if (headCheckingAllowed(pJitterBuffer, pRtpPacket)) {
        if (pJitterBuffer->sequenceNumberOverflowState) {
            if (pJitterBuffer->tailSequenceNumber < pRtpPacket->header.sequenceNumber &&
                pJitterBuffer->headSequenceNumber > pRtpPacket->header.sequenceNumber && pRtpPacket->header.sequenceNumber >= minimumHead) {
                // This purposefully misses the usecase where the buffer has >65000 entries.
                // Our buffer is not designed for that use case, and it becomes far too ambiguous
                // as to which packets are new tails or new heads without adding epoch checks.
                pJitterBuffer->headSequenceNumber = pRtpPacket->header.sequenceNumber;
                retVal = TRUE;
            }
        } else {
            if (pRtpPacket->header.sequenceNumber < pJitterBuffer->headSequenceNumber) {
                if (pRtpPacket->header.sequenceNumber >= minimumHead) {
                    pJitterBuffer->headSequenceNumber = pRtpPacket->header.sequenceNumber;
                    retVal = TRUE;
                }
            }
        }
    }
    return retVal;
}

// return true if pRtpPacket contains a new tail sequence number
BOOL tailSequenceNumberCheck(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    // If we've already done this check and it was true
    if (pJitterBuffer->tailSequenceNumber == pRtpPacket->header.sequenceNumber) {
        retVal = TRUE;
    } else if (pRtpPacket->header.sequenceNumber > pJitterBuffer->tailSequenceNumber &&
               (!pJitterBuffer->sequenceNumberOverflowState || pJitterBuffer->headSequenceNumber > pRtpPacket->header.sequenceNumber)) {
        retVal = TRUE;
        pJitterBuffer->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
    }
    return retVal;
}

// return true if sequence numbers are now overflowing
BOOL enterSequenceNumberOverflowCheck(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL overflow = FALSE;
    BOOL underflow = FALSE;
    UINT16 packetsUntilOverflow = MAX_RTP_SEQUENCE_NUM - pJitterBuffer->tailSequenceNumber;

    if (!pJitterBuffer->sequenceNumberOverflowState) {
        // overflow case
        if (MAX_OUT_OF_ORDER_PACKET_DIFFERENCE >= packetsUntilOverflow) {
            // It's possible sequence numbers and timestamps are both overflowing.
            if (pRtpPacket->header.sequenceNumber < pJitterBuffer->tailSequenceNumber &&
                pRtpPacket->header.sequenceNumber <= MAX_OUT_OF_ORDER_PACKET_DIFFERENCE - packetsUntilOverflow) {
                // Sequence number overflow detected
                overflow = TRUE;
            }
        }
        // underflow case
        else if (headCheckingAllowed(pJitterBuffer, pRtpPacket)) {
            if (pJitterBuffer->headSequenceNumber < MAX_OUT_OF_ORDER_PACKET_DIFFERENCE) {
                if (pRtpPacket->header.sequenceNumber >= (MAX_UINT16 - (MAX_OUT_OF_ORDER_PACKET_DIFFERENCE - pJitterBuffer->headSequenceNumber))) {
                    // Possible sequence number underflow detected, now lets check the timestamps to be certain
                    // this is an earlier value, and not a much later.
                    if (underflowPossible(pJitterBuffer, pRtpPacket)) {
                        underflow = TRUE;
                    }
                }
            }
        }
    }
    if (overflow && underflow) {
        // This shouldn't be possible.
        DLOGE("Critical underflow/overflow error in jitterbuffer");
    }
    if (overflow) {
        pJitterBuffer->sequenceNumberOverflowState = TRUE;
        pJitterBuffer->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        pJitterBuffer->tailTimestamp = pRtpPacket->header.timestamp;
    }
    if (underflow) {
        pJitterBuffer->sequenceNumberOverflowState = TRUE;
        pJitterBuffer->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        pJitterBuffer->headTimestamp = pRtpPacket->header.timestamp;
    }
    return (overflow || underflow);
}

BOOL enterTimestampOverflowCheck(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL underflow = FALSE;
    BOOL overflow = FALSE;
    if (!pJitterBuffer->timestampOverFlowState) {
        // overflow check
        if (pJitterBuffer->headTimestamp > pRtpPacket->header.timestamp && pJitterBuffer->tailTimestamp > pRtpPacket->header.timestamp) {
            // Check to see if this could be a timestamp overflow case
            // We always check sequence number first, so the 'or equal to' checks if we just set the tail.
            // That would be a corner case of sequence number and timestamp both overflowing
            // in this one packet.
            if (tailSequenceNumberCheck(pJitterBuffer, pRtpPacket)) {
                // RTP timestamp overflow detected!
                overflow = TRUE;
            }
        }
        // underflow check
        else if (pJitterBuffer->headTimestamp < pRtpPacket->header.timestamp && pJitterBuffer->tailTimestamp < pRtpPacket->header.timestamp) {
            // Only detect underflow if headSequenceNumberCheck actually updates the head (meaning packet is EARLIER than expected).
            // Don't trigger underflow if packet is exactly at headSequenceNumber (that's the expected next packet, not underflow).
            UINT16 prevHead = pJitterBuffer->headSequenceNumber;
            if (headSequenceNumberCheck(pJitterBuffer, pRtpPacket) && pJitterBuffer->headSequenceNumber != prevHead) {
                underflow = TRUE;
            }
        }
    }
    if (overflow && underflow) {
        // This shouldn't be possible.
        DLOGE("Critical underflow/overflow error in jitterbuffer");
    }
    if (overflow) {
        pJitterBuffer->timestampOverFlowState = TRUE;
        pJitterBuffer->tailTimestamp = pRtpPacket->header.timestamp;
    } else if (underflow) {
        pJitterBuffer->timestampOverFlowState = TRUE;
        pJitterBuffer->headTimestamp = pRtpPacket->header.timestamp;
    }
    return (underflow || overflow);
}

BOOL exitSequenceNumberOverflowCheck(PJitterBuffer pJitterBuffer)
{
    BOOL retVal = FALSE;

    // can't exit if you're not in it
    if (pJitterBuffer->sequenceNumberOverflowState) {
        if (pJitterBuffer->headSequenceNumber <= pJitterBuffer->tailSequenceNumber) {
            pJitterBuffer->sequenceNumberOverflowState = FALSE;
            retVal = TRUE;
        }
    }

    return retVal;
}

BOOL exitTimestampOverflowCheck(PJitterBuffer pJitterBuffer)
{
    BOOL retVal = FALSE;

    // can't exit if you're not in it
    if (pJitterBuffer->timestampOverFlowState) {
        if (pJitterBuffer->headTimestamp <= pJitterBuffer->tailTimestamp) {
            pJitterBuffer->timestampOverFlowState = FALSE;
            retVal = TRUE;
        }
    }

    return retVal;
}

// return true if pRtpPacket contains a new head timestamp
BOOL headTimestampCheck(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;

    if (headCheckingAllowed(pJitterBuffer, pRtpPacket)) {
        if (pJitterBuffer->timestampOverFlowState) {
            if (pJitterBuffer->headTimestamp > pRtpPacket->header.timestamp && pJitterBuffer->tailTimestamp < pRtpPacket->header.timestamp) {
                // in the correct range to be a new head or new tail.
                // if it's also the head sequence number then it's the new headtimestamp
                if (headSequenceNumberCheck(pJitterBuffer, pRtpPacket)) {
                    pJitterBuffer->headTimestamp = pRtpPacket->header.timestamp;
                    retVal = TRUE;
                }
            }
        } else {
            if (pJitterBuffer->headTimestamp > pRtpPacket->header.timestamp || pJitterBuffer->tailTimestamp < pRtpPacket->header.timestamp) {
                // in the correct range to be a new head or new tail.
                // if it's also the head sequence number then it's the new headtimestamp
                if (headSequenceNumberCheck(pJitterBuffer, pRtpPacket)) {
                    pJitterBuffer->headTimestamp = pRtpPacket->header.timestamp;
                    retVal = TRUE;
                }
            }
        }
    }
    return retVal;
}

// return true if pRtpPacket contains a new tail timestamp
BOOL tailTimestampCheck(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;

    if (pJitterBuffer->tailTimestamp < pRtpPacket->header.timestamp) {
        if (!pJitterBuffer->timestampOverFlowState || pJitterBuffer->headTimestamp > pRtpPacket->header.timestamp) {
            // in the correct range to be a new head or new tail.
            // if it's also the tail sequence number then it's the new tail timestamp
            if (tailSequenceNumberCheck(pJitterBuffer, pRtpPacket)) {
                pJitterBuffer->tailTimestamp = pRtpPacket->header.timestamp;
                retVal = TRUE;
            }
        }
    }
    return retVal;
}

// return true if pRtpPacket is within the latency tolerance (not much earlier than current head)
BOOL withinLatencyTolerance(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket)
{
    BOOL retVal = FALSE;
    UINT32 minimumTimestamp = 0;

    // Simple check, if we're at or past the tail timestamp then we're always within latency tolerance.
    // overflow is checked earlier
    if (tailTimestampCheck(pJitterBuffer, pRtpPacket) || pJitterBuffer->tailTimestamp == pRtpPacket->header.timestamp) {
        retVal = TRUE;
    } else {
        // Is our tail current less than our head due to timestamp overflow?
        if (pJitterBuffer->timestampOverFlowState) {
            // calculate max-latency across the overflow boundry without triggering underflow
            if (pJitterBuffer->tailTimestamp < pJitterBuffer->maxLatency) {
                minimumTimestamp = MAX_RTP_TIMESTAMP - (pJitterBuffer->maxLatency - pJitterBuffer->tailTimestamp);
            }
            // Is the packet within the current range or is it a new head/tail
            if (pRtpPacket->header.timestamp < pJitterBuffer->tailTimestamp || pRtpPacket->header.timestamp > pJitterBuffer->headTimestamp) {
                // The packet is within the current range
                retVal = TRUE;
            }
            // The only remaining option is that timestamp must be before headTimestamp
            else if (pRtpPacket->header.timestamp >= minimumTimestamp) {
                retVal = TRUE;
            }
        } else {
            if ((pRtpPacket->header.timestamp < pJitterBuffer->maxLatency && pJitterBuffer->tailTimestamp <= pJitterBuffer->maxLatency) ||
                pRtpPacket->header.timestamp >= pJitterBuffer->tailTimestamp - pJitterBuffer->maxLatency) {
                retVal = TRUE;
            }
        }
    }
    return retVal;
}

STATUS jitterBufferPush(PJitterBuffer pJitterBuffer, PRtpPacket pRtpPacket, PBOOL pPacketDiscarded)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS, status = STATUS_SUCCESS;
    UINT64 hashValue = 0;
    PRtpPacket pCurPacket = NULL;

    CHK(pJitterBuffer != NULL && pRtpPacket != NULL, STATUS_NULL_ARG);

    if (!pJitterBuffer->started) {
        // Set to started and initialize the sequence number
        pJitterBuffer->started = TRUE;
        pJitterBuffer->headSequenceNumber = pRtpPacket->header.sequenceNumber;
        pJitterBuffer->tailSequenceNumber = pRtpPacket->header.sequenceNumber;
        pJitterBuffer->headTimestamp = pRtpPacket->header.timestamp;
    }

    // We'll check sequence numbers first, with our MAX Out of Order packet count to avoid
    // defining a timestamp window for overflow
    // Returning true means this packet is a new tail AND we've entered overflow state.
    if (!enterSequenceNumberOverflowCheck(pJitterBuffer, pRtpPacket)) {
        tailSequenceNumberCheck(pJitterBuffer, pRtpPacket);
    } else {
        DLOGS("Entered sequenceNumber overflow state");
    }

    if (!enterTimestampOverflowCheck(pJitterBuffer, pRtpPacket)) {
        tailTimestampCheck(pJitterBuffer, pRtpPacket);
    } else {
        DLOGS("Entered timestamp overflow state");
    }

    // is the packet within the accepted latency range, if so, add it to the hashtable
    if (withinLatencyTolerance(pJitterBuffer, pRtpPacket)) {
        status = hashTableGet(pJitterBuffer->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, &hashValue);
        pCurPacket = (PRtpPacket) hashValue;
        if (STATUS_SUCCEEDED(status) && pCurPacket != NULL) {
            freeRtpPacket(&pCurPacket);
            CHK_STATUS(hashTableRemove(pJitterBuffer->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber));
        }

        CHK_STATUS(hashTablePut(pJitterBuffer->pPkgBufferHashTable, pRtpPacket->header.sequenceNumber, (UINT64) pRtpPacket));

        if (headCheckingAllowed(pJitterBuffer, pRtpPacket)) {
            // if the timestamp is less, we'll accept it as a new head, since it must be an earlier frame.
            if (headTimestampCheck(pJitterBuffer, pRtpPacket)) {
                DLOGS("New jitterbuffer head timestamp");
            }
            if (headSequenceNumberCheck(pJitterBuffer, pRtpPacket)) {
                DLOGS("New jitterbuffer head sequenceNumber");
            }
        }
        // DONE with considering the head.

        DLOGS("jitterBufferPush get packet timestamp %lu seqNum %lu", pRtpPacket->header.timestamp, pRtpPacket->header.sequenceNumber);
    } else {
        // Free the packet if it is out of range, jitter buffer need to own the packet and do free
        freeRtpPacket(&pRtpPacket);
        if (pPacketDiscarded != NULL) {
            *pPacketDiscarded = TRUE;
        }
    }

    CHK_STATUS(jitterBufferInternalParse(pJitterBuffer, FALSE));

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS jitterBufferInternalParse(PJitterBuffer pJitterBuffer, BOOL bufferClosed)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 index;
    UINT16 lastIndex;
    UINT32 earliestAllowedTimestamp = 0;
    BOOL isFrameDataContinuous = TRUE;
    BOOL headFrameIsContiguous = TRUE; // Track continuity for head frame only
    UINT32 curTimestamp = 0;
    UINT16 startDropIndex = 0;
    UINT32 curFrameSize = 0;
    BOOL sizeCalcIsFirst = TRUE; // tracks first packet in frame for start code size calculation
    UINT32 partialFrameSize = 0;
    UINT64 hashValue = 0;
    BOOL isStart = FALSE, containStartForEarliestFrame = FALSE, hasEntry = FALSE;
    UINT16 lastNonNullIndex = 0;
    UINT16 lastHeadFrameSeqNum = 0;    // Last seq number seen with head timestamp
    BOOL seenHeadFramePacket = FALSE;  // Whether we've seen any packet from head frame
    UINT16 firstGapIndex = 0;          // First gap sequence number since last frame boundary
    BOOL sawGapSinceLastFrame = FALSE; // Whether we've seen any gap since last frame boundary
    BOOL headFrameEnded = FALSE;       // Whether head frame's last packet had marker bit (complete frame)
    PRtpPacket pCurPacket = NULL;

    CHK(pJitterBuffer != NULL && pJitterBuffer->onFrameDroppedFn != NULL && pJitterBuffer->onFrameReadyFn != NULL, STATUS_NULL_ARG);
    CHK(pJitterBuffer->tailTimestamp != 0, retStatus);

    if (pJitterBuffer->tailTimestamp > pJitterBuffer->maxLatency) {
        earliestAllowedTimestamp = pJitterBuffer->tailTimestamp - pJitterBuffer->maxLatency;
    }

    lastIndex = pJitterBuffer->tailSequenceNumber + 1;
    index = pJitterBuffer->headSequenceNumber;
    startDropIndex = index;
    // Loop through entire buffer to find complete frames.
    /*A Frame is ready when these conditions are met:
     * 1. We have a starting packet
     * 2. There were no missing sequence numbers up to this point
     * 3. A different timestamp in a sequential packet was found
     * 4. There are no earlier frames still in the buffer
     *
     *A Frame is dropped when the above conditions are not met, and the following conditions have been:
     * 1. the buffer is being closed
     * 2. The time between the most recently pushed RTP packet and oldest stored packet has surpassed the
     *    maximum allowed latency
     *
     *The buffer is parsed in order of sequence numbers. It is important to note that if the Frame ready
     *conditions have been met from dropping an earlier frame, then it will be processed.
     */
    for (; index != lastIndex; index++) {
        CHK_STATUS(hashTableContains(pJitterBuffer->pPkgBufferHashTable, index, &hasEntry));
        if (!hasEntry) {
            isFrameDataContinuous = FALSE;
            // Track where first gap is found - we'll determine at frame boundary if it's in head frame
            if (!sawGapSinceLastFrame) {
                firstGapIndex = index;
                sawGapSinceLastFrame = TRUE;
            }
            // If we've seen head frame packets but not the marker bit yet, this gap might be in the head frame.
            // Set headFrameIsContiguous=FALSE proactively to prevent incorrect marker bit delivery.
            // If the gap turns out to be in a later frame, headFrameIsContiguous will be reset at frame boundary.
            if (seenHeadFramePacket && !headFrameEnded) {
                headFrameIsContiguous = FALSE;
            }
            // if the max latency has not been reached, or the buffer is not being closed, exit parse when a missing entry is found
            CHK(pJitterBuffer->headTimestamp < earliestAllowedTimestamp || bufferClosed, retStatus);
        } else {
            lastNonNullIndex = index;
            retStatus = hashTableGet(pJitterBuffer->pPkgBufferHashTable, index, &hashValue);
            if (retStatus == STATUS_HASH_KEY_NOT_PRESENT) {
                // should be unreachable, this means hashTablContains() said we had it but hashTableGet() couldn't find it.
                continue;
            } else {
                CHK_STATUS(retStatus);
            }
            pCurPacket = (PRtpPacket) hashValue;
            CHK(pCurPacket != NULL, STATUS_NULL_ARG);
            curTimestamp = pCurPacket->header.timestamp;

            // Track packets belonging to the head frame
            if (curTimestamp == pJitterBuffer->headTimestamp) {
                lastHeadFrameSeqNum = index;
                seenHeadFramePacket = TRUE;
                // Track if this packet has marker bit (indicates end of frame)
                if (pCurPacket->header.marker) {
                    headFrameEnded = TRUE;
                }
            }

            // new timestamp on an RTP packet means new frame
            if (curTimestamp != pJitterBuffer->headTimestamp) {
                // Determine if head frame is contiguous by checking if any gaps are within its range
                // Key insight: If head frame's last packet had marker bit, the frame is complete.
                // Any gap AFTER that is in the next frame, not the head frame.
                if (sawGapSinceLastFrame && seenHeadFramePacket) {
                    // Only evaluate continuity if we've actually seen head frame packets.
                    // If seenHeadFramePacket=FALSE, the head frame was already delivered via marker
                    // and any gaps are in inter-frame space, not the head frame.
                    if (firstGapIndex <= lastHeadFrameSeqNum) {
                        // Gap is within head frame's known packet range
                        headFrameIsContiguous = FALSE;
                    } else if (!headFrameEnded) {
                        // Gap is after last seen head packet, but head frame didn't have marker bit
                        // The gap might still be in the head frame (frame not complete)
                        headFrameIsContiguous = FALSE;
                    }
                    // else: gap is AFTER head frame's marker bit, so it's in next frame
                }
                // was previous frame complete? Deliver it
                if (containStartForEarliestFrame && headFrameIsContiguous) {
                    // Decrement the index because this is an inclusive end parser, and we don't want to include the current index in the processed
                    // frame.
                    CHK_STATUS(pJitterBuffer->onFrameReadyFn(pJitterBuffer->customData, startDropIndex, UINT16_DEC(index), curFrameSize));
                    CHK_STATUS(jitterBufferDropBufferData(pJitterBuffer, startDropIndex, UINT16_DEC(index), curTimestamp));
                    pJitterBuffer->firstFrameProcessed = TRUE;
                    startDropIndex = index;
                    containStartForEarliestFrame = FALSE;
                    // Reset tracking for the new head frame
                    headFrameIsContiguous = TRUE;
                    sawGapSinceLastFrame = FALSE;
                    // Track the current packet as the new head frame's first packet
                    // (headTimestamp was just updated to curTimestamp by jitterBufferDropBufferData)
                    lastHeadFrameSeqNum = index;
                    seenHeadFramePacket = TRUE;
                    headFrameEnded = pCurPacket->header.marker;
                }
                // are we forcibly clearing out the buffer? if so drop the contents of incomplete frame
                // Only force clear if:
                // 1. We've seen head frame packets (seenHeadFramePacket) - otherwise there's nothing to drop
                // 2. There are packets to drop (startDropIndex != index)
                // If seenHeadFramePacket=FALSE, the head frame was already delivered via marker bit
                // and we should go to the else block to reset state for the new frame.
                else if (seenHeadFramePacket && startDropIndex != index &&
                         (pJitterBuffer->headTimestamp < earliestAllowedTimestamp || bufferClosed)) {
                    // do not CHK_STATUS of onFrameDropped because we need to clear the jitter buffer no matter what else happens.
                    pJitterBuffer->onFrameDroppedFn(pJitterBuffer->customData, startDropIndex, UINT16_DEC(index), pJitterBuffer->headTimestamp);
                    CHK_STATUS(jitterBufferDropBufferData(pJitterBuffer, startDropIndex, UINT16_DEC(index), curTimestamp));
                    pJitterBuffer->firstFrameProcessed = TRUE;
                    isFrameDataContinuous = TRUE;
                    // Reset tracking for the new head frame
                    headFrameIsContiguous = TRUE;
                    sawGapSinceLastFrame = FALSE;
                    // Track the current packet as the new head frame's first packet
                    // (headTimestamp was just updated to curTimestamp by jitterBufferDropBufferData)
                    lastHeadFrameSeqNum = index;
                    seenHeadFramePacket = TRUE;
                    headFrameEnded = pCurPacket->header.marker;
                    startDropIndex = index;
                } else if (seenHeadFramePacket) {
                    // if you're here, it means we're not force clearing the buffer, and the previous frame must be missing its starting packet.
                    // The starting packet isn't going to be found at an incremental sequence number, so we can save some time and break here.
                    break;
                } else {
                    // No head frame packets were seen - the head frame was likely already delivered via marker bit.
                    // Update state for the new frame and continue processing.
                    pJitterBuffer->headTimestamp = curTimestamp;
                    startDropIndex = index;
                    // Track the current packet as the new head frame's first packet
                    lastHeadFrameSeqNum = index;
                    seenHeadFramePacket = TRUE;
                    headFrameEnded = pCurPacket->header.marker;
                    headFrameIsContiguous = TRUE;
                    sawGapSinceLastFrame = FALSE;
                }
                // new timestamp means new frame, drop tracking for previous frame size
                curFrameSize = 0;
                sizeCalcIsFirst = TRUE;
            }

            // Size-only call: pass sizeCalcIsFirst as input so start code size matches jitterBufferFillFrameData.
            // The depayloader reads *pIsStart for start code choice, then overwrites with isStartingPacket output.
            isStart = sizeCalcIsFirst;
            CHK_STATUS(pJitterBuffer->depayPayloadFn(pCurPacket->payload, pCurPacket->payloadLength, NULL, &partialFrameSize, &isStart));
            curFrameSize += partialFrameSize;
            sizeCalcIsFirst = FALSE;
            if (isStart && pJitterBuffer->headTimestamp == curTimestamp) {
                containStartForEarliestFrame = TRUE;
            }

            // Immediate marker bit delivery: if we have a complete frame (start + marker + no gaps), deliver now
            // This reduces latency by not waiting for the next frame's first packet
            // Use headFrameIsContiguous (per-frame tracking) instead of isFrameDataContinuous (global) for consistency
            // with the frame boundary delivery logic
            if (curTimestamp == pJitterBuffer->headTimestamp && pCurPacket->header.marker && containStartForEarliestFrame && headFrameIsContiguous) {
                // Frame is complete: has start, has marker, all packets contiguous
                CHK_STATUS(pJitterBuffer->onFrameReadyFn(pJitterBuffer->customData, startDropIndex, index, curFrameSize));
                CHK_STATUS(jitterBufferDropBufferData(pJitterBuffer, startDropIndex, index, curTimestamp));
                pJitterBuffer->firstFrameProcessed = TRUE;
                // Update startDropIndex so the bufferClosed block doesn't try to re-deliver
                startDropIndex = index + 1;
                curFrameSize = 0;
                sizeCalcIsFirst = TRUE;
                // Note: jitterBufferDropBufferData sets headTimestamp to curTimestamp (delivered frame's timestamp)
                // since we don't know the next frame's timestamp yet. Break here and let the next parse
                // correctly detect the frame boundary via the else branch at lines 606-617.
                // When buffer is being closed, freeJitterBuffer will call parse repeatedly until all frames are done.
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
            CHK_STATUS(hashTableContains(pJitterBuffer->pPkgBufferHashTable, index, &hasEntry));
            if (hasEntry) {
                CHK_STATUS(hashTableGet(pJitterBuffer->pPkgBufferHashTable, index, &hashValue));
                pCurPacket = (PRtpPacket) hashValue;
                isStart = sizeCalcIsFirst;
                CHK_STATUS(pJitterBuffer->depayPayloadFn(pCurPacket->payload, pCurPacket->payloadLength, NULL, &partialFrameSize, &isStart));
                curFrameSize += partialFrameSize;
                sizeCalcIsFirst = FALSE;
            }
        }

        // There is no NULL between startIndex and lastNonNullIndex
        if (UINT16_DEC(index) == lastNonNullIndex) {
            CHK_STATUS(pJitterBuffer->onFrameReadyFn(pJitterBuffer->customData, startDropIndex, lastNonNullIndex, curFrameSize));
            CHK_STATUS(jitterBufferDropBufferData(pJitterBuffer, startDropIndex, lastNonNullIndex, pJitterBuffer->headTimestamp));
        } else {
            CHK_STATUS(pJitterBuffer->onFrameDroppedFn(pJitterBuffer->customData, startDropIndex, UINT16_DEC(index), pJitterBuffer->headTimestamp));
            CHK_STATUS(jitterBufferDropBufferData(pJitterBuffer, startDropIndex, lastNonNullIndex, pJitterBuffer->headTimestamp));
        }
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

// Remove all packets containing sequence numbers between and including the startIndex and endIndex for the JitterBuffer.
// The nextTimestamp is assumed to be the timestamp of the next earliest Frame
STATUS jitterBufferDropBufferData(PJitterBuffer pJitterBuffer, UINT16 startIndex, UINT16 endIndex, UINT32 nextTimestamp)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 index = startIndex;
    UINT64 hashValue;
    PRtpPacket pCurPacket = NULL;
    BOOL hasEntry = FALSE;

    CHK(pJitterBuffer != NULL, STATUS_NULL_ARG);
    for (; UINT16_DEC(index) != endIndex; index++) {
        CHK_STATUS(hashTableContains(pJitterBuffer->pPkgBufferHashTable, index, &hasEntry));
        if (hasEntry) {
            CHK_STATUS(hashTableGet(pJitterBuffer->pPkgBufferHashTable, index, &hashValue));
            pCurPacket = (PRtpPacket) hashValue;
            if (pCurPacket) {
                freeRtpPacket(&pCurPacket);
            }
            CHK_STATUS(hashTableRemove(pJitterBuffer->pPkgBufferHashTable, index));
        }
    }
    pJitterBuffer->headTimestamp = nextTimestamp;
    pJitterBuffer->headSequenceNumber = endIndex + 1;
    if (exitTimestampOverflowCheck(pJitterBuffer)) {
        DLOGS("Exited timestamp overflow state");
    }
    if (exitSequenceNumberOverflowCheck(pJitterBuffer)) {
        DLOGS("Exited sequenceNumber overflow state");
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

// Depay all packets containing sequence numbers between and including the startIndex and endIndex for the JitterBuffer.
STATUS jitterBufferFillFrameData(PJitterBuffer pJitterBuffer, PBYTE pFrame, UINT32 frameSize, PUINT32 pFilledSize, UINT16 startIndex, UINT16 endIndex)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 index = startIndex;
    UINT64 hashValue;
    PRtpPacket pCurPacket = NULL;
    PBYTE pCurPtrInFrame = pFrame;
    UINT32 remainingFrameSize = frameSize;
    UINT32 partialFrameSize = 0;

    CHK(pJitterBuffer != NULL && pFrame != NULL && pFilledSize != NULL, STATUS_NULL_ARG);
    BOOL isFirstInFrame = TRUE;
    for (; UINT16_DEC(index) != endIndex; index++) {
        hashValue = 0;
        CHK_STATUS(hashTableGet(pJitterBuffer->pPkgBufferHashTable, index, &hashValue));
        pCurPacket = (PRtpPacket) hashValue;
        CHK(pCurPacket != NULL, STATUS_NULL_ARG);
        partialFrameSize = remainingFrameSize;
        CHK_STATUS(pJitterBuffer->depayPayloadFn(pCurPacket->payload, pCurPacket->payloadLength, pCurPtrInFrame, &partialFrameSize, &isFirstInFrame));
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
