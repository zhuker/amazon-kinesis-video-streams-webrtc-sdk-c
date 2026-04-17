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
typedef struct {
    UINT8 payloadType;
    UINT32 tsOffset; // 0 for primary
    UINT32 payloadLen;
    BOOL isLast; // TRUE for primary block
} RedBlockInfo;

// Parse the RED header chain. Body is the RTP payload body of the RED packet.
// Returns STATUS_RTP_INVALID_RED_PACKET if the chain is truncated or too long.
static STATUS parseRedHeaders(PBYTE body, UINT32 bodyLen, RedBlockInfo* pBlocks, UINT32 maxBlocks, PUINT32 pBlockCount, PUINT32 pHeaderTotalLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 count = 0;
    UINT32 off = 0;

    while (off < bodyLen) {
        CHK(count < maxBlocks, STATUS_RTP_INVALID_RED_PACKET);
        BYTE first = body[off];
        BOOL hasF = (first & 0x80) != 0;
        if (hasF) {
            // Non-last: 4-byte header
            CHK(off + RED_HEADER_LEN_NON_LAST <= bodyLen, STATUS_RTP_INVALID_RED_PACKET);
            UINT8 pt = (UINT8) (first & 0x7F);
            UINT32 tsDelta = (((UINT32) body[off + 1]) << 6) | (((UINT32) body[off + 2]) >> 2);
            UINT32 len = ((((UINT32) body[off + 2]) & 0x3) << 8) | ((UINT32) body[off + 3]);
            pBlocks[count].payloadType = pt;
            pBlocks[count].tsOffset = tsDelta;
            pBlocks[count].payloadLen = len;
            pBlocks[count].isLast = FALSE;
            count++;
            off += RED_HEADER_LEN_NON_LAST;
        } else {
            // Last (primary): 1-byte header. Length determined by remaining bytes.
            UINT8 pt = (UINT8) (first & 0x7F);
            pBlocks[count].payloadType = pt;
            pBlocks[count].tsOffset = 0;
            pBlocks[count].payloadLen = 0; // fixed up below after total header length known
            pBlocks[count].isLast = TRUE;
            count++;
            off += RED_HEADER_LEN_LAST;
            break;
        }
    }

    CHK(count > 0, STATUS_RTP_INVALID_RED_PACKET);
    CHK(pBlocks[count - 1].isLast, STATUS_RTP_INVALID_RED_PACKET);

    *pBlockCount = count;
    *pHeaderTotalLen = off;

CleanUp:
    return retStatus;
}

// Build one synthetic RtpPacket with a fresh owned pRawPacket buffer.
static STATUS buildSyntheticPacket(PRtpPacket pSourceRed, UINT8 opusPt, UINT32 timestamp, UINT16 sequenceNumber, PBYTE payload, UINT32 payloadLen,
                                   BOOL isSynthetic, PRtpPacket* ppOut)
{
    STATUS retStatus = STATUS_SUCCESS;
    PRtpPacket pPkt = NULL;
    PBYTE pBuf = NULL;

    pPkt = (PRtpPacket) MEMCALLOC(1, SIZEOF(RtpPacket));
    CHK(pPkt != NULL, STATUS_NOT_ENOUGH_MEMORY);

    if (payloadLen > 0) {
        pBuf = (PBYTE) MEMALLOC(payloadLen);
        CHK(pBuf != NULL, STATUS_NOT_ENOUGH_MEMORY);
        MEMCPY(pBuf, payload, payloadLen);
    }

    pPkt->header.version = 2;
    pPkt->header.padding = FALSE;
    pPkt->header.extension = FALSE;
    pPkt->header.marker = pSourceRed->header.marker;
    pPkt->header.csrcCount = 0;
    pPkt->header.payloadType = opusPt;
    pPkt->header.sequenceNumber = sequenceNumber;
    pPkt->header.timestamp = timestamp;
    pPkt->header.ssrc = pSourceRed->header.ssrc;
    pPkt->header.csrcArray = NULL;
    pPkt->header.extensionProfile = 0;
    pPkt->header.extensionPayload = NULL;
    pPkt->header.extensionLength = 0;

    pPkt->pRawPacket = pBuf;
    pPkt->rawPacketLength = payloadLen;
    pPkt->payload = pBuf;
    pPkt->payloadLength = payloadLen;
    pPkt->receivedTime = pSourceRed->receivedTime;
    pPkt->sentTime = pSourceRed->sentTime;
    pPkt->isSynthetic = isSynthetic;

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        SAFE_MEMFREE(pBuf);
        SAFE_MEMFREE(pPkt);
        pPkt = NULL;
    }
    if (ppOut != NULL) {
        *ppOut = pPkt;
    }
    return retStatus;
}

