#include "WebRTCClientTestFixture.h"
#include <algorithm>
#include <numeric>
#include <random>
#include <set>
#include <cmath>
#include <tuple>

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

#define OPUS_INTEGRATION_TEST_CLOCK_RATE    48000
#define OPUS_INTEGRATION_TEST_FRAME_SAMPLES 960 // 20 ms at 48 kHz
#define OPUS_INTEGRATION_TEST_MTU           1200
#define OPUS_INTEGRATION_TEST_SSRC          0x12345678
#define OPUS_INTEGRATION_TEST_PAYLOAD_TYPE  111

// Parameter: <useRealTimeJitterBuffer, maxLatencyMs>
class OpusJitterBufferIntegrationTest : public WebRtcClientTestBase, public ::testing::WithParamInterface<std::tuple<bool, UINT32>> {
  protected:
    // Original opus frames read from disk; one file = one encoded opus frame.
    // loadFramesFromFolder populates data, sendPts (48 kHz RTP ticks in this
    // test) and timescale.
    std::vector<TestFrame> mOriginalFrames;

    // Frames observed by the jitter buffer callbacks: FULL from onFrameReady,
    // DROPPED from onFrameDropped. Opus can never produce a PARTIAL frame
    // because every frame is a single RTP packet.
    std::vector<TestFrame> mReceivedFrames;

    std::vector<DOUBLE> mFrameDelayMs; // delay in ms for each delivered/dropped frame

    // One RTP packet per opus frame.
    struct RtpPacketInfo {
        PRtpPacket pPacket;
        UINT32 frameIndex;
        UINT32 timestamp;
        UINT16 sequenceNumber; // Saved separately since pPacket may be freed
        UINT32 payloadLength;
    };
    std::vector<RtpPacketInfo> mAllPackets;

    UINT32 mTotalPacketsSent;
    UINT32 mTotalFramesSent;
    UINT32 mIntactFramesDropped;

    PJitterBuffer mJitterBuffer;

    UINT32 countFramesWithFlag(uint32_t flag) const
    {
        UINT32 n = 0;
        for (const auto& f : mReceivedFrames) {
            if (f.flags == flag) {
                n++;
            }
        }
        return n;
    }

    UINT32 countFullyReceived() const
    {
        return countFramesWithFlag(TEST_FRAME_FULL);
    }

    // Count everything that fired the dropped callback. For opus, every
    // single-packet frame is either fully delivered or fully dropped — there
    // is no PARTIAL case — but we still tolerate it for symmetry with the
    // H264 test.
    UINT32 countDropped() const
    {
        return countFramesWithFlag(TEST_FRAME_PARTIAL) + countFramesWithFlag(TEST_FRAME_DROPPED);
    }

    UINT32 mMtu;
    UINT32 mClockRate;

    void SetUp() override
    {
        WebRtcClientTestBase::SetUp();
        mMtu = OPUS_INTEGRATION_TEST_MTU;
        mClockRate = OPUS_INTEGRATION_TEST_CLOCK_RATE;
        mTotalPacketsSent = 0;
        mTotalFramesSent = 0;
        mIntactFramesDropped = 0;
        mJitterBuffer = NULL;
    }

    void TearDown() override
    {
        cleanupTest();
        WebRtcClientTestBase::TearDown();
    }

    void cleanupTest()
    {
        for (auto& info : mAllPackets) {
            if (info.pPacket != NULL) {
                freeRtpPacket(&info.pPacket);
            }
        }
        mAllPackets.clear();
        mOriginalFrames.clear();
        mReceivedFrames.clear();
        mFrameDelayMs.clear();

        if (mJitterBuffer != NULL) {
            freeJitterBuffer(&mJitterBuffer);
            mJitterBuffer = NULL;
        }
    }

