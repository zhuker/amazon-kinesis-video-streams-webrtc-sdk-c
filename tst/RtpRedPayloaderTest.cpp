#include "WebRTCClientTestFixture.h"
#include "src/source/Rtp/Codecs/RtpRedPayloader.h"

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class RtpRedPayloaderTest : public WebRtcClientTestBase {};

// Helper: run the two-call size-then-fill pattern and return the final body bytes.
static STATUS packOpusRed(UINT32 mtu, const std::vector<BYTE>& opus, UINT32 rtpTs, PRedSenderState pState, std::vector<BYTE>& outBody,
                          BOOL& outFallback)
{
    UINT32 bodyLen = 0;
    UINT32 subLen = 0;
    UINT32 subLenSize = 1;
    BOOL fallback = FALSE;
    STATUS s = createPayloadForOpusRed(mtu, (PBYTE) opus.data(), (UINT32) opus.size(), rtpTs, pState, NULL, &bodyLen, NULL, &subLenSize, &fallback);
    if (STATUS_FAILED(s)) {
        return s;
    }
    outBody.assign(bodyLen, 0);
    UINT32 cap = bodyLen;
    UINT32 capSub = 1;
    s = createPayloadForOpusRed(mtu, (PBYTE) opus.data(), (UINT32) opus.size(), rtpTs, pState, outBody.data(), &cap, &subLen, &capSub, &fallback);
    outFallback = fallback;
    return s;
}

// ---------------- Pack tests ----------------

TEST_F(RtpRedPayloaderTest, packFirstPacketHasNoRedundancy)
{
    PRedSenderState pState = NULL;
    EXPECT_EQ(STATUS_SUCCESS, createRedSenderState(1, 111, &pState));

    std::vector<BYTE> opus = {0x01, 0x02, 0x03, 0x04};
    std::vector<BYTE> body;
    BOOL fallback = FALSE;
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, opus, 10000, pState, body, fallback));
    EXPECT_FALSE(fallback);

    // Expected: [F=0|111][opus bytes]
    ASSERT_EQ(1u + opus.size(), body.size());
    EXPECT_EQ(0x6F, body[0]); // 111 = 0x6F, F=0
    EXPECT_EQ(0x01, body[1]);
    EXPECT_EQ(0x02, body[2]);
    EXPECT_EQ(0x03, body[3]);
    EXPECT_EQ(0x04, body[4]);

    freeRedSenderState(&pState);
}

TEST_F(RtpRedPayloaderTest, packWithOnePriorFrame)
{
    PRedSenderState pState = NULL;
    EXPECT_EQ(STATUS_SUCCESS, createRedSenderState(1, 111, &pState));

    std::vector<BYTE> frame0 = {0xAA, 0xBB, 0xCC};
    std::vector<BYTE> body0;
    BOOL fallback = FALSE;
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, frame0, 48000, pState, body0, fallback));

    std::vector<BYTE> frame1 = {0x11, 0x22, 0x33, 0x44};
    std::vector<BYTE> body1;
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, frame1, 48000 + 960, pState, body1, fallback));
    EXPECT_FALSE(fallback);

    // Expected layout:
    //   [F=1|111][tsDelta_hi][(tsDelta_lo<<2)|(len>>8)][len&0xFF]      <- redundant hdr (4 bytes)
    //   [F=0|111]                                                       <- primary hdr (1 byte)
    //   [frame0 bytes 3]                                                <- redundant payload
    //   [frame1 bytes 4]                                                <- primary payload
    // tsDelta = 960 (14 bits). len = 3 (10 bits).
    ASSERT_EQ(4u + 1u + frame0.size() + frame1.size(), body1.size());
    EXPECT_EQ(0xEF, body1[0]); // F=1 | 111
    // 960 = 0x3C0. hi = 0x0F (960 >> 6 = 15). lo = 0x0 ((960 & 0x3F) = 0). len[9:8] = 0.
    EXPECT_EQ(0x0F, body1[1]);
    EXPECT_EQ(0x00, body1[2]);
    EXPECT_EQ(0x03, body1[3]);
    EXPECT_EQ(0x6F, body1[4]); // F=0 primary
    EXPECT_EQ(0xAA, body1[5]);
    EXPECT_EQ(0xBB, body1[6]);
    EXPECT_EQ(0xCC, body1[7]);
    EXPECT_EQ(0x11, body1[8]);
    EXPECT_EQ(0x22, body1[9]);
    EXPECT_EQ(0x33, body1[10]);
    EXPECT_EQ(0x44, body1[11]);

    freeRedSenderState(&pState);
}

