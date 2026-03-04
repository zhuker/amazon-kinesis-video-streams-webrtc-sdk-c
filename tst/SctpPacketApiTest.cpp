#include "SctpTestHelpers.h"

#ifdef ENABLE_DATA_CHANNEL
namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class SctpPacketApiTest : public WebRtcClientTestBase {
};

/******************************************************************************
 * Common Header
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeCommonHeader_returnsCorrectSize)
{
    BYTE buf[64];
    EXPECT_EQ(sctpWriteCommonHeader(buf, 5000, 5000, 0), (UINT32) SCTP_COMMON_HEADER_SIZE);
}

TEST_F(SctpPacketApiTest, writeCommonHeader_portsEncoded)
{
    BYTE buf[64];
    MEMSET(buf, 0, sizeof(buf));
    sctpWriteCommonHeader(buf, 5000, 6000, 0);

    UINT16 srcPort = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 0));
    UINT16 dstPort = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 2));
    EXPECT_EQ(srcPort, 5000);
    EXPECT_EQ(dstPort, 6000);
}

TEST_F(SctpPacketApiTest, writeCommonHeader_vtagEncoded)
{
    BYTE buf[64];
    sctpWriteCommonHeader(buf, 5000, 5000, 0xDEADBEEF);

    UINT32 vtag = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 4));
    EXPECT_EQ(vtag, (UINT32) 0xDEADBEEF);
}

TEST_F(SctpPacketApiTest, writeCommonHeader_checksumZeroed)
{
    BYTE buf[64];
    MEMSET(buf, 0xFF, sizeof(buf));
    sctpWriteCommonHeader(buf, 5000, 5000, 0);

    // Checksum at bytes 8-11 should be zero
    UINT32 checksum = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 8));
    EXPECT_EQ(checksum, (UINT32) 0);
}

TEST_F(SctpPacketApiTest, finalizePacket_setsValidCrc32c)
{
    BYTE buf[64];
    UINT32 len = sctpWriteCommonHeader(buf, 5000, 5000, 0x12345678);
    sctpFinalizePacket(buf, len);

    // Read stored CRC (little-endian)
    UINT32 storedCrc = (UINT32) buf[8] | ((UINT32) buf[9] << 8) | ((UINT32) buf[10] << 16) | ((UINT32) buf[11] << 24);

    // Verify by re-computing
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    UINT32 computedCrc = sctpCrc32c(buf, len);
    EXPECT_EQ(storedCrc, computedCrc);
}

/******************************************************************************
 * INIT Chunk
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeInitChunk_returnsCorrectSize)
{
    BYTE buf[128];
    // INIT_HEADER_SIZE=20 + Forward-TSN-Supported param=4 → 24
    UINT32 size = sctpWriteInitChunk(buf, 0x11111111, 131072, 300, 300, 0x11111111);
    EXPECT_EQ(size, SCTP_PAD4(SCTP_INIT_HEADER_SIZE + 4));
}

TEST_F(SctpPacketApiTest, writeInitChunk_chunkTypeAndFlags)
{
    BYTE buf[128];
    sctpWriteInitChunk(buf, 0x11111111, 131072, 300, 300, 0x11111111);
    EXPECT_EQ(buf[0], SCTP_CHUNK_INIT);
    EXPECT_EQ(buf[1], 0); // flags
}

TEST_F(SctpPacketApiTest, writeInitChunk_fieldsEncoded)
{
    BYTE buf[128];
    sctpWriteInitChunk(buf, 0xAABBCCDD, 65536, 100, 200, 0x11223344);

    UINT32 initTag = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 4));
    UINT32 arwnd = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 8));
    UINT16 os = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 12));
    UINT16 is = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 14));
    UINT32 initialTsn = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 16));

    EXPECT_EQ(initTag, (UINT32) 0xAABBCCDD);
    EXPECT_EQ(arwnd, (UINT32) 65536);
    EXPECT_EQ(os, 100);
    EXPECT_EQ(is, 200);
    EXPECT_EQ(initialTsn, (UINT32) 0x11223344);
}

TEST_F(SctpPacketApiTest, writeInitChunk_forwardTsnSupported)
{
    BYTE buf[128];
    UINT32 chunkSize = sctpWriteInitChunk(buf, 0x11111111, 131072, 300, 300, 0x11111111);

    // Forward-TSN-Supported param at offset 20 (after INIT header)
    UINT16 paramType = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 20));
    UINT16 paramLen = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 22));
    EXPECT_EQ(paramType, (UINT16) SCTP_PARAM_FORWARD_TSN_SUPPORTED);
    EXPECT_EQ(paramLen, (UINT16) 4);
}

TEST_F(SctpPacketApiTest, writeInitChunk_roundtripParseable)
{
    BYTE packet[256];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0);
    off += sctpWriteInitChunk(packet + off, 0x11111111, 131072, 300, 300, 0x11111111);
    sctpFinalizePacket(packet, off);

    // Verify the packet is parseable
    struct {
        UINT8 type;
        UINT32 callCount;
    } ctx = {0, 0};

    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        auto* c = (decltype(ctx)*) customData;
        c->type = type;
        c->callCount++;
        return STATUS_SUCCESS;
    };

    EXPECT_EQ(sctpParseChunks(packet, off, callback, (UINT64) &ctx), STATUS_SUCCESS);
    EXPECT_EQ(ctx.callCount, (UINT32) 1);
    EXPECT_EQ(ctx.type, SCTP_CHUNK_INIT);
}

/******************************************************************************
 * INIT-ACK Chunk
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeInitAckChunk_returnsCorrectSize)
{
    BYTE buf[256];
    BYTE cookie[SCTP_COOKIE_SIZE];
    MEMSET(cookie, 0xAA, SCTP_COOKIE_SIZE);

    UINT32 size = sctpWriteInitAckChunk(buf, 0x11111111, 131072, 300, 300, 0x11111111, cookie, SCTP_COOKIE_SIZE);
    // INIT_HEADER_SIZE(20) + State Cookie param header(4) + cookie(44) + Forward-TSN param(4) = 72
    UINT32 expected = SCTP_PAD4(SCTP_INIT_HEADER_SIZE + 4 + SCTP_COOKIE_SIZE + 4);
    EXPECT_EQ(size, expected);
}

TEST_F(SctpPacketApiTest, writeInitAckChunk_containsCookie)
{
    BYTE buf[256];
    BYTE cookie[SCTP_COOKIE_SIZE];
    MEMSET(cookie, 0xBB, SCTP_COOKIE_SIZE);

    sctpWriteInitAckChunk(buf, 0x11111111, 131072, 300, 300, 0x11111111, cookie, SCTP_COOKIE_SIZE);

    // State Cookie parameter starts at offset 20 (after INIT-ACK body)
    UINT16 paramType = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 20));
    EXPECT_EQ(paramType, (UINT16) SCTP_PARAM_STATE_COOKIE);
}

TEST_F(SctpPacketApiTest, writeInitAckChunk_cookieDataMatches)
{
    BYTE buf[256];
    BYTE cookie[SCTP_COOKIE_SIZE];
    for (UINT32 i = 0; i < SCTP_COOKIE_SIZE; i++) {
        cookie[i] = (BYTE)(i & 0xFF);
    }

    sctpWriteInitAckChunk(buf, 0x11111111, 131072, 300, 300, 0x11111111, cookie, SCTP_COOKIE_SIZE);

    // Cookie data at offset 24 (param header at 20 + 4 bytes header)
    EXPECT_EQ(0, MEMCMP(buf + 24, cookie, SCTP_COOKIE_SIZE));
}

TEST_F(SctpPacketApiTest, writeInitAckChunk_forwardTsnSupported)
{
    BYTE buf[256];
    BYTE cookie[SCTP_COOKIE_SIZE];
    MEMSET(cookie, 0, SCTP_COOKIE_SIZE);

    UINT32 size = sctpWriteInitAckChunk(buf, 0x11111111, 131072, 300, 300, 0x11111111, cookie, SCTP_COOKIE_SIZE);

    // Forward-TSN-Supported comes after cookie (padded)
    UINT32 ftsOffset = 20 + 4 + SCTP_PAD4(SCTP_COOKIE_SIZE);
    UINT16 paramType = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + ftsOffset));
    EXPECT_EQ(paramType, (UINT16) SCTP_PARAM_FORWARD_TSN_SUPPORTED);
}

TEST_F(SctpPacketApiTest, writeInitAckChunk_roundtripParseable)
{
    BYTE packet[256];
    BYTE cookie[SCTP_COOKIE_SIZE];
    MEMSET(cookie, 0, SCTP_COOKIE_SIZE);

    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    off += sctpWriteInitAckChunk(packet + off, 0x11111111, 131072, 300, 300, 0x11111111, cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(packet, off);

    UINT8 foundType = 0;
    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        *(UINT8*) customData = type;
        return STATUS_SUCCESS;
    };

    EXPECT_EQ(sctpParseChunks(packet, off, callback, (UINT64) &foundType), STATUS_SUCCESS);
    EXPECT_EQ(foundType, SCTP_CHUNK_INIT_ACK);
}

/******************************************************************************
 * COOKIE-ECHO Chunk
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeCookieEchoChunk_returnsCorrectSize)
{
    BYTE buf[128];
    BYTE cookie[SCTP_COOKIE_SIZE];
    MEMSET(cookie, 0, SCTP_COOKIE_SIZE);

    UINT32 size = sctpWriteCookieEchoChunk(buf, cookie, SCTP_COOKIE_SIZE);
    EXPECT_EQ(size, SCTP_PAD4(SCTP_CHUNK_HEADER_SIZE + SCTP_COOKIE_SIZE));
}

TEST_F(SctpPacketApiTest, writeCookieEchoChunk_chunkType)
{
    BYTE buf[128];
    BYTE cookie[SCTP_COOKIE_SIZE];
    MEMSET(cookie, 0, SCTP_COOKIE_SIZE);

    sctpWriteCookieEchoChunk(buf, cookie, SCTP_COOKIE_SIZE);
    EXPECT_EQ(buf[0], SCTP_CHUNK_COOKIE_ECHO);
}

TEST_F(SctpPacketApiTest, writeCookieEchoChunk_cookieDataIntact)
{
    BYTE buf[128];
    BYTE cookie[SCTP_COOKIE_SIZE];
    for (UINT32 i = 0; i < SCTP_COOKIE_SIZE; i++) {
        cookie[i] = (BYTE)(i * 3);
    }

    sctpWriteCookieEchoChunk(buf, cookie, SCTP_COOKIE_SIZE);
    EXPECT_EQ(0, MEMCMP(buf + SCTP_CHUNK_HEADER_SIZE, cookie, SCTP_COOKIE_SIZE));
}

/******************************************************************************
 * COOKIE-ACK Chunk
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeCookieAckChunk_returns4)
{
    BYTE buf[16];
    EXPECT_EQ(sctpWriteCookieAckChunk(buf), (UINT32) SCTP_COOKIE_ACK_SIZE);
}

TEST_F(SctpPacketApiTest, writeCookieAckChunk_chunkType)
{
    BYTE buf[16];
    sctpWriteCookieAckChunk(buf);
    EXPECT_EQ(buf[0], SCTP_CHUNK_COOKIE_ACK);
}

/******************************************************************************
 * DATA Chunk
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeDataChunk_returnsCorrectSize)
{
    BYTE buf[256];
    BYTE payload[] = "hello";
    UINT32 payloadLen = 5;

    UINT32 size = sctpWriteDataChunk(buf, 1, 0, 0, 51, FALSE, payload, payloadLen);
    // DATA_HEADER_SIZE(16) + 5 = 21, padded to 24
    EXPECT_EQ(size, SCTP_PAD4(SCTP_DATA_HEADER_SIZE + payloadLen));
}

TEST_F(SctpPacketApiTest, writeDataChunk_chunkType)
{
    BYTE buf[256];
    BYTE payload[] = "test";
    sctpWriteDataChunk(buf, 1, 0, 0, 51, FALSE, payload, 4);
    EXPECT_EQ(buf[0], SCTP_CHUNK_DATA);
}

TEST_F(SctpPacketApiTest, writeDataChunk_orderedFlags)
{
    BYTE buf[256];
    BYTE payload[] = "test";
    sctpWriteDataChunk(buf, 1, 0, 0, 51, FALSE, payload, 4);
    // Ordered: B+E flags only
    EXPECT_EQ(buf[1], (SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END));
}

TEST_F(SctpPacketApiTest, writeDataChunk_unorderedFlags)
{
    BYTE buf[256];
    BYTE payload[] = "test";
    sctpWriteDataChunk(buf, 1, 0, 0, 51, TRUE, payload, 4);
    // Unordered: B+E+U flags
    EXPECT_EQ(buf[1], (SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END | SCTP_DATA_FLAG_UNORDERED));
}

TEST_F(SctpPacketApiTest, writeDataChunk_tsnEncoded)
{
    BYTE buf[256];
    BYTE payload[] = "test";
    sctpWriteDataChunk(buf, 0xAABBCCDD, 0, 0, 51, FALSE, payload, 4);

    UINT32 tsn = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 4));
    EXPECT_EQ(tsn, (UINT32) 0xAABBCCDD);
}

TEST_F(SctpPacketApiTest, writeDataChunk_streamIdSsnPpid)
{
    BYTE buf[256];
    BYTE payload[] = "test";
    sctpWriteDataChunk(buf, 1, 42, 7, 53, FALSE, payload, 4);

    UINT16 sid = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 8));
    UINT16 ssn = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 10));
    UINT32 ppid = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 12));

    EXPECT_EQ(sid, 42);
    EXPECT_EQ(ssn, 7);
    EXPECT_EQ(ppid, (UINT32) 53);
}

TEST_F(SctpPacketApiTest, writeDataChunk_payloadIntact)
{
    BYTE buf[256];
    BYTE payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    sctpWriteDataChunk(buf, 1, 0, 0, 51, FALSE, payload, 5);

    EXPECT_EQ(0, MEMCMP(buf + SCTP_DATA_HEADER_SIZE, payload, 5));
}

TEST_F(SctpPacketApiTest, writeDataChunk_paddingApplied)
{
    BYTE buf[256];
    BYTE payload[] = {0x01, 0x02, 0x03, 0x04, 0x05}; // 5 bytes — needs padding to 8
    UINT32 size = sctpWriteDataChunk(buf, 1, 0, 0, 51, FALSE, payload, 5);
    // 16 + 5 = 21, padded to 24
    EXPECT_EQ(size, (UINT32) 24);
    EXPECT_EQ(size % 4, (UINT32) 0);
}

/******************************************************************************
 * SACK Chunk
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeSackChunk_noGaps)
{
    BYTE buf[128];
    UINT32 size = sctpWriteSackChunk(buf, 100, 131072, NULL, NULL, 0);
    EXPECT_EQ(size, (UINT32) SCTP_SACK_HEADER_SIZE);
}

TEST_F(SctpPacketApiTest, writeSackChunk_cumTsnEncoded)
{
    BYTE buf[128];
    sctpWriteSackChunk(buf, 0xAABBCCDD, 131072, NULL, NULL, 0);

    UINT32 cumTsn = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 4));
    EXPECT_EQ(cumTsn, (UINT32) 0xAABBCCDD);
}

TEST_F(SctpPacketApiTest, writeSackChunk_arwndEncoded)
{
    BYTE buf[128];
    sctpWriteSackChunk(buf, 100, 65536, NULL, NULL, 0);

    UINT32 arwnd = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 8));
    EXPECT_EQ(arwnd, (UINT32) 65536);
}

TEST_F(SctpPacketApiTest, writeSackChunk_withGapBlocks)
{
    BYTE buf[128];
    UINT16 gapStarts[] = {3};
    UINT16 gapEnds[] = {5};

    UINT32 size = sctpWriteSackChunk(buf, 100, 131072, gapStarts, gapEnds, 1);
    // SACK_HEADER_SIZE(16) + 1 gap * 4 = 20
    EXPECT_EQ(size, (UINT32) 20);

    UINT16 numGaps = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 12));
    EXPECT_EQ(numGaps, (UINT16) 1);

    UINT16 gStart = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 16));
    UINT16 gEnd = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 18));
    EXPECT_EQ(gStart, (UINT16) 3);
    EXPECT_EQ(gEnd, (UINT16) 5);
}

TEST_F(SctpPacketApiTest, writeSackChunk_multipleGaps)
{
    BYTE buf[128];
    UINT16 gapStarts[] = {2, 5, 8};
    UINT16 gapEnds[] = {3, 6, 10};

    UINT32 size = sctpWriteSackChunk(buf, 100, 131072, gapStarts, gapEnds, 3);
    // SACK_HEADER_SIZE(16) + 3 gaps * 4 = 28
    EXPECT_EQ(size, (UINT32) 28);

    UINT16 numGaps = (UINT16) getUnalignedInt16BigEndian((PINT16)(buf + 12));
    EXPECT_EQ(numGaps, (UINT16) 3);
}

TEST_F(SctpPacketApiTest, writeSackChunk_roundtripParseable)
{
    BYTE packet[256];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    off += sctpWriteSackChunk(packet + off, 100, 131072, NULL, NULL, 0);
    sctpFinalizePacket(packet, off);

    UINT8 foundType = 0;
    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        *(UINT8*) customData = type;
        return STATUS_SUCCESS;
    };

    EXPECT_EQ(sctpParseChunks(packet, off, callback, (UINT64) &foundType), STATUS_SUCCESS);
    EXPECT_EQ(foundType, SCTP_CHUNK_SACK);
}

/******************************************************************************
 * FORWARD-TSN Chunk
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeForwardTsnChunk_returnsCorrectSize)
{
    BYTE buf[16];
    EXPECT_EQ(sctpWriteForwardTsnChunk(buf, 100), (UINT32) SCTP_FORWARD_TSN_HEADER_SIZE);
}

TEST_F(SctpPacketApiTest, writeForwardTsnChunk_chunkType)
{
    BYTE buf[16];
    sctpWriteForwardTsnChunk(buf, 100);
    EXPECT_EQ(buf[0], SCTP_CHUNK_FORWARD_TSN);
}

TEST_F(SctpPacketApiTest, writeForwardTsnChunk_newCumTsnEncoded)
{
    BYTE buf[16];
    sctpWriteForwardTsnChunk(buf, 0xDEADBEEF);

    UINT32 tsn = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 4));
    EXPECT_EQ(tsn, (UINT32) 0xDEADBEEF);
}

/******************************************************************************
 * SHUTDOWN Family
 *****************************************************************************/

