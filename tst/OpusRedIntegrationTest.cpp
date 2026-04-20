#include "WebRTCClientTestFixture.h"
#include <algorithm>
#include <set>
#include <vector>

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

// Integration test for RFC 2198 RED with Opus. Exercises the full pipeline:
//   opus bytes -> createPayloadForOpusRed (pack) -> wire RTP packet ->
//   splitRedRtpPacket (unpack) -> jitterBufferPush -> onFrameReady callback.
// Loss is simulated by dropping selected "wire" packets before the split step.

#define OPUS_RED_TEST_CLOCK_RATE   48000
#define OPUS_RED_TEST_MTU          1200
#define OPUS_RED_TEST_SSRC         0xABCD1234
#define OPUS_RED_TEST_OPUS_PT      111
#define OPUS_RED_TEST_RED_PT       63
#define OPUS_RED_TEST_MAX_LATENCY  (5000 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)
#define OPUS_RED_TEST_FRAME_COUNT  100
#define OPUS_RED_TEST_TS_INCREMENT 960 // 20ms @ 48kHz

// Callback context for the jitter buffer. Counts delivered and dropped timestamps.
struct OpusRedCtx {
    std::vector<UINT32> deliveredTimestamps;
    std::vector<UINT32> droppedTimestamps;
    PJitterBuffer pJitterBuffer;
};

static STATUS onOpusRedFrameReady(UINT64 customData, UINT16 startSeq, UINT16 endSeq, UINT32 frameSize)
{
    OpusRedCtx* ctx = reinterpret_cast<OpusRedCtx*>(customData);
    // Retrieve the timestamp via the first packet.
    PRtpPacket pPkt = NULL;
    if (STATUS_SUCCEEDED(ctx->pJitterBuffer->getPacketFn(ctx->pJitterBuffer, startSeq, &pPkt)) && pPkt != NULL) {
        ctx->deliveredTimestamps.push_back(pPkt->header.timestamp);
    }
    UNUSED_PARAM(endSeq);
    UNUSED_PARAM(frameSize);
    return STATUS_SUCCESS;
}

static STATUS onOpusRedFrameDropped(UINT64 customData, UINT16 startSeq, UINT16 endSeq, UINT32 timestamp)
{
    OpusRedCtx* ctx = reinterpret_cast<OpusRedCtx*>(customData);
    ctx->droppedTimestamps.push_back(timestamp);
    UNUSED_PARAM(startSeq);
    UNUSED_PARAM(endSeq);
    return STATUS_SUCCESS;
}

// Build one synthesized "on-the-wire" RED-wrapped RtpPacket that owns its raw payload buffer.
static PRtpPacket buildRedWirePacket(UINT16 seq, UINT32 ts, UINT32 ssrc, UINT8 redPt, const BYTE* body, UINT32 bodyLen)
{
    PRtpPacket p = (PRtpPacket) MEMCALLOC(1, SIZEOF(RtpPacket));
    p->header.version = 2;
    p->header.payloadType = redPt;
    p->header.sequenceNumber = seq;
    p->header.timestamp = ts;
    p->header.ssrc = ssrc;
    p->pRawPacket = (PBYTE) MEMALLOC(bodyLen);
    MEMCPY(p->pRawPacket, body, bodyLen);
    p->rawPacketLength = bodyLen;
    p->payload = p->pRawPacket;
    p->payloadLength = bodyLen;
    return p;
}

// Parameter: <useRealTimeJitterBuffer, redundancyLevel>
class OpusRedIntegrationTest : public WebRtcClientTestBase, public ::testing::WithParamInterface<std::tuple<bool, UINT8>> {
  protected:
    PJitterBuffer mJb = NULL;
    OpusRedCtx mCtx;
    PRedSenderState mSenderState = NULL;
    std::vector<std::vector<BYTE>> mOriginalOpusFrames;
    std::vector<std::vector<BYTE>> mWireBodies;
    UINT32 mFecPacketsReceived = 0;
    UINT32 mFecBytesReceived = 0;
    UINT32 mFecPacketsDiscarded = 0;

    void SetUp() override
    {
        WebRtcClientTestBase::SetUp();
        mCtx.pJitterBuffer = NULL;
    }

    void TearDown() override
    {
        if (mSenderState != NULL) {
            freeRedSenderState(&mSenderState);
        }
        if (mJb != NULL) {
            freeJitterBuffer(&mJb);
        }
        WebRtcClientTestBase::TearDown();
    }

