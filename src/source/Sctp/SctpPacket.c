#define LOG_CLASS "SctpPacket"
#include "../Include_i.h"

UINT32 sctpWriteCommonHeader(PBYTE pBuf, UINT16 srcPort, UINT16 dstPort, UINT32 vtag)
{
    putUnalignedInt16BigEndian(pBuf + 0, (INT16) srcPort);
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) dstPort);
    putUnalignedInt32BigEndian(pBuf + 4, (INT32) vtag);
    // checksum at offset 8 set to 0 — filled by sctpFinalizePacket
    putUnalignedInt32BigEndian(pBuf + 8, 0);
    return SCTP_COMMON_HEADER_SIZE;
}

VOID sctpFinalizePacket(PBYTE pBuf, UINT32 len)
{
    UINT32 crc;
    // Zero the checksum field before computing
    putUnalignedInt32BigEndian(pBuf + 8, 0);
    crc = sctpCrc32c(pBuf, len);
    // CRC32c is stored in little-endian per RFC 9260 Appendix A
    pBuf[8] = (BYTE)(crc & 0xFF);
    pBuf[9] = (BYTE)((crc >> 8) & 0xFF);
    pBuf[10] = (BYTE)((crc >> 16) & 0xFF);
    pBuf[11] = (BYTE)((crc >> 24) & 0xFF);
}

UINT32 sctpWriteInitChunk(PBYTE pBuf, UINT32 initTag, UINT32 arwnd, UINT16 numOutStreams, UINT16 numInStreams, UINT32 initialTsn)
{
    UINT32 chunkLen;
    UINT32 offset = 0;

    // INIT includes Forward-TSN-Supported parameter (RFC 3758)
    // Chunk header
    chunkLen = SCTP_INIT_HEADER_SIZE + 4; // +4 for Forward-TSN-Supported param
    pBuf[offset] = SCTP_CHUNK_INIT;
    pBuf[offset + 1] = 0; // flags
    putUnalignedInt16BigEndian(pBuf + offset + 2, (INT16) chunkLen);
    offset += SCTP_CHUNK_HEADER_SIZE;

    // Initiate Tag
    putUnalignedInt32BigEndian(pBuf + offset, (INT32) initTag);
    offset += 4;
    // A-RWND
    putUnalignedInt32BigEndian(pBuf + offset, (INT32) arwnd);
    offset += 4;
    // Number of Outbound Streams
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) numOutStreams);
    offset += 2;
    // Number of Inbound Streams
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) numInStreams);
    offset += 2;
    // Initial TSN
    putUnalignedInt32BigEndian(pBuf + offset, (INT32) initialTsn);
    offset += 4;

    // Forward-TSN-Supported parameter (type=0xC000, length=4)
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) SCTP_PARAM_FORWARD_TSN_SUPPORTED);
    offset += 2;
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) 4);
    offset += 2;

    return SCTP_PAD4(offset);
}

UINT32 sctpWriteInitAckChunk(PBYTE pBuf, UINT32 initTag, UINT32 arwnd, UINT16 numOutStreams, UINT16 numInStreams, UINT32 initialTsn,
                              PBYTE pCookie, UINT32 cookieLen)
{
    UINT32 chunkLen;
    UINT32 offset = 0;
    UINT32 paramLen;

    // INIT-ACK body + State Cookie param + Forward-TSN-Supported param
    paramLen = 4 + cookieLen; // State Cookie parameter: type(2) + len(2) + cookie
    chunkLen = SCTP_INIT_HEADER_SIZE + paramLen + 4; // +4 for Forward-TSN-Supported

    pBuf[offset] = SCTP_CHUNK_INIT_ACK;
    pBuf[offset + 1] = 0;
    putUnalignedInt16BigEndian(pBuf + offset + 2, (INT16) chunkLen);
    offset += SCTP_CHUNK_HEADER_SIZE;

    putUnalignedInt32BigEndian(pBuf + offset, (INT32) initTag);
    offset += 4;
    putUnalignedInt32BigEndian(pBuf + offset, (INT32) arwnd);
    offset += 4;
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) numOutStreams);
    offset += 2;
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) numInStreams);
    offset += 2;
    putUnalignedInt32BigEndian(pBuf + offset, (INT32) initialTsn);
    offset += 4;

    // State Cookie parameter (type=7)
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) SCTP_PARAM_STATE_COOKIE);
    offset += 2;
    putUnalignedInt16BigEndian(pBuf + offset, (INT16)(4 + cookieLen));
    offset += 2;
    MEMCPY(pBuf + offset, pCookie, cookieLen);
    offset += cookieLen;
    // Pad cookie to 4-byte boundary
    while (offset % 4 != 0) {
        pBuf[offset++] = 0;
    }

    // Forward-TSN-Supported parameter
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) SCTP_PARAM_FORWARD_TSN_SUPPORTED);
    offset += 2;
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) 4);
    offset += 2;

    return SCTP_PAD4(offset);
}