TEST_F(SctpPacketApiTest, writeShutdownChunk_returns8)
{
    BYTE buf[16];
    EXPECT_EQ(sctpWriteShutdownChunk(buf, 100), (UINT32) SCTP_SHUTDOWN_SIZE);
}

TEST_F(SctpPacketApiTest, writeShutdownChunk_cumTsnEncoded)
{
    BYTE buf[16];
    sctpWriteShutdownChunk(buf, 0x11223344);

    EXPECT_EQ(buf[0], SCTP_CHUNK_SHUTDOWN);
    UINT32 tsn = (UINT32) getUnalignedInt32BigEndian((PINT32)(buf + 4));
    EXPECT_EQ(tsn, (UINT32) 0x11223344);
}

TEST_F(SctpPacketApiTest, writeShutdownAckChunk_returns4)
{
    BYTE buf[16];
    EXPECT_EQ(sctpWriteShutdownAckChunk(buf), (UINT32) SCTP_SHUTDOWN_ACK_SIZE);
}

TEST_F(SctpPacketApiTest, writeShutdownAckChunk_chunkType)
{
    BYTE buf[16];
    sctpWriteShutdownAckChunk(buf);
    EXPECT_EQ(buf[0], SCTP_CHUNK_SHUTDOWN_ACK);
}