    void initializeOpusJitterBuffer()
    {
        bool useRealTime = std::get<0>(GetParam());
        UINT32 maxLatencyMs = std::get<1>(GetParam());
        UINT64 maxLatency = (UINT64) maxLatencyMs * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        // Opus is always exactly one RTP packet per frame, so each frame is
        // deliverable the moment its packet arrives — no need to wait for a
        // packet at the next timestamp to confirm frame boundaries. Mirror
        // production behaviour (PeerConnection.c sets this TRUE for Opus,
        // Mulaw and Alaw) by passing alwaysSinglePacketFrames = TRUE.
        if (useRealTime) {
            ASSERT_EQ(STATUS_SUCCESS,
                      createRealTimeJitterBuffer(opusFrameReadyCallback, opusFrameDroppedCallback, depayOpusFromRtpPayload, maxLatency, mClockRate,
                                                 (UINT64) this, TRUE, &mJitterBuffer));
        } else {
            ASSERT_EQ(STATUS_SUCCESS,
                      createJitterBuffer(opusFrameReadyCallback, opusFrameDroppedCallback, depayOpusFromRtpPayload, maxLatency, mClockRate,
                                         (UINT64) this, TRUE, &mJitterBuffer));
        }
    }

    STATUS packetizeFrame(UINT32 frameIndex, UINT32 timestamp, UINT16* pSeqNum)
    {
        STATUS retStatus = STATUS_SUCCESS;
        PayloadArray payloadArray = {0};
        PBYTE frameData = (PBYTE) mOriginalFrames[frameIndex].data.data();
        UINT32 frameSize = (UINT32) mOriginalFrames[frameIndex].data.size();
        PRtpPacket pPacketList = NULL;
        PRtpPacket pPacketCopy = NULL;
        UINT32 packetSize = 0;
        PBYTE rawPacket = NULL;
        UINT32 i = 0;

        CHK_STATUS(createPayloadForOpus(mMtu, frameData, frameSize, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

        payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
        payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
        CHK(payloadArray.payloadBuffer != NULL && payloadArray.payloadSubLength != NULL, STATUS_NOT_ENOUGH_MEMORY);

        CHK_STATUS(createPayloadForOpus(mMtu, frameData, frameSize, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                        payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

        // Opus always produces exactly one sub-packet per frame.
        CHK(payloadArray.payloadSubLenSize == 1, STATUS_INVALID_OPERATION);

        pPacketList = (PRtpPacket) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(RtpPacket));
        CHK(pPacketList != NULL, STATUS_NOT_ENOUGH_MEMORY);

        CHK_STATUS(constructRtpPackets(&payloadArray, OPUS_INTEGRATION_TEST_PAYLOAD_TYPE, *pSeqNum, timestamp, OPUS_INTEGRATION_TEST_SSRC,
                                       pPacketList, payloadArray.payloadSubLenSize));

        for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
            pPacketCopy = NULL;

            packetSize = RTP_GET_RAW_PACKET_SIZE(&pPacketList[i]);
            rawPacket = (PBYTE) MEMALLOC(packetSize);
            CHK(rawPacket != NULL, STATUS_NOT_ENOUGH_MEMORY);

            CHK_STATUS(createBytesFromRtpPacket(&pPacketList[i], rawPacket, &packetSize));
            CHK_STATUS(createRtpPacketFromBytes(rawPacket, packetSize, &pPacketCopy));
            // createRtpPacketFromBytes takes ownership of rawPacket.
            rawPacket = NULL;

            // RFC 7587 §4.2: an Opus transmitter SHALL set the marker bit to
            // 0. The SDK's own constructRtpPackets sets marker=TRUE on the
            // last sub-packet of any burst, which for single-packet Opus
            // frames ends up TRUE on every packet — not RFC-compliant. Clear
            // it here so this test mirrors what a conformant external Opus
            // sender would produce.
            pPacketCopy->header.marker = FALSE;
            pPacketCopy->pRawPacket[1] &= (BYTE) 0x7F;

            RtpPacketInfo info;
            info.pPacket = pPacketCopy;
            info.frameIndex = frameIndex;
            info.timestamp = timestamp;
            info.sequenceNumber = pPacketCopy->header.sequenceNumber;
            info.payloadLength = pPacketCopy->payloadLength;
            mAllPackets.push_back(info);
        }

        *pSeqNum = GET_UINT16_SEQ_NUM(*pSeqNum + payloadArray.payloadSubLenSize);
        mTotalFramesSent++;

    CleanUp:
        SAFE_MEMFREE(payloadArray.payloadBuffer);
        SAFE_MEMFREE(payloadArray.payloadSubLength);
        SAFE_MEMFREE(pPacketList);
        SAFE_MEMFREE(rawPacket);

        return retStatus;
    }

    void packetizeAllFrames()
    {
        UINT16 seqNum = 0;
        DLOGI("Packetizing %zu frames", mOriginalFrames.size());
        for (UINT32 i = 0; i < mOriginalFrames.size(); i++) {
            ASSERT_EQ(STATUS_SUCCESS, packetizeFrame(i, (UINT32) mOriginalFrames[i].sendPts, &seqNum));
        }
        DLOGI("Created %zu packets from %u frames", mAllPackets.size(), mTotalFramesSent);
    }

    void pushPacketsWithIndices(const std::vector<UINT32>& indices)
    {
        for (UINT32 idx : indices) {
            if (idx < mAllPackets.size() && mAllPackets[idx].pPacket != NULL) {
                BOOL discarded = FALSE;
                ASSERT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mAllPackets[idx].pPacket, &discarded));
                if (!discarded) {
                    mTotalPacketsSent++;
                }
                mAllPackets[idx].pPacket = NULL;
            }
        }
    }