TEST_F(RtpRedPayloaderTest, packWithTwoPriorFrames)
{
    PRedSenderState pState = NULL;
    EXPECT_EQ(STATUS_SUCCESS, createRedSenderState(2, 111, &pState));

    std::vector<BYTE> f0 = {0xA0};
    std::vector<BYTE> f1 = {0xA1, 0xA2};
    std::vector<BYTE> f2 = {0xA3, 0xA4, 0xA5};
    std::vector<BYTE> body;
    BOOL fb = FALSE;

    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, f0, 1000, pState, body, fb));
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, f1, 2000, pState, body, fb));
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, f2, 3000, pState, body, fb));
    EXPECT_FALSE(fb);

    // Two redundant blocks + primary. Redundant blocks in oldest-first order: f0, f1.
    ASSERT_EQ(4u + 4u + 1u + f0.size() + f1.size() + f2.size(), body.size());
    // First redundant (f0, tsDelta=2000, len=1).
    // 2000 = 0x7D0. hi=2000>>6=31=0x1F; lo=(2000&0x3F)=16; byte[2]=(16<<2)|(len>>8)=0x40.
    EXPECT_EQ(0xEF, body[0]);
    EXPECT_EQ(0x1F, body[1]);
    EXPECT_EQ(0x40, body[2]);
    EXPECT_EQ(0x01, body[3]);
    // Second redundant (f1, tsDelta=1000, len=2)
    EXPECT_EQ(0xEF, body[4]);
    // 1000 = 0x3E8. hi = 0x0F (1000>>6=15), lo = 1000&0x3F = 40 = 0x28, <<2 = 0xA0
    EXPECT_EQ(0x0F, body[5]);
    EXPECT_EQ(0xA0, body[6]);
    EXPECT_EQ(0x02, body[7]);
    // Primary header
    EXPECT_EQ(0x6F, body[8]);
    // Payloads: f0, f1, f2
    EXPECT_EQ(0xA0, body[9]);
    EXPECT_EQ(0xA1, body[10]);
    EXPECT_EQ(0xA2, body[11]);
    EXPECT_EQ(0xA3, body[12]);
    EXPECT_EQ(0xA4, body[13]);
    EXPECT_EQ(0xA5, body[14]);

    freeRedSenderState(&pState);
}

TEST_F(RtpRedPayloaderTest, packDropsSlotsExceeding14BitOffset)
{
    PRedSenderState pState = NULL;
    EXPECT_EQ(STATUS_SUCCESS, createRedSenderState(1, 111, &pState));

    std::vector<BYTE> old = {0xDE, 0xAD};
    std::vector<BYTE> body;
    BOOL fb = FALSE;
    // First frame — goes into ring.
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, old, 1000, pState, body, fb));

    // Second frame with timestamp 20000 samples later — offset 19000 > 16383.
    std::vector<BYTE> cur = {0xBE, 0xEF};
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, cur, 1000 + 19000, pState, body, fb));
    EXPECT_FALSE(fb);

    // Expect no redundancy: just [F=0|111][cur bytes].
    ASSERT_EQ(1u + cur.size(), body.size());
    EXPECT_EQ(0x6F, body[0]);

    freeRedSenderState(&pState);
}

TEST_F(RtpRedPayloaderTest, packFallsBackToPlainOpusWhenPrimaryOver1023)
{
    PRedSenderState pState = NULL;
    EXPECT_EQ(STATUS_SUCCESS, createRedSenderState(1, 111, &pState));

    std::vector<BYTE> big(1100, 0x55);
    std::vector<BYTE> body;
    BOOL fb = FALSE;
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(2000, big, 100, pState, body, fb));
    EXPECT_TRUE(fb);
    ASSERT_EQ(big.size(), body.size());
    EXPECT_EQ(0, MEMCMP(body.data(), big.data(), big.size()));

    // Fallback must NOT update the ring: subsequent small frame still has no redundancy.
    std::vector<BYTE> small = {0x01};
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(2000, small, 200, pState, body, fb));
    EXPECT_FALSE(fb);
    ASSERT_EQ(2u, body.size()); // [F=0][0x01]
    EXPECT_EQ(0x6F, body[0]);

    freeRedSenderState(&pState);
}