TEST_F(SctpPacketApiTest, writeShutdownCompleteChunk_returns4)
{
    BYTE buf[16];
    EXPECT_EQ(sctpWriteShutdownCompleteChunk(buf), (UINT32) SCTP_SHUTDOWN_COMPLETE_SIZE);
}

TEST_F(SctpPacketApiTest, writeShutdownCompleteChunk_chunkType)
{
    BYTE buf[16];
    sctpWriteShutdownCompleteChunk(buf);
    EXPECT_EQ(buf[0], SCTP_CHUNK_SHUTDOWN_COMPLETE);
}

/******************************************************************************
 * Packet Validation
 *****************************************************************************/

TEST_F(SctpPacketApiTest, validatePacket_nullBuffer)
{
    EXPECT_NE(sctpValidatePacket(NULL, 12, 0), STATUS_SUCCESS);
}

TEST_F(SctpPacketApiTest, validatePacket_tooShort)
{
    BYTE buf[8] = {0};
    EXPECT_NE(sctpValidatePacket(buf, 8, 0), STATUS_SUCCESS);
}

TEST_F(SctpPacketApiTest, validatePacket_badCrc)
{
    BYTE packet[64];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    sctpFinalizePacket(packet, off);

    // Corrupt one byte
    packet[0] ^= 0xFF;
    EXPECT_NE(sctpValidatePacket(packet, off, 0x12345678), STATUS_SUCCESS);
}