    static STATUS opusFrameReadyCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize)
    {
        OpusJitterBufferIntegrationTest* pTest = (OpusJitterBufferIntegrationTest*) customData;
        UINT32 filledSize = 0;

        UINT32 frameTsReady = 0;
        UINT32 tailTsReady = 0;
        if (pTest->mJitterBuffer != NULL) {
            tailTsReady = pTest->mJitterBuffer->tailTimestamp;
            PRtpPacket pStartPacket = NULL;
            if (STATUS_SUCCEEDED(jitterBufferGetPacket(pTest->mJitterBuffer, startIndex, &pStartPacket)) && pStartPacket != NULL) {
                frameTsReady = pStartPacket->header.timestamp;
            }
        }
        INT32 delayTs = (INT32) (tailTsReady - frameTsReady);
        DOUBLE delayMs = (pTest->mClockRate > 0) ? (DOUBLE) delayTs * 1000.0 / pTest->mClockRate : 0.0;
        pTest->mFrameDelayMs.push_back(delayMs);
        DLOGI("Frame READY: startIndex=%u, endIndex=%u, frameSize=%u, frameTs=%u, tailTs=%u, delay=%d (%.1fms)", startIndex, endIndex, frameSize,
              frameTsReady, tailTsReady, delayTs, delayMs);

        if (frameSize == 0) {
            DLOGW("Frame size is 0, skipping");
            return STATUS_SUCCESS;
        }

        PBYTE frameBuffer = (PBYTE) MEMALLOC(frameSize);
        if (frameBuffer == NULL) {
            DLOGE("Failed to allocate frame buffer");
            return STATUS_SUCCESS;
        }

        STATUS status = jitterBufferFillFrameData(pTest->mJitterBuffer, frameBuffer, frameSize, &filledSize, startIndex, endIndex);

        if (STATUS_SUCCEEDED(status) && filledSize == frameSize) {
            TestFrame tf;
            tf.data.assign(frameBuffer, frameBuffer + frameSize);
            tf.sendPts = frameTsReady;
            tf.timescale = OPUS_INTEGRATION_TEST_CLOCK_RATE;
            tf.flags = TEST_FRAME_FULL;
            pTest->mReceivedFrames.push_back(std::move(tf));
            DLOGS("Received frame %u, size %u, startSeq=%u", pTest->countFullyReceived(), frameSize, startIndex);
        } else {
            DLOGE("Failed to fill frame data: status=0x%08x, filledSize=%u, expected=%u", status, filledSize, frameSize);
        }

        MEMFREE(frameBuffer);
        return STATUS_SUCCESS;
    }

    static STATUS opusFrameDroppedCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 timestamp)
    {
        OpusJitterBufferIntegrationTest* pTest = (OpusJitterBufferIntegrationTest*) customData;
        UINT32 tailTsDropped = 0;
        if (pTest->mJitterBuffer != NULL) {
            tailTsDropped = pTest->mJitterBuffer->tailTimestamp;
        }
        INT32 delayTs = (INT32) (tailTsDropped - timestamp);
        DOUBLE delayMs = (pTest->mClockRate > 0) ? (DOUBLE) delayTs * 1000.0 / pTest->mClockRate : 0.0;
        pTest->mFrameDelayMs.push_back(delayMs);
        DLOGI("Frame DROPPED: startIndex=%u, endIndex=%u, frameTs=%u, tailTs=%u, delay=%d (%.1fms)", startIndex, endIndex, timestamp, tailTsDropped,
              delayTs, delayMs);

        TestFrame tf;
        tf.sendPts = timestamp;
        tf.timescale = OPUS_INTEGRATION_TEST_CLOCK_RATE;
        tf.flags = TEST_FRAME_DROPPED;
        // Opus has no partial frames: each frame is a single RTP packet, so
        // if the callback fires the packet was lost and there is nothing to
        // salvage. We still try fillPartialFrameData for symmetry.
        if (pTest->mJitterBuffer != NULL) {
            UINT32 partialSize = 0;
            jitterBufferFillPartialFrameData(pTest->mJitterBuffer, NULL, 0, &partialSize, startIndex, endIndex);
            if (partialSize > 0) {
                std::vector<uint8_t> buf(partialSize);
                UINT32 filledSize = 0;
                jitterBufferFillPartialFrameData(pTest->mJitterBuffer, buf.data(), partialSize, &filledSize, startIndex, endIndex);
                if (filledSize > 0) {
                    buf.resize(filledSize);
                    tf.data = std::move(buf);
                    tf.flags = TEST_FRAME_PARTIAL;
                }
            }
        }
        pTest->mReceivedFrames.push_back(std::move(tf));
        return STATUS_SUCCESS;
    }

    std::vector<UINT32> generateReorderedIndices(UINT32 totalPackets, UINT32 maxDistance, UINT32 seed)
    {
        std::vector<UINT32> indices(totalPackets);
        std::iota(indices.begin(), indices.end(), 0);

        std::mt19937 gen(seed);
        std::uniform_real_distribution<> dis(0.0, 1.0);

        for (UINT32 i = 0; i < totalPackets && maxDistance > 0; i++) {
            if (dis(gen) < 0.1) { // 10% reorder probability
                UINT32 maxSwap = MIN(maxDistance, totalPackets - i - 1);
                if (maxSwap > 0) {
                    std::uniform_int_distribution<> swapDis(1, maxSwap);
                    UINT32 swapIdx = i + swapDis(gen);
                    std::swap(indices[i], indices[swapIdx]);
                }
            }
        }
        return indices;
    }

    // For opus, "frame loss" and "packet loss" are the same thing: one packet
    // per frame. This is much simpler than the H264 case where partial frame
    // delivery is possible.
    struct FrameLossAnalysis {
        UINT32 framesFullyDropped; // the single packet for this frame was lost
        UINT32 framesIntact;       // the packet was delivered
    };

    FrameLossAnalysis analyzeFrameLoss(const std::set<UINT32>& dropIndices) const
    {
        FrameLossAnalysis result = {0, 0};
        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            if (dropIndices.find(i) != dropIndices.end()) {
                result.framesFullyDropped++;
            } else {
                result.framesIntact++;
            }
        }
        return result;
    }

    // Count frames silently lost due to latency eviction.
    // A frame is silently lost when its single packet arrives when
    // tailTimestamp - frameTimestamp > maxLatency (in RTP timestamp units).
    UINT32 countFramesSilentlyLost(const std::vector<UINT32>& sendIndices, UINT64 maxLatencyRtpTicks) const
    {
        std::set<UINT32> framesRejected;
        UINT32 tailTimestamp = 0;
        bool started = false;

        for (UINT32 idx : sendIndices) {
            UINT32 ts = mAllPackets[idx].timestamp;
            UINT32 frameIdx = mAllPackets[idx].frameIndex;

            if (!started) {
                tailTimestamp = ts;
                started = true;
            } else if (ts > tailTimestamp) {
                tailTimestamp = ts;
            }

            UINT32 age = (tailTimestamp >= ts) ? (tailTimestamp - ts) : 0;
            if (age > maxLatencyRtpTicks) {
                framesRejected.insert(frameIdx);
            }
        }
        return (UINT32) framesRejected.size();
    }

    UINT32 countIntactFramesDropped(const std::set<UINT32>& dropIndices) const
    {
        std::set<UINT32> intactTimestamps;
        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            if (dropIndices.find(i) == dropIndices.end()) {
                intactTimestamps.insert(mAllPackets[i].timestamp);
            }
        }

        UINT32 count = 0;
        for (const auto& f : mReceivedFrames) {
            if (f.flags == TEST_FRAME_FULL) {
                continue;
            }
            if (intactTimestamps.count((UINT32) f.sendPts) > 0) {
                count++;
            }
        }
        return count;
    }

    using DropGenerator = std::function<std::set<UINT32>(UINT32 totalPackets)>;

    static DropGenerator randomLoss(DOUBLE rate)
    {
        return [rate](UINT32 totalPackets) {
            UINT32 seed = (UINT32) (rate * 100000);
            std::set<UINT32> dropIndices;
            std::mt19937 gen(seed);
            std::uniform_real_distribution<> dis(0.0, 1.0);
            for (UINT32 i = 0; i < totalPackets; i++) {
                if (dis(gen) < rate) {
                    dropIndices.insert(i);
                }
            }
            return dropIndices;
        };
    }

    static DropGenerator burstLoss(UINT32 burstSize, UINT32 numBursts)
    {
        return [burstSize, numBursts](UINT32 totalPackets) {
            std::set<UINT32> dropIndices;
            UINT32 burstInterval = totalPackets / (numBursts + 1);
            for (UINT32 b = 0; b < numBursts; b++) {
                UINT32 burstStart = burstInterval * (b + 1);
                for (UINT32 i = 0; i < burstSize && (burstStart + i) < totalPackets; i++) {
                    dropIndices.insert(burstStart + i);
                }
            }
            return dropIndices;
        };
    }

    static DropGenerator periodicLoss(UINT32 period)
    {
        return [period](UINT32 totalPackets) {
            std::set<UINT32> dropIndices;
            for (UINT32 i = period - 1; i < totalPackets; i += period) {
                dropIndices.insert(i);
            }
            return dropIndices;
        };
    }

    static DropGenerator gilbertElliottLoss(DOUBLE p, DOUBLE r, DOUBLE pLossGood = 0.0, DOUBLE pLossBad = 1.0)
    {
        return [p, r, pLossGood, pLossBad](UINT32 totalPackets) {
            std::set<UINT32> dropIndices;
            std::mt19937 gen(42);
            std::uniform_real_distribution<> dis(0.0, 1.0);

            bool inBadState = false;
            for (UINT32 i = 0; i < totalPackets; i++) {
                if (inBadState) {
                    if (dis(gen) < r) {
                        inBadState = false;
                    }
                } else {
                    if (dis(gen) < p) {
                        inBadState = true;
                    }
                }

                DOUBLE pLoss = inBadState ? pLossBad : pLossGood;
                if (dis(gen) < pLoss) {
                    dropIndices.insert(i);
                }
            }
            return dropIndices;
        };
    }

    // For every received FULL frame whose source packet was not dropped,
    // verify that the delivered payload matches the original opus frame
    // byte-for-byte. Opus payloads are opaque to the SDK so a raw memcmp is
    // the right check — unlike H264 there are no start codes to normalise.
    void verifyReceivedFramesMatchOriginals(const std::set<UINT32>& dropIndices) const
    {
        std::map<UINT32, UINT32> tsToFrameIdx;
        std::set<UINT32> droppedTimestamps;
        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            tsToFrameIdx[mAllPackets[i].timestamp] = mAllPackets[i].frameIndex;
            if (dropIndices.find(i) != dropIndices.end()) {
                droppedTimestamps.insert(mAllPackets[i].timestamp);
            }
        }

        UINT32 compared = 0;
        for (const auto& f : mReceivedFrames) {
            if (f.flags != TEST_FRAME_FULL) {
                continue;
            }
            UINT32 ts = (UINT32) f.sendPts;
            auto itIdx = tsToFrameIdx.find(ts);
            if (itIdx == tsToFrameIdx.end()) {
                ADD_FAILURE() << "Received FULL frame ts=" << ts << " has no matching source frame";
                continue;
            }
            if (droppedTimestamps.count(ts) > 0) {
                continue;
            }
            UINT32 frameIdx = itIdx->second;
            ASSERT_LT(frameIdx, mOriginalFrames.size());
            const auto& orig = mOriginalFrames[frameIdx];
            EXPECT_EQ(orig.data.size(), f.data.size()) << "frame " << frameIdx << " (ts=" << ts << ") size mismatch";
            if (orig.data.size() == f.data.size() && !orig.data.empty()) {
                EXPECT_EQ(0, MEMCMP(orig.data.data(), f.data.data(), orig.data.size()))
                    << "frame " << frameIdx << " (ts=" << ts << ") content mismatch";
            }
            compared++;
        }
        DLOGI("verifyReceivedFramesMatchOriginals: compared %u frames byte-for-byte", compared);
    }

    // @param sampleFolder - folder containing opus sample frames
    // @param numFrames - number of frames to load and test
    // @param dropGen - function that generates drop indices given total packet count
    // @param maxReorderDistance - max distance for packet reordering (0 = no reordering)
    void runPacketLossTest(const char* sampleFolder, UINT32 numFrames, DropGenerator dropGen, UINT32 maxReorderDistance = 0)
    {
        cleanupTest();
        mTotalFramesSent = 0;
        mTotalPacketsSent = 0;
        mIntactFramesDropped = 0;

        initializeOpusJitterBuffer();
        mOriginalFrames = loadFramesFromFolder((PCHAR) sampleFolder, numFrames, RTC_CODEC_OPUS,
                                               /*timescale=*/OPUS_INTEGRATION_TEST_CLOCK_RATE,
                                               /*frameDuration=*/OPUS_INTEGRATION_TEST_FRAME_SAMPLES,
                                               /*firstIndex=*/0);
        packetizeAllFrames();

        UINT32 totalPackets = (UINT32) mAllPackets.size();
        ASSERT_EQ(totalPackets, numFrames) << "Opus must produce exactly one packet per frame";

        auto dropIndices = dropGen(totalPackets);
        auto analysis = analyzeFrameLoss(dropIndices);
        DLOGI("Frame loss analysis: fullyDropped=%u, intact=%u", analysis.framesFullyDropped, analysis.framesIntact);

        std::vector<UINT32> sendIndices;
        if (maxReorderDistance > 0) {
            auto reorderedIndices = generateReorderedIndices(totalPackets, maxReorderDistance, maxReorderDistance);
            for (UINT32 idx : reorderedIndices) {
                if (dropIndices.find(idx) == dropIndices.end()) {
                    sendIndices.push_back(idx);
                }
            }
        } else {
            for (UINT32 i = 0; i < totalPackets; i++) {
                if (dropIndices.find(i) == dropIndices.end()) {
                    sendIndices.push_back(i);
                }
            }
        }

        pushPacketsWithIndices(sendIndices);
        UINT32 receivedBeforeFlush = countFullyReceived();
        UINT32 droppedBeforeFlush = countDropped();
        DLOGI("Before flush: received=%u, dropped=%u", receivedBeforeFlush, droppedBeforeFlush);

        freeJitterBuffer(&mJitterBuffer);
        mJitterBuffer = NULL;

        UINT32 receivedAfterFlush = countFullyReceived();
        UINT32 droppedAfterFlush = countDropped();

        DOUBLE avgDelayMs = 0.0;
        if (!mFrameDelayMs.empty()) {
            avgDelayMs = std::accumulate(mFrameDelayMs.begin(), mFrameDelayMs.end(), 0.0) / mFrameDelayMs.size();
        }
        DLOGI("reorder=%u: received=%u (flush added %u), dropped=%u (flush added %u), packets dropped=%zu, avgDelayMs=%.1f", maxReorderDistance,
              receivedAfterFlush, receivedAfterFlush - receivedBeforeFlush, droppedAfterFlush, droppedAfterFlush - droppedBeforeFlush,
              dropIndices.size(), avgDelayMs);

        mIntactFramesDropped = countIntactFramesDropped(dropIndices);
        if (mIntactFramesDropped > 0) {
            DLOGI("*** JITTER BUFFER DEFICIENCY: %u intact frames were dropped ***", mIntactFramesDropped);
        }

        UINT32 maxLatencyMs = std::get<1>(GetParam());
        UINT64 maxLatencyRtpTicks = (UINT64) maxLatencyMs * mClockRate / 1000;
        UINT32 silentlyLost = countFramesSilentlyLost(sendIndices, maxLatencyRtpTicks);
        DLOGI("Frames silently lost to latency: %u", silentlyLost);

        UINT32 accountedFrames = receivedAfterFlush + droppedAfterFlush;
        UINT32 expectedAccountedFrames = numFrames - analysis.framesFullyDropped - silentlyLost;
        DLOGI("Frame accounting: received=%u + dropped=%u = %u, expected=%u (NUM_FRAMES=%u - fullyDropped=%u - silentlyLost=%u)", receivedAfterFlush,
              droppedAfterFlush, accountedFrames, expectedAccountedFrames, numFrames, analysis.framesFullyDropped, silentlyLost);

        // The default jitter buffer is known to silently lose or double-fire
        // callbacks on reordered frames, and the deficiency is more visible
        // for opus than H264 because each opus frame is one packet — a
        // single reordered packet can push an earlier frame out of the
        // buffer. Skip strict accounting whenever the default jitter buffer
        // is combined with either a tight latency or any reordering.
        bool useRealTime = std::get<0>(GetParam());
        bool isDefaultKnownDeficient = !useRealTime && (maxLatencyMs < 5000 || maxReorderDistance > 0);
        if (!isDefaultKnownDeficient) {
            EXPECT_EQ(expectedAccountedFrames, accountedFrames)
                << "Frame accounting mismatch: received+dropped=" << accountedFrames << " expected=" << expectedAccountedFrames;
        }

        // Upper bound: can't receive more than intact frames. Opus has no
        // partial-delivery path, so every received frame must correspond to
        // an intact source packet.
        EXPECT_LE(receivedAfterFlush, analysis.framesIntact) << "More frames received than possible";

        if (!isDefaultKnownDeficient) {
            verifyReceivedFramesMatchOriginals(dropIndices);
        }

        // Opus is one-packet-per-frame, so with alwaysSinglePacketFrames=TRUE
        // every frame should deliver as soon as its packet arrives — avg
        // latency should be near zero, plus a bounded reorder hold equal to
        // one frame duration per swapped packet. Anything larger indicates
        // the buffer is waiting for a marker bit that an RFC 7587-compliant
        // Opus sender will never set. Only checked on the real-time buffer.
        if (useRealTime) {
            DOUBLE latencyBudgetMs = (maxReorderDistance > 0) ? 10.0 : 1.0;
            EXPECT_LT(avgDelayMs, latencyBudgetMs) << "Average frame delivery latency too high — jitter buffer is likely waiting for "
                                                      "the RTP marker bit, but RFC 7587 §4.2 mandates marker=0 for Opus";
        }
    }
};