TEST_F(RtpRedPayloaderTest, packMtuBudgetLimitsRedundancy)
{
    PRedSenderState pState = NULL;
    EXPECT_EQ(STATUS_SUCCESS, createRedSenderState(1, 111, &pState));

    // Fill the ring with a 100-byte frame.
    std::vector<BYTE> big(100, 0x77);
    std::vector<BYTE> body;
    BOOL fb = FALSE;
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(1200, big, 1000, pState, body, fb));

    // Send a second frame with a tight MTU: 50-byte budget minus 24+10 overhead leaves negative.
    // With mtu=50, mtuBudget=50-10-24=16. Can't fit the 100-byte redundancy (4+100=104).
    std::vector<BYTE> cur = {0x08};
    EXPECT_EQ(STATUS_SUCCESS, packOpusRed(50, cur, 2000, pState, body, fb));
    EXPECT_FALSE(fb);
    // No redundancy fit. Just [F=0][payload].
    ASSERT_EQ(2u, body.size());

    freeRedSenderState(&pState);
}

// ---------------- Split tests ----------------

// Helper: build a minimal RtpPacket with given body bytes as payload.
static PRtpPacket makeRedRtpPacket(UINT8 redPt, UINT16 seq, UINT32 ts, UINT32 ssrc, const std::vector<BYTE>& body)
{
    PRtpPacket p = (PRtpPacket) MEMCALLOC(1, SIZEOF(RtpPacket));
    p->header.version = 2;
    p->header.payloadType = redPt;
    p->header.sequenceNumber = seq;
    p->header.timestamp = ts;
    p->header.ssrc = ssrc;
    p->pRawPacket = (PBYTE) MEMALLOC(body.size());
    MEMCPY(p->pRawPacket, body.data(), body.size());
    p->rawPacketLength = (UINT32) body.size();
    p->payload = p->pRawPacket;
    p->payloadLength = (UINT32) body.size();
    return p;
}

TEST_F(RtpRedPayloaderTest, splitPrimaryOnly)
{
    // Body = [0x6F][0x01 0x02 0x03]
    std::vector<BYTE> body = {0x6F, 0x01, 0x02, 0x03};
    PRtpPacket pRed = makeRedRtpPacket(63, 100, 5000, 0xABCD, body);

    PRtpPacket out[RED_MAX_BLOCKS] = {};
    UINT32 produced = 0;
    UINT32 fecBytes = 0;
    EXPECT_EQ(STATUS_SUCCESS, splitRedRtpPacket(pRed, 111, out, RED_MAX_BLOCKS, &produced, &fecBytes));
    EXPECT_EQ(1u, produced);
    EXPECT_EQ(0u, fecBytes);

    ASSERT_NE(nullptr, out[0]);
    EXPECT_EQ(111, out[0]->header.payloadType);
    EXPECT_EQ(5000u, out[0]->header.timestamp);
    EXPECT_EQ(100u, out[0]->header.sequenceNumber);
    EXPECT_FALSE(out[0]->isSynthetic);
    ASSERT_EQ(3u, out[0]->payloadLength);
    EXPECT_EQ(0x01, out[0]->payload[0]);
    EXPECT_EQ(0x02, out[0]->payload[1]);
    EXPECT_EQ(0x03, out[0]->payload[2]);

    freeRtpPacket(&out[0]);
    freeRtpPacket(&pRed);
}

TEST_F(RtpRedPayloaderTest, splitOneRedundantPlusPrimary)
{
    // Pack via the encoder to produce a known-good body, then split and compare.
    PRedSenderState pState = NULL;
    createRedSenderState(1, 111, &pState);

    std::vector<BYTE> f0 = {0xA1, 0xA2, 0xA3};
    std::vector<BYTE> f1 = {0xB1, 0xB2};
    std::vector<BYTE> body0, body1;
    BOOL fb = FALSE;
    packOpusRed(1200, f0, 1000, pState, body0, fb);
    packOpusRed(1200, f1, 1960, pState, body1, fb);

    PRtpPacket pRed = makeRedRtpPacket(63, 42, 1960, 0xABCD, body1);

    PRtpPacket out[RED_MAX_BLOCKS] = {};
    UINT32 produced = 0;
    UINT32 fecBytes = 0;
    EXPECT_EQ(STATUS_SUCCESS, splitRedRtpPacket(pRed, 111, out, RED_MAX_BLOCKS, &produced, &fecBytes));
    EXPECT_EQ(2u, produced);
    EXPECT_EQ(f0.size(), fecBytes);

    // Redundant first
    ASSERT_NE(nullptr, out[0]);
    EXPECT_TRUE(out[0]->isSynthetic);
    EXPECT_EQ(1000u, out[0]->header.timestamp);
    EXPECT_EQ(41u, out[0]->header.sequenceNumber);
    ASSERT_EQ(f0.size(), out[0]->payloadLength);
    EXPECT_EQ(0, MEMCMP(out[0]->payload, f0.data(), f0.size()));

    // Primary
    ASSERT_NE(nullptr, out[1]);
    EXPECT_FALSE(out[1]->isSynthetic);
    EXPECT_EQ(1960u, out[1]->header.timestamp);
    EXPECT_EQ(42u, out[1]->header.sequenceNumber);
    ASSERT_EQ(f1.size(), out[1]->payloadLength);
    EXPECT_EQ(0, MEMCMP(out[1]->payload, f1.data(), f1.size()));

    freeRtpPacket(&out[0]);
    freeRtpPacket(&out[1]);
    freeRtpPacket(&pRed);
    freeRedSenderState(&pState);
}