TEST_F(SctpPacketApiTest, validatePacket_vtagMismatch)
{
    BYTE packet[64];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    sctpFinalizePacket(packet, off);

    // Expect different vtag
    EXPECT_NE(sctpValidatePacket(packet, off, 0xAAAAAAAA), STATUS_SUCCESS);
}

TEST_F(SctpPacketApiTest, validatePacket_initVtagZeroAllowed)
{
    BYTE packet[64];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0); // vtag=0 for INIT
    sctpFinalizePacket(packet, off);

    // Should pass regardless of expectedVtag when packet vtag is 0
    EXPECT_EQ(sctpValidatePacket(packet, off, 0x12345678), STATUS_SUCCESS);
}

TEST_F(SctpPacketApiTest, validatePacket_validPacket)
{
    BYTE packet[64];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    sctpFinalizePacket(packet, off);

    EXPECT_EQ(sctpValidatePacket(packet, off, 0x12345678), STATUS_SUCCESS);
}

TEST_F(SctpPacketApiTest, validatePacket_crcRestoredAfterValidation)
{
    BYTE packet[64];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    sctpFinalizePacket(packet, off);

    // Save original CRC bytes
    BYTE origCrc[4];
    MEMCPY(origCrc, packet + 8, 4);

    sctpValidatePacket(packet, off, 0x12345678);

    // CRC should be restored
    EXPECT_EQ(0, MEMCMP(packet + 8, origCrc, 4));
}

