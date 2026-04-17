#define LOG_CLASS "RtpRedPayloader"

#include "../../Include_i.h"

STATUS createRedSenderState(UINT8 redundancyLevel, UINT8 opusPayloadType, PRedSenderState* ppState)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PRedSenderState pState = NULL;

    CHK(ppState != NULL, STATUS_NULL_ARG);

    if (redundancyLevel == 0) {
        redundancyLevel = RED_DEFAULT_REDUNDANCY;
    }
    if (redundancyLevel > RED_MAX_REDUNDANCY) {
        redundancyLevel = RED_MAX_REDUNDANCY;
    }

    pState = (PRedSenderState) MEMCALLOC(1, SIZEOF(RedSenderState));
    CHK(pState != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pState->redundancyLevel = redundancyLevel;
    pState->opusPayloadType = opusPayloadType;
    pState->nextSlot = 0;

CleanUp:
    if (ppState != NULL) {
        *ppState = pState;
    }
    LEAVES();
    return retStatus;
}

STATUS freeRedSenderState(PRedSenderState* ppState)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHK(ppState != NULL, STATUS_NULL_ARG);
    SAFE_MEMFREE(*ppState);
CleanUp:
    LEAVES();
    return retStatus;
}

// Collect indices of ring slots to include in redundancy, oldest first.
// Returns the count of blocks chosen and the total body size they contribute.
// Does not mutate state.
static VOID selectRedundantBlocks(PRedSenderState pState, UINT32 rtpTimestamp, UINT32 mtuBudget, UINT32 primaryLen, PUINT32 pChosenIndices,
                                  PUINT32 pChosenCount, PUINT32 pBodyBytes)
{
    // The body layout is:
    //   chosenCount * 4 bytes (non-last headers)
    //   + 1 byte (last header for primary)
    //   + sum(chosen slot payload lengths)
    //   + primary payload length
    UINT32 chosenCount = 0;
    UINT32 bodyBytes = RED_HEADER_LEN_LAST + primaryLen;
    UINT32 i;
    UINT32 slotIdx;
    UINT32 slotsToScan = pState->redundancyLevel;

    // Walk the ring from oldest (pState->nextSlot) toward newest ((pState->nextSlot - 1) mod N).
    // We emit blocks in that order so their timestamp offsets decrease monotonically.
    for (i = 0; i < slotsToScan; i++) {
        slotIdx = (pState->nextSlot + i) % pState->redundancyLevel;
        PRedSenderSlot pSlot = &pState->slots[slotIdx];
        if (pSlot->payloadLen == 0) {
            continue;
        }

        UINT32 tsDelta = rtpTimestamp - pSlot->rtpTimestamp;
        if (tsDelta == 0 || tsDelta >= RED_MAX_TS_DELTA) {
            // Cannot express offset in 14 bits. Drop this slot silently
            // (DTX gap / wrap / duplicate ts).
            continue;
        }

        if (pSlot->payloadLen > RED_MAX_BLOCK_LEN) {
            // Defensive — should never happen because we don't insert oversize
            // primaries into the ring.
            continue;
        }

        UINT32 addedBytes = RED_HEADER_LEN_NON_LAST + pSlot->payloadLen;
        if (bodyBytes + addedBytes > mtuBudget) {
            break;
        }

        pChosenIndices[chosenCount++] = slotIdx;
        bodyBytes += addedBytes;
        if (chosenCount >= RED_MAX_BLOCKS - 1) {
            break;
        }
    }

    *pChosenCount = chosenCount;
    *pBodyBytes = bodyBytes;
}