STATUS splitRedRtpPacket(PRtpPacket pRedPacket, UINT8 negotiatedOpusPt, PRtpPacket* ppSyntheticPackets, UINT32 maxCount, PUINT32 pProducedCount,
                         PUINT32 pFecBytes)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    RedBlockInfo blocks[RED_MAX_BLOCKS];
    UINT32 blockCount = 0;
    UINT32 headerTotalLen = 0;
    UINT32 fecBytes = 0;
    UINT32 produced = 0;
    UINT32 i;
    PBYTE body;
    UINT32 bodyLen;
    PBYTE pPayloadCursor;
    UINT32 payloadsTotalLen = 0;
    UINT32 createdCount = 0;

    CHK(pRedPacket != NULL && ppSyntheticPackets != NULL && pProducedCount != NULL, STATUS_NULL_ARG);
    CHK(maxCount >= 1, STATUS_INVALID_ARG);

    body = pRedPacket->payload;
    bodyLen = pRedPacket->payloadLength;
    CHK(body != NULL && bodyLen > 0, STATUS_RTP_INVALID_RED_PACKET);

    CHK_STATUS(parseRedHeaders(body, bodyLen, blocks, RED_MAX_BLOCKS, &blockCount, &headerTotalLen));

    // Sum declared payload lengths (excluding primary, which is implicit).
    for (i = 0; i + 1 < blockCount; i++) {
        payloadsTotalLen += blocks[i].payloadLen;
    }
    CHK(headerTotalLen + payloadsTotalLen <= bodyLen, STATUS_RTP_INVALID_RED_PACKET);

    // Primary block length = remaining bytes after all headers and redundant payloads.
    blocks[blockCount - 1].payloadLen = bodyLen - headerTotalLen - payloadsTotalLen;

    // Safety: we need the caller to have room for every possibly-emitted synthetic.
    // If not, we error out rather than silently truncating.
    CHK(blockCount <= maxCount, STATUS_RTP_INVALID_RED_PACKET);

    pPayloadCursor = body + headerTotalLen;

    for (i = 0; i < blockCount; i++) {
        BOOL isPrimary = blocks[i].isLast;
        UINT32 ts;
        UINT16 seq;
        PRtpPacket pSyn = NULL;

        // Reject blocks whose inner PT disagrees with Opus. Skip them but keep parsing.
        if (blocks[i].payloadType != negotiatedOpusPt) {
            DLOGW("RED block %u PT %u != Opus PT %u — skipping", i, blocks[i].payloadType, negotiatedOpusPt);
            pPayloadCursor += blocks[i].payloadLen;
            continue;
        }

        if (isPrimary) {
            ts = pRedPacket->header.timestamp;
            seq = pRedPacket->header.sequenceNumber;
        } else {
            ts = pRedPacket->header.timestamp - blocks[i].tsOffset;
            // Synthetic seqnum: distance from primary is (blockCount - 1 - i), subtract from outer seq.
            seq = (UINT16) (pRedPacket->header.sequenceNumber - (blockCount - 1 - i));
            fecBytes += blocks[i].payloadLen;
        }

        CHK_STATUS(
            buildSyntheticPacket(pRedPacket, negotiatedOpusPt, ts, seq, pPayloadCursor, blocks[i].payloadLen, !isPrimary /*isSynthetic*/, &pSyn));
        ppSyntheticPackets[createdCount++] = pSyn;
        pPayloadCursor += blocks[i].payloadLen;
    }

    produced = createdCount;

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        // Free any partially-built synthetics so the caller gets all-or-nothing semantics.
        for (i = 0; i < createdCount; i++) {
            PRtpPacket p = ppSyntheticPackets[i];
            freeRtpPacket(&p);
            ppSyntheticPackets[i] = NULL;
        }
        produced = 0;
        fecBytes = 0;
    }
    if (pProducedCount != NULL) {
        *pProducedCount = produced;
    }
    if (pFecBytes != NULL) {
        *pFecBytes = fecBytes;
    }
    LEAVES();
    return retStatus;
}