/******************************************************************************
 * Chunk Parsing
 *****************************************************************************/

TEST_F(SctpPacketApiTest, parseChunks_singleChunk)
{
    BYTE packet[256];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0);
    off += sctpWriteInitChunk(packet + off, 0x11111111, 131072, 300, 300, 0x11111111);

    UINT32 callCount = 0;
    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        (*(UINT32*) customData)++;
        return STATUS_SUCCESS;
    };

    sctpParseChunks(packet, off, callback, (UINT64) &callCount);
    EXPECT_EQ(callCount, (UINT32) 1);
}

TEST_F(SctpPacketApiTest, parseChunks_multipleChunks)
{
    BYTE packet[256];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    off += sctpWriteCookieAckChunk(packet + off);
    off += sctpWriteShutdownCompleteChunk(packet + off);

    // Note: COOKIE_ACK is only 4 bytes and has length=4 which equals header size.
    // sctpParseChunks requires chunkLen >= 4, so chunkLen=4 should work.
    // But valueLen = chunkLen - 4 = 0, which means the chunk body is empty. That's valid for COOKIE-ACK.
    // The second chunk starts at offset 12 + PAD4(4) = 16.
    UINT32 callCount = 0;
    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        (*(UINT32*) customData)++;
        return STATUS_SUCCESS;
    };

    sctpParseChunks(packet, off, callback, (UINT64) &callCount);
    EXPECT_EQ(callCount, (UINT32) 2);
}

