#include "SctpTestHelpers.h"

#ifdef ENABLE_DATA_CHANNEL
namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class SctpCrc32cApiTest : public WebRtcClientTestBase {
};

// CRC32c (Castagnoli) test vectors

TEST_F(SctpCrc32cApiTest, crc32c_emptyInput)
{
    BYTE dummy = 0;
    // CRC32c of zero-length data should be 0x00000000
    EXPECT_EQ(sctpCrc32c(&dummy, 0), (UINT32) 0x00000000);
}

TEST_F(SctpCrc32cApiTest, crc32c_singleByteZero)
{
    BYTE data[] = {0x00};
    UINT32 crc = sctpCrc32c(data, 1);
    EXPECT_EQ(crc, (UINT32) 0x527D5351);
}

TEST_F(SctpCrc32cApiTest, crc32c_singleByte0xFF)
{
    BYTE data[] = {0xFF};
    UINT32 crc = sctpCrc32c(data, 1);
    EXPECT_NE(crc, (UINT32) 0x00000000);
    EXPECT_EQ(crc, (UINT32) 0xFF000000);
}

TEST_F(SctpCrc32cApiTest, crc32c_rfcTestVector_32Zeros)
{
    BYTE data[32];
    MEMSET(data, 0x00, 32);
    UINT32 crc = sctpCrc32c(data, 32);
    EXPECT_EQ(crc, (UINT32) 0x8A9136AA);
}

TEST_F(SctpCrc32cApiTest, crc32c_rfcTestVector_32_0xFF)
{
    BYTE data[32];
    MEMSET(data, 0xFF, 32);
    UINT32 crc = sctpCrc32c(data, 32);
    EXPECT_EQ(crc, (UINT32) 0x62A8AB43);
}

TEST_F(SctpCrc32cApiTest, crc32c_rfcTestVector_ascending)
{
    BYTE data[32];
    for (int i = 0; i < 32; i++) {
        data[i] = (BYTE) i;
    }
    UINT32 crc = sctpCrc32c(data, 32);
    EXPECT_EQ(crc, (UINT32) 0x46DD794E);
}

TEST_F(SctpCrc32cApiTest, crc32c_rfcTestVector_descending)
{
    BYTE data[32];
    for (int i = 0; i < 32; i++) {
        data[i] = (BYTE)(31 - i);
    }
    UINT32 crc = sctpCrc32c(data, 32);
    EXPECT_EQ(crc, (UINT32) 0x113FDB5C);
}

