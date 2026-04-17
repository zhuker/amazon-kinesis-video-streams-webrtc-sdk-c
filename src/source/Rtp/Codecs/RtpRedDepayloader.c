#define LOG_CLASS "RtpRedDepayloader"

#include "../../Include_i.h"

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