TEST_F(SctpPacketApiTest, parseChunks_truncatedChunk)
{
    BYTE packet[256];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0);
    // Write a chunk header claiming 100 bytes but only provide 20 total
    packet[off] = SCTP_CHUNK_DATA;
    packet[off + 1] = 0;
    putUnalignedInt16BigEndian(packet + off + 2, (INT16) 100); // claims 100 bytes
    off += 4;

    UINT32 callCount = 0;
    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        (*(UINT32*) customData)++;
        return STATUS_SUCCESS;
    };

    // Should not call the callback since chunk extends beyond packet
    sctpParseChunks(packet, off, callback, (UINT64) &callCount);
    EXPECT_EQ(callCount, (UINT32) 0);
}

TEST_F(SctpPacketApiTest, parseChunks_chunkLenTooSmall)
{
    BYTE packet[256];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0);
    // Write a chunk header with length < 4
    packet[off] = SCTP_CHUNK_DATA;
    packet[off + 1] = 0;
    putUnalignedInt16BigEndian(packet + off + 2, (INT16) 2); // too small
    off += 20; // enough bytes exist

    UINT32 callCount = 0;
    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        (*(UINT32*) customData)++;
        return STATUS_SUCCESS;
    };

    sctpParseChunks(packet, off, callback, (UINT64) &callCount);
    EXPECT_EQ(callCount, (UINT32) 0);
}