TEST_F(RtpRedPayloaderTest, splitTwoRedundantPlusPrimary)
{
    PRedSenderState pState = NULL;
    createRedSenderState(2, 111, &pState);

    std::vector<BYTE> f0 = {0xC0};
    std::vector<BYTE> f1 = {0xC1, 0xC1};
    std::vector<BYTE> f2 = {0xC2, 0xC2, 0xC2};
    std::vector<BYTE> body0, body1, body2;
    BOOL fb = FALSE;
    packOpusRed(1200, f0, 1000, pState, body0, fb);
    packOpusRed(1200, f1, 2000, pState, body1, fb);
    packOpusRed(1200, f2, 3000, pState, body2, fb);

    PRtpPacket pRed = makeRedRtpPacket(63, 100, 3000, 0xABCD, body2);
    PRtpPacket out[RED_MAX_BLOCKS] = {};
    UINT32 produced = 0;
    UINT32 fecBytes = 0;
    EXPECT_EQ(STATUS_SUCCESS, splitRedRtpPacket(pRed, 111, out, RED_MAX_BLOCKS, &produced, &fecBytes));
    EXPECT_EQ(3u, produced);

    // Oldest first: f0@ts1000, f1@ts2000, f2(primary)@ts3000.
    EXPECT_EQ(1000u, out[0]->header.timestamp);
    EXPECT_EQ(98u, out[0]->header.sequenceNumber); // 100 - 2
    EXPECT_TRUE(out[0]->isSynthetic);
    EXPECT_EQ(2000u, out[1]->header.timestamp);
    EXPECT_EQ(99u, out[1]->header.sequenceNumber);
    EXPECT_TRUE(out[1]->isSynthetic);
    EXPECT_EQ(3000u, out[2]->header.timestamp);
    EXPECT_EQ(100u, out[2]->header.sequenceNumber);
    EXPECT_FALSE(out[2]->isSynthetic);

    for (UINT32 i = 0; i < produced; i++) {
        freeRtpPacket(&out[i]);
    }
    freeRtpPacket(&pRed);
    freeRedSenderState(&pState);
}

TEST_F(RtpRedPayloaderTest, splitMalformedTruncated)
{
    // Redundant header claims len=100 but body has only 10 bytes after header.
    // Header: F=1|111, tsDelta=100, len=100
    // 100 = 0x64. hi = 0x01 (100>>6=1), lo = 0x24, <<2 = 0x90, len[9:8]=0 -> 0x90.
    // len & 0xFF = 0x64.
    std::vector<BYTE> body = {0xEF, 0x01, 0x90, 0x64,
                              0x6F, // primary header
                              0x00, 0x00, 0x00, 0x00, 0x00};
    PRtpPacket pRed = makeRedRtpPacket(63, 1, 200, 0xABCD, body);
    PRtpPacket out[RED_MAX_BLOCKS] = {};
    UINT32 produced = 99;
    UINT32 fec = 99;
    EXPECT_EQ(STATUS_RTP_INVALID_RED_PACKET, splitRedRtpPacket(pRed, 111, out, RED_MAX_BLOCKS, &produced, &fec));
    EXPECT_EQ(0u, produced);
    EXPECT_EQ(0u, fec);
    freeRtpPacket(&pRed);
}