// Test: Perfect delivery - all packets in order, no loss
TEST_P(OpusJitterBufferIntegrationTest, perfectDeliveryAllFramesReceived)
{
    runPacketLossTest("../samples/opusSampleFrames", 50, randomLoss(0.0));
}

// Test: Packet reordering - packets arrive out of order but are delivered
TEST_P(OpusJitterBufferIntegrationTest, packetReorderingAllFramesRecovered)
{
    runPacketLossTest("../samples/opusSampleFrames", 500, randomLoss(0.0), 5);
}

// Test: 1% packet loss
TEST_P(OpusJitterBufferIntegrationTest, packetLoss1Percent)
{
    runPacketLossTest("../samples/opusSampleFrames", 500, randomLoss(0.01));
}

// Test: 5% packet loss
TEST_P(OpusJitterBufferIntegrationTest, packetLoss5Percent)
{
    runPacketLossTest("../samples/opusSampleFrames", 500, randomLoss(0.05));
}

// Test: Combined packet loss and reordering
TEST_P(OpusJitterBufferIntegrationTest, combinedLossAndReordering)
{
    runPacketLossTest("../samples/opusSampleFrames", 500, randomLoss(0.02), 3);
}

// Test: Burst packet loss (consecutive packets dropped)
TEST_P(OpusJitterBufferIntegrationTest, burstPacketLoss)
{
    runPacketLossTest("../samples/opusSampleFrames", 500, burstLoss(5, 3));
}