TEST_F(SctpPacketApiTest, parseChunks_callbackErrorStopsParsing)
{
    BYTE packet[256];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    off += sctpWriteCookieAckChunk(packet + off);
    off += sctpWriteShutdownCompleteChunk(packet + off);

    UINT32 callCount = 0;
    auto callback = [](UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData) -> STATUS {
        (*(UINT32*) customData)++;
        return STATUS_SCTP_INVALID_DCEP_PACKET; // return error to stop
    };

    // Should stop after first chunk
    sctpParseChunks(packet, off, callback, (UINT64) &callCount);
    EXPECT_EQ(callCount, (UINT32) 1);
}

/******************************************************************************
 * TSN/SSN Serial Number Arithmetic (matches Rust dcsctp types.rs tests)
 *
 * Our C code uses these macros in SctpAssoc.c:
 *   #define TSN_LT(a, b)  ((INT32)((a) - (b)) < 0)
 *   #define TSN_LTE(a, b) ((INT32)((a) - (b)) <= 0)
 *   #define TSN_GT(a, b)  ((INT32)((a) - (b)) > 0)
 *   #define TSN_GTE(a, b) ((INT32)((a) - (b)) >= 0)
 * We replicate them here for direct unit-testing of the arithmetic.
 *****************************************************************************/

#define TEST_TSN_LT(a, b)  ((INT32)((UINT32)(a) - (UINT32)(b)) < 0)
#define TEST_TSN_LTE(a, b) ((INT32)((UINT32)(a) - (UINT32)(b)) <= 0)
#define TEST_TSN_GT(a, b)  ((INT32)((UINT32)(a) - (UINT32)(b)) > 0)
#define TEST_TSN_GTE(a, b) ((INT32)((UINT32)(a) - (UINT32)(b)) >= 0)

// Rust: tsn_cmp — Tsn(1) < Tsn(2), Tsn(0xFFFFFFFF) < Tsn(1), Tsn(5) > Tsn(0xFFFFFFFE)
TEST_F(SctpPacketApiTest, tsnArithmetic_comparison)
{
    // Normal ordering
    EXPECT_TRUE(TEST_TSN_LT(1, 2));
    EXPECT_FALSE(TEST_TSN_LT(2, 1));
    EXPECT_TRUE(TEST_TSN_GT(2, 1));

    // Equal
    EXPECT_FALSE(TEST_TSN_LT(5, 5));
    EXPECT_TRUE(TEST_TSN_LTE(5, 5));
    EXPECT_TRUE(TEST_TSN_GTE(5, 5));

    // Wraparound: 0xFFFFFFFF is "before" 1 (distance 2 forward)
    EXPECT_TRUE(TEST_TSN_LT(0xFFFFFFFF, 1));
    EXPECT_FALSE(TEST_TSN_GT(0xFFFFFFFF, 1));

    // Wraparound: 5 is "after" 0xFFFFFFFE (distance 7 forward)
    EXPECT_TRUE(TEST_TSN_GT(5, 0xFFFFFFFE));
    EXPECT_FALSE(TEST_TSN_LT(5, 0xFFFFFFFE));
}

