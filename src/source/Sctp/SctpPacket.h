#ifndef __KINESIS_VIDEO_WEBRTC_SCTP_PACKET__
#define __KINESIS_VIDEO_WEBRTC_SCTP_PACKET__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Write SCTP common header (12 bytes). Returns bytes written.
UINT32 sctpWriteCommonHeader(PBYTE pBuf, UINT16 srcPort, UINT16 dstPort, UINT32 vtag);

// Finalize packet: compute and write CRC32c at offset 8.
VOID sctpFinalizePacket(PBYTE pBuf, UINT32 len);

// Write INIT chunk. Returns chunk length (padded).
UINT32 sctpWriteInitChunk(PBYTE pBuf, UINT32 initTag, UINT32 arwnd, UINT16 numOutStreams, UINT16 numInStreams, UINT32 initialTsn);

// Write INIT-ACK chunk with cookie. Returns chunk length (padded).
UINT32 sctpWriteInitAckChunk(PBYTE pBuf, UINT32 initTag, UINT32 arwnd, UINT16 numOutStreams, UINT16 numInStreams, UINT32 initialTsn,
                              PBYTE pCookie, UINT32 cookieLen);

// Write COOKIE-ECHO chunk. Returns chunk length (padded).
UINT32 sctpWriteCookieEchoChunk(PBYTE pBuf, PBYTE pCookie, UINT32 cookieLen);

// Write COOKIE-ACK chunk. Returns 4.
UINT32 sctpWriteCookieAckChunk(PBYTE pBuf);

// Write DATA chunk. Returns chunk length (padded).
UINT32 sctpWriteDataChunk(PBYTE pBuf, UINT32 tsn, UINT16 streamId, UINT16 ssn, UINT32 ppid, BOOL unordered, PBYTE pPayload,
                           UINT32 payloadLen);

// Write SACK chunk. Returns chunk length (padded).
UINT32 sctpWriteSackChunk(PBYTE pBuf, UINT32 cumTsn, UINT32 arwnd, PUINT16 pGapStarts, PUINT16 pGapEnds, UINT16 numGaps);

// Write FORWARD-TSN chunk. Returns chunk length (padded).
UINT32 sctpWriteForwardTsnChunk(PBYTE pBuf, UINT32 newCumTsn);

// Write SHUTDOWN chunk. Returns 8.
UINT32 sctpWriteShutdownChunk(PBYTE pBuf, UINT32 cumTsn);

// Write SHUTDOWN-ACK chunk. Returns 4.
UINT32 sctpWriteShutdownAckChunk(PBYTE pBuf);

// Write SHUTDOWN-COMPLETE chunk. Returns 4.
UINT32 sctpWriteShutdownCompleteChunk(PBYTE pBuf);

// Validate packet CRC32c and verification tag. Returns STATUS_SUCCESS if valid.
STATUS sctpValidatePacket(PBYTE pBuf, UINT32 len, UINT32 expectedVtag);

// Chunk iteration callback: type, flags, pChunkValue (after chunk header), valueLen
typedef STATUS (*SctpChunkCallback)(UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData);

// Iterate all chunks in a packet (skipping the 12-byte common header).
STATUS sctpParseChunks(PBYTE pBuf, UINT32 len, SctpChunkCallback callback, UINT64 customData);

#ifdef __cplusplus
}
#endif

#endif /* __KINESIS_VIDEO_WEBRTC_SCTP_PACKET__ */