// Standard CRC32c check values — matches Rust dcsctp test_crc32c_vectors
TEST_F(SctpCrc32cApiTest, crc32c_standardCheckValue)
{
    // "123456789" — the canonical CRC32c check value
    BYTE data9[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(sctpCrc32c(data9, 9), (UINT32) 0xE3069283);

    // "1234" — 4 bytes (less than 8, exercises short-input path)
    BYTE data4[] = {'1', '2', '3', '4'};
    EXPECT_EQ(sctpCrc32c(data4, 4), (UINT32) 0xF63AF4EE);

    // "12345678" — exactly 8 bytes (one chunk boundary)
    BYTE data8[] = {'1', '2', '3', '4', '5', '6', '7', '8'};
    EXPECT_EQ(sctpCrc32c(data8, 8), (UINT32) 0x6087809A);
}

// Verify lookup table entries indirectly via single-byte CRC outputs.
// For a single byte b: CRC32c({b}) = 0xFF000000 ^ TABLE[0xFF ^ b]
// This lets us verify specific TABLE entries match RFC 9260 Appendix A:
//   TABLE[0]   = 0x00000000 → CRC({0xFF}) = 0xFF000000 ^ 0x00000000 = 0xFF000000
//   TABLE[1]   = 0xF26B8303 → CRC({0xFE}) = 0xFF000000 ^ 0xF26B8303 = 0x0D6B8303
//   TABLE[255] = 0xAD7D5351 → CRC({0x00}) = 0xFF000000 ^ 0xAD7D5351 = 0x527D5351
TEST_F(SctpCrc32cApiTest, crc32c_lookupTableSpotChecks)
{
    BYTE b0[] = {0x00};
    BYTE b1[] = {0x01};
    BYTE bFE[] = {0xFE};
    BYTE bFF[] = {0xFF};

    // Verifies TABLE[255] = 0xAD7D5351
    EXPECT_EQ(sctpCrc32c(b0, 1), (UINT32) 0x527D5351);
    // Verifies TABLE[254]
    EXPECT_EQ(sctpCrc32c(b1, 1), (UINT32) 0xA016D052);
    // Verifies TABLE[1] = 0xF26B8303
    EXPECT_EQ(sctpCrc32c(bFE, 1), (UINT32) 0x0D6B8303);
    // Verifies TABLE[0] = 0x00000000
    EXPECT_EQ(sctpCrc32c(bFF, 1), (UINT32) 0xFF000000);
}

// Matches Rust dcsctp test_vs_reference: verify CRC across many lengths with
// deterministic data. Our C implementation is byte-at-a-time, so we validate
// against precomputed reference values derived from the RFC 9260 algorithm.
TEST_F(SctpCrc32cApiTest, crc32c_crossValidateMultipleLengths)
{
    // Precomputed CRC32c values for data[i] = (i * 17) & 0xFF at various lengths
    static const struct {
        UINT32 len;
        UINT32 expected;
    } vectors[] = {
        {0, 0x00000000},   {1, 0x527D5351},   {2, 0x135433BE},   {3, 0x8D8534A5},
        {4, 0xE6F9CD9A},   {5, 0x2D30F410},   {6, 0x26DA6A05},   {7, 0x20CDE9AD},
        {8, 0xEA6C572B},   {9, 0xE38C7954},   {10, 0x2E3BB53F},  {11, 0xF50A80EF},
        {12, 0xC437141D},  {13, 0x730106ED},   {14, 0x62ED1BE6},  {15, 0xD8C6E685},
        {16, 0x48DFE982},  {17, 0x21A60058},   {18, 0x5B767A90},  {19, 0x1156E07A},
        {20, 0x1A3D97CC},
    };

    for (UINT32 v = 0; v < ARRAY_SIZE(vectors); v++) {
        BYTE data[100];
        for (UINT32 i = 0; i < vectors[v].len; i++) {
            data[i] = (BYTE) ((i * 17) & 0xFF);
        }
        BYTE dummy = 0;
        PBYTE ptr = vectors[v].len > 0 ? data : &dummy;
        EXPECT_EQ(sctpCrc32c(ptr, vectors[v].len), vectors[v].expected);
    }

    // 1024-byte buffer with data[i] = (i * 13) & 0xFF
    BYTE largeData[1024];
    for (UINT32 i = 0; i < 1024; i++) {
        largeData[i] = (BYTE) ((i * 13) & 0xFF);
    }
    EXPECT_EQ(sctpCrc32c(largeData, 1024), (UINT32) 0x9040F0D6);
}

TEST_F(SctpCrc32cApiTest, crc32c_knownSctpPacket)
{
    // Build a simple SCTP common header and verify the CRC
    BYTE packet[SCTP_COMMON_HEADER_SIZE];
    sctpWriteCommonHeader(packet, 5000, 5000, 0x12345678);
    // Zero out the checksum field
    packet[8] = packet[9] = packet[10] = packet[11] = 0;

    UINT32 crc = sctpCrc32c(packet, SCTP_COMMON_HEADER_SIZE);
    EXPECT_NE(crc, (UINT32) 0x00000000);

    // Finalize and verify the stored CRC matches
    sctpFinalizePacket(packet, SCTP_COMMON_HEADER_SIZE);

    // Read stored CRC (little-endian)
    UINT32 storedCrc = (UINT32) packet[8] | ((UINT32) packet[9] << 8) | ((UINT32) packet[10] << 16) | ((UINT32) packet[11] << 24);
    EXPECT_EQ(storedCrc, crc);
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
#endif