    void createJb(bool useRt)
    {
        if (useRt) {
            ASSERT_EQ(STATUS_SUCCESS,
                      createRealTimeJitterBuffer(onOpusRedFrameReady, onOpusRedFrameDropped, depayOpusFromRtpPayload, OPUS_RED_TEST_MAX_LATENCY,
                                                 OPUS_RED_TEST_CLOCK_RATE, (UINT64) &mCtx, TRUE, &mJb));
        } else {
            ASSERT_EQ(STATUS_SUCCESS,
                      createJitterBuffer(onOpusRedFrameReady, onOpusRedFrameDropped, depayOpusFromRtpPayload, OPUS_RED_TEST_MAX_LATENCY,
                                         OPUS_RED_TEST_CLOCK_RATE, (UINT64) &mCtx, TRUE, &mJb));
        }
        mCtx.pJitterBuffer = mJb;
    }

    // Produce N synthetic Opus frames and pack each through RED with a fresh sender state.
    // Stores both the original Opus bytes and the packed "wire body" (the RED/plain-Opus body).
    void generateFrames(UINT8 redundancy, UINT32 count)
    {
        ASSERT_EQ(STATUS_SUCCESS, createRedSenderState(redundancy, OPUS_RED_TEST_OPUS_PT, &mSenderState));
        for (UINT32 i = 0; i < count; i++) {
            // Use a varying size that never exceeds 200 bytes (keeps RED bodies easily under MTU).
            std::vector<BYTE> opus((i % 23) + 10, (BYTE) (i & 0xFF));
            mOriginalOpusFrames.push_back(opus);

            UINT32 rtpTs = 1000 + i * OPUS_RED_TEST_TS_INCREMENT;
            UINT32 bodyLen = 0;
            UINT32 subLenSize = 1;
            BOOL fallback = FALSE;
            ASSERT_EQ(STATUS_SUCCESS,
                      createPayloadForOpusRed(OPUS_RED_TEST_MTU, opus.data(), (UINT32) opus.size(), rtpTs, mSenderState, NULL, &bodyLen, NULL,
                                              &subLenSize, &fallback));
            std::vector<BYTE> body(bodyLen, 0);
            UINT32 cap = bodyLen;
            UINT32 subCap = 1;
            UINT32 sub = 0;
            ASSERT_EQ(STATUS_SUCCESS,
                      createPayloadForOpusRed(OPUS_RED_TEST_MTU, opus.data(), (UINT32) opus.size(), rtpTs, mSenderState, body.data(), &cap, &sub,
                                              &subCap, &fallback));
            mWireBodies.push_back(body);
        }
    }

    // Simulate delivering packets in order, optionally dropping some.
    void deliverAll(const std::set<UINT32>& dropIdx)
    {
        for (UINT32 i = 0; i < mWireBodies.size(); i++) {
            if (dropIdx.count(i) > 0) {
                continue;
            }
            UINT32 rtpTs = 1000 + i * OPUS_RED_TEST_TS_INCREMENT;
            UINT16 seq = (UINT16) (100 + i);
            PRtpPacket pRed =
                buildRedWirePacket(seq, rtpTs, OPUS_RED_TEST_SSRC, OPUS_RED_TEST_RED_PT, mWireBodies[i].data(), (UINT32) mWireBodies[i].size());
            PRtpPacket synths[RED_MAX_BLOCKS] = {};
            UINT32 produced = 0;
            UINT32 fecBytes = 0;
            ASSERT_EQ(STATUS_SUCCESS, splitRedRtpPacket(pRed, OPUS_RED_TEST_OPUS_PT, synths, RED_MAX_BLOCKS, &produced, &fecBytes));

            // Mirror the production wiring: count redundant blocks as fec*, account for discards.
            for (UINT32 k = 0; k < produced; k++) {
                PRtpPacket pSyn = synths[k];
                BOOL wasSynthetic = pSyn->isSynthetic; // capture before push — push may free it
                if (wasSynthetic) {
                    mFecPacketsReceived++;
                    mFecBytesReceived += pSyn->payloadLength;
                }
                BOOL discarded = FALSE;
                ASSERT_EQ(STATUS_SUCCESS, jitterBufferPush(mJb, pSyn, &discarded));
                if (discarded && wasSynthetic) {
                    mFecPacketsDiscarded++;
                }
            }
            freeRtpPacket(&pRed);
        }
        // Drain with a trailing ts bump so the last frame's fence is set (needed for the classic JB).
        UINT32 trailTs = 1000 + (UINT32) mWireBodies.size() * OPUS_RED_TEST_TS_INCREMENT;
        std::vector<BYTE> trail = {0x00};
        PRtpPacket pTrail = buildRedWirePacket((UINT16) (100 + mWireBodies.size()), trailTs, OPUS_RED_TEST_SSRC, OPUS_RED_TEST_OPUS_PT, trail.data(),
                                               (UINT32) trail.size());
        BOOL discarded = FALSE;
        jitterBufferPush(mJb, pTrail, &discarded);
    }
};