// Rust: tsn_next_and_prev_value — Tsn(1).next()==Tsn(2), Tsn(0xFFFFFFFF).next()==Tsn(0), Tsn(0).prev()==Tsn(0xFFFFFFFF)
TEST_F(SctpPacketApiTest, tsnArithmetic_nextAndPrev)
{
    UINT32 tsn;

    // next(1) == 2
    tsn = 1;
    tsn++;
    EXPECT_EQ(tsn, (UINT32) 2);

    // next(0xFFFFFFFF) == 0 (wraparound)
    tsn = 0xFFFFFFFF;
    tsn++;
    EXPECT_EQ(tsn, (UINT32) 0);

    // prev(0) == 0xFFFFFFFF (wraparound)
    tsn = 0;
    tsn--;
    EXPECT_EQ(tsn, (UINT32) 0xFFFFFFFF);

    // prev(2) == 1
    tsn = 2;
    tsn--;
    EXPECT_EQ(tsn, (UINT32) 1);
}

// Rust: tsn_increment — Tsn(1) += 1 == Tsn(2), Tsn(0xFFFFFFFF) += 1 == Tsn(0)
TEST_F(SctpPacketApiTest, tsnArithmetic_increment)
{
    UINT32 tsn = 1;
    tsn += 1;
    EXPECT_EQ(tsn, (UINT32) 2);

    tsn = 0xFFFFFFFF;
    tsn += 1;
    EXPECT_EQ(tsn, (UINT32) 0);

    tsn = 0xFFFFFFF0;
    tsn += 20;
    EXPECT_EQ(tsn, (UINT32) 4);
}

// Rust: tsn_distance_to — Tsn(1).distance_to(Tsn(5)) == 4, Tsn(0xFFFFFFFE).distance_to(Tsn(1)) == 3
TEST_F(SctpPacketApiTest, tsnArithmetic_distanceTo)
{
    // Our code computes distance as: (UINT32)(b - a) for forward distance
    EXPECT_EQ((UINT32) (5 - 1), (UINT32) 4);

    // Wraparound distance: 0xFFFFFFFE → 1 = 3 steps forward
    EXPECT_EQ((UINT32) ((UINT32) 1 - (UINT32) 0xFFFFFFFE), (UINT32) 3);

    // Same TSN → distance 0
    EXPECT_EQ((UINT32) ((UINT32) 42 - (UINT32) 42), (UINT32) 0);
}

// Rust: ssn_cmp — Ssn(1) < Ssn(2), Ssn(0xFFFF) < Ssn(1)
TEST_F(SctpPacketApiTest, ssnArithmetic_comparison)
{
    // SSN uses UINT16 serial arithmetic: (INT16)(a - b) < 0
    #define TEST_SSN_LT(a, b) ((INT16)((UINT16)(a) - (UINT16)(b)) < 0)

    // Normal ordering
    EXPECT_TRUE(TEST_SSN_LT(1, 2));
    EXPECT_FALSE(TEST_SSN_LT(2, 1));

    // Wraparound: 0xFFFF is "before" 1
    EXPECT_TRUE(TEST_SSN_LT(0xFFFF, 1));
    EXPECT_FALSE(TEST_SSN_LT(1, 0xFFFF));

    // Equal
    EXPECT_FALSE(TEST_SSN_LT(100, 100));

    #undef TEST_SSN_LT
}

// Rust: ssn_next_and_prev_value — Ssn(0xFFFF).next()==Ssn(0), Ssn(0).prev()==Ssn(0xFFFF)
TEST_F(SctpPacketApiTest, ssnArithmetic_nextAndPrev)
{
    UINT16 ssn;

    // next(0xFFFF) == 0 (wraparound)
    ssn = 0xFFFF;
    ssn++;
    EXPECT_EQ(ssn, (UINT16) 0);

    // prev(0) == 0xFFFF (wraparound)
    ssn = 0;
    ssn--;
    EXPECT_EQ(ssn, (UINT16) 0xFFFF);

    // next(1) == 2
    ssn = 1;
    ssn++;
    EXPECT_EQ(ssn, (UINT16) 2);
}

// Rust: ssn_increment — Ssn(0xFFFF) += 1 == Ssn(0)
TEST_F(SctpPacketApiTest, ssnArithmetic_increment)
{
    UINT16 ssn = 0xFFFF;
    ssn = (UINT16)(ssn + 1);
    EXPECT_EQ(ssn, (UINT16) 0);

    ssn = 100;
    ssn = (UINT16)(ssn + 1);
    EXPECT_EQ(ssn, (UINT16) 101);
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
#endif