STATUS createPayloadForOpusRed(UINT32 mtu, PBYTE opusFrame, UINT32 opusFrameLength, UINT32 rtpTimestamp, PRedSenderState pState, PBYTE payloadBuffer,
                               PUINT32 pPayloadLength, PUINT32 pPayloadSubLength, PUINT32 pPayloadSubLenSize, PBOOL pIsFallbackToPlainOpus)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL sizeCalculationOnly = (payloadBuffer == NULL);
    BOOL fallback = FALSE;
    UINT32 chosenIndices[RED_MAX_BLOCKS];
    UINT32 chosenCount = 0;
    UINT32 bodyBytes = 0;
    UINT32 mtuBudget;
    UINT32 i;
    PBYTE pOut;

    CHK(pState != NULL && opusFrame != NULL && pPayloadLength != NULL && pPayloadSubLenSize != NULL && pIsFallbackToPlainOpus != NULL &&
            (sizeCalculationOnly || pPayloadSubLength != NULL),
        STATUS_NULL_ARG);
    CHK(opusFrameLength > 0, STATUS_INVALID_ARG);

    // Reserve MTU headroom for the outer RTP header and SRTP auth tag. The caller passes
    // the raw MTU and we approximate the per-packet overhead the same way libwebrtc does.
    // RTP_HEADER is at minimum 12 bytes; TWCC adds up to 8 bytes of extension header; we
    // subtract a comfortable 24 to cover both.
    if (mtu > (UINT32) (SRTP_AUTH_TAG_OVERHEAD + 24)) {
        mtuBudget = mtu - SRTP_AUTH_TAG_OVERHEAD - 24;
    } else {
        mtuBudget = 0;
    }

    // Fallback to bare Opus when primary exceeds RED's 10-bit block length field.
    if (opusFrameLength > RED_MAX_BLOCK_LEN) {
        fallback = TRUE;
        bodyBytes = opusFrameLength;
    } else {
        selectRedundantBlocks(pState, rtpTimestamp, mtuBudget, opusFrameLength, chosenIndices, &chosenCount, &bodyBytes);
    }

    if (sizeCalculationOnly) {
        *pPayloadLength = bodyBytes;
        *pPayloadSubLenSize = 1;
        *pIsFallbackToPlainOpus = fallback;
        CHK(FALSE, retStatus);
    }

    CHK(*pPayloadLength >= bodyBytes && *pPayloadSubLenSize >= 1, STATUS_BUFFER_TOO_SMALL);

    pOut = payloadBuffer;

    if (fallback) {
        MEMCPY(pOut, opusFrame, opusFrameLength);
        pOut += opusFrameLength;
        // Fallback packets must NOT pollute the redundancy ring: they represent an
        // out-of-band Opus payload whose size is incompatible with RED's wire format.
    } else {
        UINT8 opusPt = pState->opusPayloadType;

        // Write all non-last RED headers.
        for (i = 0; i < chosenCount; i++) {
            PRedSenderSlot pSlot = &pState->slots[chosenIndices[i]];
            UINT32 tsDelta = rtpTimestamp - pSlot->rtpTimestamp;
            UINT32 blockLen = pSlot->payloadLen;
            pOut[0] = (BYTE) (0x80 | (opusPt & 0x7F));
            pOut[1] = (BYTE) (((tsDelta & 0x3FFF) >> 6) & 0xFF);
            pOut[2] = (BYTE) ((((tsDelta & 0x3F) << 2) | ((blockLen >> 8) & 0x3)) & 0xFF);
            pOut[3] = (BYTE) (blockLen & 0xFF);
            pOut += RED_HEADER_LEN_NON_LAST;
        }

        // Primary header (F=0, PT only).
        pOut[0] = (BYTE) (opusPt & 0x7F);
        pOut += RED_HEADER_LEN_LAST;

        // Redundant payloads (oldest first).
        for (i = 0; i < chosenCount; i++) {
            PRedSenderSlot pSlot = &pState->slots[chosenIndices[i]];
            MEMCPY(pOut, pSlot->payload, pSlot->payloadLen);
            pOut += pSlot->payloadLen;
        }

        // Primary payload.
        MEMCPY(pOut, opusFrame, opusFrameLength);
        pOut += opusFrameLength;

        // Now mutate the ring: write the just-sent primary into the newest slot.
        // nextSlot points at the oldest; overwrite it and advance.
        PRedSenderSlot pNewest = &pState->slots[pState->nextSlot];
        pNewest->rtpTimestamp = rtpTimestamp;
        pNewest->payloadLen = opusFrameLength;
        MEMCPY(pNewest->payload, opusFrame, opusFrameLength);
        pState->nextSlot = (pState->nextSlot + 1) % pState->redundancyLevel;
    }

    *pPayloadLength = bodyBytes;
    *pPayloadSubLenSize = 1;
    pPayloadSubLength[0] = bodyBytes;
    *pIsFallbackToPlainOpus = fallback;

CleanUp:
    LEAVES();
    return retStatus;
}