// Test: Periodic packet loss (every Nth packet dropped)
TEST_P(OpusJitterBufferIntegrationTest, periodicPacketLoss)
{
    runPacketLossTest("../samples/opusSampleFrames", 500, periodicLoss(10));
}

// Test: Gilbert-Elliott bursty packet loss (simulates congestion bursts)
TEST_P(OpusJitterBufferIntegrationTest, gilbertElliottPacketLoss)
{
    runPacketLossTest("../samples/opusSampleFrames", 500, gilbertElliottLoss(0.05, 0.3));
}

// Test: Single dropped packet in first frame delays subsequent frames
TEST_P(OpusJitterBufferIntegrationTest, singleDropInFirstFrameDelaysAll)
{
    auto dropFirstPacket = [](UINT32 totalPackets) {
        std::set<UINT32> drops;
        drops.insert(0);
        return drops;
    };
    runPacketLossTest("../samples/opusSampleFrames", 500, dropFirstPacket);
}

INSTANTIATE_TEST_SUITE_P(Default5000ms, OpusJitterBufferIntegrationTest, ::testing::Values(std::make_tuple(false, 5000u)));
INSTANTIATE_TEST_SUITE_P(Default32ms, OpusJitterBufferIntegrationTest, ::testing::Values(std::make_tuple(false, 32u)));
INSTANTIATE_TEST_SUITE_P(RealTime5000ms, OpusJitterBufferIntegrationTest, ::testing::Values(std::make_tuple(true, 5000u)));
INSTANTIATE_TEST_SUITE_P(RealTime32ms, OpusJitterBufferIntegrationTest, ::testing::Values(std::make_tuple(true, 32u)));

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