TEST_F(RtpRedPayloaderTest, splitMalformedTooManyBlocks)
{
    // 33 F=1 headers with tiny lengths — must reject.
    std::vector<BYTE> body;
    for (UINT32 i = 0; i < 33; i++) {
        body.push_back(0xEF);
        body.push_back(0x00);
        body.push_back(0x00); // tsDelta=0, len=0
        body.push_back(0x00);
    }
    body.push_back(0x6F); // primary
    PRtpPacket pRed = makeRedRtpPacket(63, 1, 1000, 0xABCD, body);
    PRtpPacket out[RED_MAX_BLOCKS] = {};
    UINT32 produced = 0;
    EXPECT_EQ(STATUS_RTP_INVALID_RED_PACKET, splitRedRtpPacket(pRed, 111, out, RED_MAX_BLOCKS, &produced, nullptr));
    EXPECT_EQ(0u, produced);
    freeRtpPacket(&pRed);
}

TEST_F(RtpRedPayloaderTest, splitSkipsBlockWithWrongInnerPt)
{
    // One F=1 block with PT=94 (wrong), then primary with PT=111.
    // Layout per RFC 2198: [all headers][all payloads].
    // header1 (F=1|94, tsDelta=100, len=2) → 0xDE, 0x01, 0x90, 0x02
    // primary header (F=0|111)             → 0x6F
    // redundant payload (2 bytes)          → 0xDD, 0xDD
    // primary payload (3 bytes)            → 0x11, 0x22, 0x33
    std::vector<BYTE> body = {0xDE, 0x01, 0x90, 0x02, 0x6F, 0xDD, 0xDD, 0x11, 0x22, 0x33};
    PRtpPacket pRed = makeRedRtpPacket(63, 50, 500, 0xABCD, body);
    PRtpPacket out[RED_MAX_BLOCKS] = {};
    UINT32 produced = 0;
    UINT32 fec = 0;
    EXPECT_EQ(STATUS_SUCCESS, splitRedRtpPacket(pRed, 111, out, RED_MAX_BLOCKS, &produced, &fec));
    // Only the primary comes out; the wrong-PT block is skipped.
    EXPECT_EQ(1u, produced);
    EXPECT_EQ(0u, fec);
    ASSERT_NE(nullptr, out[0]);
    EXPECT_EQ(500u, out[0]->header.timestamp);
    EXPECT_EQ(3u, out[0]->payloadLength);
    EXPECT_EQ(0x11, out[0]->payload[0]);
    freeRtpPacket(&out[0]);
    freeRtpPacket(&pRed);
}

TEST_F(RtpRedPayloaderTest, roundTripHundredFrames)
{
    PRedSenderState pState = NULL;
    createRedSenderState(1, 111, &pState);

    std::vector<std::vector<BYTE>> frames;
    for (UINT32 i = 0; i < 100; i++) {
        std::vector<BYTE> f((i % 17) + 5, (BYTE) (i & 0xFF));
        frames.push_back(f);
    }

    std::vector<std::vector<BYTE>> packed;
    for (UINT32 i = 0; i < 100; i++) {
        std::vector<BYTE> body;
        BOOL fb = FALSE;
        ASSERT_EQ(STATUS_SUCCESS, packOpusRed(1200, frames[i], 1000 + i * 960, pState, body, fb));
        packed.push_back(body);
    }

    // Split each packed body and verify primary[i] == frames[i] and, for i>=1,
    // the redundant block equals frames[i-1].
    for (UINT32 i = 0; i < 100; i++) {
        PRtpPacket pRed = makeRedRtpPacket(63, (UINT16) (1000 + i), 1000 + i * 960, 0xABCD, packed[i]);
        PRtpPacket out[RED_MAX_BLOCKS] = {};
        UINT32 produced = 0;
        UINT32 fec = 0;
        ASSERT_EQ(STATUS_SUCCESS, splitRedRtpPacket(pRed, 111, out, RED_MAX_BLOCKS, &produced, &fec));

        if (i == 0) {
            ASSERT_EQ(1u, produced);
            ASSERT_EQ(frames[0].size(), out[0]->payloadLength);
            EXPECT_EQ(0, MEMCMP(out[0]->payload, frames[0].data(), frames[0].size()));
        } else {
            ASSERT_EQ(2u, produced);
            // Redundant = frames[i-1]
            ASSERT_EQ(frames[i - 1].size(), out[0]->payloadLength);
            EXPECT_EQ(0, MEMCMP(out[0]->payload, frames[i - 1].data(), frames[i - 1].size()));
            // Primary = frames[i]
            ASSERT_EQ(frames[i].size(), out[1]->payloadLength);
            EXPECT_EQ(0, MEMCMP(out[1]->payload, frames[i].data(), frames[i].size()));
        }

        for (UINT32 k = 0; k < produced; k++) {
            freeRtpPacket(&out[k]);
        }
        freeRtpPacket(&pRed);
    }

    freeRedSenderState(&pState);
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