UINT32 sctpWriteCookieEchoChunk(PBYTE pBuf, PBYTE pCookie, UINT32 cookieLen)
{
    UINT32 chunkLen = SCTP_CHUNK_HEADER_SIZE + cookieLen;

    pBuf[0] = SCTP_CHUNK_COOKIE_ECHO;
    pBuf[1] = 0;
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) chunkLen);
    MEMCPY(pBuf + SCTP_CHUNK_HEADER_SIZE, pCookie, cookieLen);

    return SCTP_PAD4(chunkLen);
}

UINT32 sctpWriteCookieAckChunk(PBYTE pBuf)
{
    pBuf[0] = SCTP_CHUNK_COOKIE_ACK;
    pBuf[1] = 0;
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) SCTP_COOKIE_ACK_SIZE);
    return SCTP_COOKIE_ACK_SIZE;
}

UINT32 sctpWriteDataChunk(PBYTE pBuf, UINT32 tsn, UINT16 streamId, UINT16 ssn, UINT32 ppid, BOOL unordered, PBYTE pPayload,
                           UINT32 payloadLen)
{
    UINT32 chunkLen = SCTP_DATA_HEADER_SIZE + payloadLen;
    UINT8 flags = SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END; // single fragment

    if (unordered) {
        flags |= SCTP_DATA_FLAG_UNORDERED;
    }

    pBuf[0] = SCTP_CHUNK_DATA;
    pBuf[1] = flags;
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) chunkLen);

    // TSN
    putUnalignedInt32BigEndian(pBuf + 4, (INT32) tsn);
    // Stream Identifier
    putUnalignedInt16BigEndian(pBuf + 8, (INT16) streamId);
    // Stream Sequence Number
    putUnalignedInt16BigEndian(pBuf + 10, (INT16) ssn);
    // Payload Protocol Identifier
    putUnalignedInt32BigEndian(pBuf + 12, (INT32) ppid);

    MEMCPY(pBuf + SCTP_DATA_HEADER_SIZE, pPayload, payloadLen);

    return SCTP_PAD4(chunkLen);
}

UINT32 sctpWriteSackChunk(PBYTE pBuf, UINT32 cumTsn, UINT32 arwnd, PUINT16 pGapStarts, PUINT16 pGapEnds, UINT16 numGaps)
{
    UINT32 offset = 0;
    UINT16 i;
    UINT32 chunkLen = SCTP_SACK_HEADER_SIZE + numGaps * 4; // each gap block = 4 bytes

    pBuf[offset] = SCTP_CHUNK_SACK;
    pBuf[offset + 1] = 0;
    putUnalignedInt16BigEndian(pBuf + offset + 2, (INT16) chunkLen);
    offset += SCTP_CHUNK_HEADER_SIZE;

    // Cumulative TSN Ack
    putUnalignedInt32BigEndian(pBuf + offset, (INT32) cumTsn);
    offset += 4;
    // Advertised Receiver Window Credit
    putUnalignedInt32BigEndian(pBuf + offset, (INT32) arwnd);
    offset += 4;
    // Number of Gap Ack Blocks
    putUnalignedInt16BigEndian(pBuf + offset, (INT16) numGaps);
    offset += 2;
    // Number of Duplicate TSNs (always 0 in our impl)
    putUnalignedInt16BigEndian(pBuf + offset, 0);
    offset += 2;

    // Gap Ack Blocks
    for (i = 0; i < numGaps; i++) {
        putUnalignedInt16BigEndian(pBuf + offset, (INT16) pGapStarts[i]);
        offset += 2;
        putUnalignedInt16BigEndian(pBuf + offset, (INT16) pGapEnds[i]);
        offset += 2;
    }

    return SCTP_PAD4(offset);
}