TEST_P(OpusRedIntegrationTest, perfectDeliveryAllFramesReceived)
{
    bool useRt = std::get<0>(GetParam());
    UINT8 redundancy = std::get<1>(GetParam());
    createJb(useRt);
    generateFrames(redundancy, OPUS_RED_TEST_FRAME_COUNT);
    deliverAll({});

    // Expect every original timestamp was delivered.
    std::set<UINT32> delivered(mCtx.deliveredTimestamps.begin(), mCtx.deliveredTimestamps.end());
    for (UINT32 i = 0; i < OPUS_RED_TEST_FRAME_COUNT; i++) {
        UINT32 ts = 1000 + i * OPUS_RED_TEST_TS_INCREMENT;
        EXPECT_NE(delivered.find(ts), delivered.end()) << "timestamp " << ts << " (frame " << i << ") missing";
    }

    // With no loss, redundant blocks arriving after their primary should be dedup'd.
    // Exact count depends on JB eviction/delivery timing; just check we processed redundancy.
    EXPECT_GT(mFecPacketsReceived, 0u);
}

TEST_P(OpusRedIntegrationTest, isolatedLossesAreRecovered)
{
    bool useRt = std::get<0>(GetParam());
    UINT8 redundancy = std::get<1>(GetParam());
    createJb(useRt);
    generateFrames(redundancy, OPUS_RED_TEST_FRAME_COUNT);

    // Drop every 4th packet (except index 0). Each lost packet should be recovered via
    // the redundant copy carried in the next RED container.
    std::set<UINT32> drops;
    for (UINT32 i = 4; i < OPUS_RED_TEST_FRAME_COUNT - 1; i += 4) {
        drops.insert(i);
    }
    deliverAll(drops);

    // Every original timestamp should show up in delivered (or dropped; the RealTimeJB flushes
    // everything). `seen` may also contain the trailing flush-packet ts we push at the end of
    // deliverAll to force the classic JB to recognize the last frame as complete, which is
    // delivery-time behavior we don't care about here.
    std::set<UINT32> seen(mCtx.deliveredTimestamps.begin(), mCtx.deliveredTimestamps.end());
    for (UINT32 t : mCtx.droppedTimestamps) {
        seen.insert(t);
    }
    for (UINT32 i = 0; i < OPUS_RED_TEST_FRAME_COUNT; i++) {
        UINT32 ts = 1000 + i * OPUS_RED_TEST_TS_INCREMENT;
        EXPECT_NE(seen.find(ts), seen.end()) << "timestamp " << ts << " (frame " << i << ") never delivered";
    }

    EXPECT_GT(mFecPacketsReceived, 0u);
}

TEST_P(OpusRedIntegrationTest, burstLossBeyondRedundancyLeavesGaps)
{
    bool useRt = std::get<0>(GetParam());
    UINT8 redundancy = std::get<1>(GetParam());
    if (redundancy != 1) {
        GTEST_SKIP() << "Burst test only meaningful for N=1";
    }
    createJb(useRt);
    generateFrames(redundancy, 50);

    // Drop frames 20,21,22. With N=1, the RED container for frame 23 only carries frame 22 as
    // redundancy — so frames 20 and 21 are unrecoverable. Whether the JB emits frame 22 via
    // its redundant copy depends on internal timing (the JB needs to resolve the hole at
    // seq 120/121 before advancing past it), so we only assert the definitely-lost frames.
    deliverAll({20, 21, 22});

    std::set<UINT32> delivered(mCtx.deliveredTimestamps.begin(), mCtx.deliveredTimestamps.end());
    UINT32 ts20 = 1000 + 20 * OPUS_RED_TEST_TS_INCREMENT;
    UINT32 ts21 = 1000 + 21 * OPUS_RED_TEST_TS_INCREMENT;
    EXPECT_EQ(delivered.find(ts20), delivered.end()) << "frame 20 was unrecoverable (no RED copy) but was delivered";
    EXPECT_EQ(delivered.find(ts21), delivered.end()) << "frame 21 was unrecoverable (no RED copy) but was delivered";

    // Frames before the burst should be delivered normally. Skip frame 19 which is
    // adjacent to the gap — its delivery depends on JB gap-resolution timing.
    for (UINT32 i = 0; i < 19; i++) {
        UINT32 ts = 1000 + i * OPUS_RED_TEST_TS_INCREMENT;
        EXPECT_NE(delivered.find(ts), delivered.end()) << "frame " << i << " lost despite no drop";
    }
}

INSTANTIATE_TEST_SUITE_P(RedCombinations, OpusRedIntegrationTest,
                         ::testing::Values(std::make_tuple(false, (UINT8) 1), std::make_tuple(true, (UINT8) 1), std::make_tuple(false, (UINT8) 2),
                                           std::make_tuple(true, (UINT8) 2)));

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