UINT32 sctpWriteForwardTsnChunk(PBYTE pBuf, UINT32 newCumTsn)
{
    UINT32 chunkLen = SCTP_FORWARD_TSN_HEADER_SIZE;

    pBuf[0] = SCTP_CHUNK_FORWARD_TSN;
    pBuf[1] = 0;
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) chunkLen);
    putUnalignedInt32BigEndian(pBuf + 4, (INT32) newCumTsn);

    return SCTP_PAD4(chunkLen);
}

UINT32 sctpWriteShutdownChunk(PBYTE pBuf, UINT32 cumTsn)
{
    pBuf[0] = SCTP_CHUNK_SHUTDOWN;
    pBuf[1] = 0;
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) SCTP_SHUTDOWN_SIZE);
    putUnalignedInt32BigEndian(pBuf + 4, (INT32) cumTsn);
    return SCTP_SHUTDOWN_SIZE;
}

UINT32 sctpWriteShutdownAckChunk(PBYTE pBuf)
{
    pBuf[0] = SCTP_CHUNK_SHUTDOWN_ACK;
    pBuf[1] = 0;
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) SCTP_SHUTDOWN_ACK_SIZE);
    return SCTP_SHUTDOWN_ACK_SIZE;
}

UINT32 sctpWriteShutdownCompleteChunk(PBYTE pBuf)
{
    pBuf[0] = SCTP_CHUNK_SHUTDOWN_COMPLETE;
    pBuf[1] = 0;
    putUnalignedInt16BigEndian(pBuf + 2, (INT16) SCTP_SHUTDOWN_COMPLETE_SIZE);
    return SCTP_SHUTDOWN_COMPLETE_SIZE;
}

STATUS sctpValidatePacket(PBYTE pBuf, UINT32 len, UINT32 expectedVtag)
{
    UINT32 storedCrc, computedCrc;
    UINT32 vtag;

    if (pBuf == NULL || len < SCTP_COMMON_HEADER_SIZE) {
        return STATUS_SCTP_INVALID_DCEP_PACKET;
    }

    // Read stored checksum (little-endian per RFC 9260)
    storedCrc = (UINT32) pBuf[8] | ((UINT32) pBuf[9] << 8) | ((UINT32) pBuf[10] << 16) | ((UINT32) pBuf[11] << 24);

    // Zero checksum field and compute
    pBuf[8] = pBuf[9] = pBuf[10] = pBuf[11] = 0;
    computedCrc = sctpCrc32c(pBuf, len);

    // Restore checksum
    pBuf[8] = (BYTE)(storedCrc & 0xFF);
    pBuf[9] = (BYTE)((storedCrc >> 8) & 0xFF);
    pBuf[10] = (BYTE)((storedCrc >> 16) & 0xFF);
    pBuf[11] = (BYTE)((storedCrc >> 24) & 0xFF);

    if (storedCrc != computedCrc) {
        DLOGW("SCTP CRC32c mismatch: stored=0x%08x computed=0x%08x", storedCrc, computedCrc);
        return STATUS_SCTP_INVALID_DCEP_PACKET;
    }

    // Check verification tag
    vtag = (UINT32) getUnalignedInt32BigEndian((PINT32)(pBuf + 4));
    // INIT chunks have vtag=0 — allow that
    if (vtag != 0 && expectedVtag != 0 && vtag != expectedVtag) {
        DLOGW("SCTP vtag mismatch: got=0x%08x expected=0x%08x", vtag, expectedVtag);
        return STATUS_SCTP_INVALID_DCEP_PACKET;
    }

    return STATUS_SUCCESS;
}

STATUS sctpParseChunks(PBYTE pBuf, UINT32 len, SctpChunkCallback callback, UINT64 customData)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 offset = SCTP_COMMON_HEADER_SIZE;
    UINT8 chunkType, chunkFlags;
    UINT16 chunkLen;
    UINT32 paddedLen;

    while (offset + SCTP_CHUNK_HEADER_SIZE <= len) {
        chunkType = pBuf[offset];
        chunkFlags = pBuf[offset + 1];
        chunkLen = (UINT16) getUnalignedInt16BigEndian((PINT16)(pBuf + offset + 2));

        if (chunkLen < SCTP_CHUNK_HEADER_SIZE || offset + chunkLen > len) {
            break;
        }

        CHK_STATUS(callback(chunkType, chunkFlags, pBuf + offset + SCTP_CHUNK_HEADER_SIZE, chunkLen - SCTP_CHUNK_HEADER_SIZE, customData));

        paddedLen = SCTP_PAD4(chunkLen);
        offset += paddedLen;
    }

CleanUp:
    return retStatus;
}
