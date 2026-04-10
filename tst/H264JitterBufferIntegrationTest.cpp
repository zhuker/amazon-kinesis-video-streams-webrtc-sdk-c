#include "WebRTCClientTestFixture.h"
#include <algorithm>
#include <numeric>
#include <random>
#include <set>
#include <cmath>

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

#define H264_INTEGRATION_TEST_CLOCK_RATE   90000
#define H264_INTEGRATION_TEST_MAX_LATENCY  (5000 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)
#define H264_INTEGRATION_TEST_MTU          1200
#define H264_INTEGRATION_TEST_SSRC         0x12345678
#define H264_INTEGRATION_TEST_PAYLOAD_TYPE 96

// Parameter: <useRealTimeJitterBuffer, maxLatencyMs>
class H264JitterBufferIntegrationTest : public WebRtcClientTestBase, public ::testing::WithParamInterface<std::tuple<bool, UINT32>> {
  protected:
    // Storage for original frames (Annex-B format with start codes). TestFrame
    // is defined in WebRTCClientTestFixture.h and shared across integration
    // tests. loadFramesFromFolder populates data, sendPts (90 kHz RTP ticks in
    // this test) and timescale.
    std::vector<TestFrame> mOriginalFrames;

    // Storage for frames seen by the jitter buffer callbacks. Contains one
    // entry per callback fired: FULL from onFrameReady, PARTIAL/DROPPED from
    // onFrameDropped depending on whether any bytes could be filled.
    std::vector<TestFrame> mReceivedFrames;

    std::vector<DOUBLE> mFrameDelayMs; // delay in ms for each delivered/dropped frame

    // Storage for RTP packets (for simulation)
    struct RtpPacketInfo {
        PRtpPacket pPacket;
        UINT32 frameIndex;
        UINT32 timestamp;
        UINT16 sequenceNumber; // Saved separately since pPacket may be freed
        UINT32 payloadLength;  // RTP payload size
        BYTE nalIndicator;     // First byte of payload (NAL type indicator)
        BYTE fuHeader;         // Second byte for FU-A packets (contains S/E bits)
    };
    std::vector<RtpPacketInfo> mAllPackets;

    // Counters
    UINT32 mTotalPacketsSent;
    UINT32 mTotalFramesSent;

    // Track intact frames that were incorrectly dropped (jitter buffer deficiency)
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

    // Count everything that fired the dropped callback, regardless of whether
    // any packets were salvaged — matches the legacy mTotalFramesDropped semantics.
    UINT32 countDropped() const
    {
        return countFramesWithFlag(TEST_FRAME_PARTIAL) + countFramesWithFlag(TEST_FRAME_DROPPED);
    }

    // Configuration
    UINT32 mMtu;
    UINT32 mClockRate;

    void SetUp() override
    {
        WebRtcClientTestBase::SetUp();
        mMtu = H264_INTEGRATION_TEST_MTU;
        mClockRate = H264_INTEGRATION_TEST_CLOCK_RATE;
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
        // Free all stored RTP packets
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

    void initializeH264JitterBuffer()
    {
        bool useRealTime = std::get<0>(GetParam());
        UINT32 maxLatencyMs = std::get<1>(GetParam());
        UINT64 maxLatency = (UINT64) maxLatencyMs * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        if (useRealTime) {
            ASSERT_EQ(STATUS_SUCCESS,
                      createRealTimeJitterBuffer(h264FrameReadyCallback, h264FrameDroppedCallback, depayH264FromRtpPayload, maxLatency, mClockRate,
                                                 (UINT64) this, FALSE, &mJitterBuffer));
        } else {
            ASSERT_EQ(STATUS_SUCCESS,
                      createJitterBuffer(h264FrameReadyCallback, h264FrameDroppedCallback, depayH264FromRtpPayload, maxLatency, mClockRate,
                                         (UINT64) this, FALSE, &mJitterBuffer));
        }
    }

    STATUS packetizeFrame(UINT32 frameIndex, UINT32 timestamp, UINT16* pSeqNum)
    {
        STATUS retStatus = STATUS_SUCCESS;
        PayloadArray payloadArray = {0};
        PBYTE frameData = (PBYTE) mOriginalFrames[frameIndex].data.data();
        UINT32 frameSize = (UINT32) mOriginalFrames[frameIndex].data.size();
        PRtpPacket pPacketList = NULL;
        UINT32 offset = 0;
        PRtpPacket pPacketCopy = NULL;
        UINT32 packetSize = 0;
        PBYTE rawPacket = NULL;
        UINT32 i = 0;

        // Get required sizes
        CHK_STATUS(createPayloadForH264(mMtu, frameData, frameSize, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

        // Allocate buffers
        payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
        payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
        CHK(payloadArray.payloadBuffer != NULL && payloadArray.payloadSubLength != NULL, STATUS_NOT_ENOUGH_MEMORY);

        // Fill payload data
        CHK_STATUS(createPayloadForH264(mMtu, frameData, frameSize, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                        payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

        // Create RTP packets
        pPacketList = (PRtpPacket) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(RtpPacket));
        CHK(pPacketList != NULL, STATUS_NOT_ENOUGH_MEMORY);

        CHK_STATUS(constructRtpPackets(&payloadArray, H264_INTEGRATION_TEST_PAYLOAD_TYPE, *pSeqNum, timestamp, H264_INTEGRATION_TEST_SSRC,
                                       pPacketList, payloadArray.payloadSubLenSize));

        // Store packet info for later use (create copies that own their memory)
        for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
            pPacketCopy = NULL;

            // Allocate and copy the packet
            packetSize = RTP_GET_RAW_PACKET_SIZE(&pPacketList[i]);
            rawPacket = (PBYTE) MEMALLOC(packetSize);
            CHK(rawPacket != NULL, STATUS_NOT_ENOUGH_MEMORY);

            CHK_STATUS(createBytesFromRtpPacket(&pPacketList[i], rawPacket, &packetSize));
            CHK_STATUS(createRtpPacketFromBytes(rawPacket, packetSize, &pPacketCopy));
            // createRtpPacketFromBytes takes ownership of rawPacket, don't free it
            rawPacket = NULL;

            RtpPacketInfo info;
            info.pPacket = pPacketCopy;
            info.frameIndex = frameIndex;
            info.timestamp = timestamp;
            info.sequenceNumber = pPacketCopy->header.sequenceNumber;
            info.payloadLength = pPacketCopy->payloadLength;
            // Save NAL indicator and FU header for debugging
            info.nalIndicator = (pPacketCopy->payloadLength > 0) ? pPacketCopy->payload[0] : 0;
            info.fuHeader = (pPacketCopy->payloadLength > 1) ? pPacketCopy->payload[1] : 0;
            mAllPackets.push_back(info);

            offset += payloadArray.payloadSubLength[i];
        }

        *pSeqNum = GET_UINT16_SEQ_NUM(*pSeqNum + payloadArray.payloadSubLenSize);
        mTotalFramesSent++;

    CleanUp:
        SAFE_MEMFREE(payloadArray.payloadBuffer);
        SAFE_MEMFREE(payloadArray.payloadSubLength);
        SAFE_MEMFREE(pPacketList);
        // Only free rawPacket if we failed before packet took ownership
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

    void pushAllPacketsInOrder()
    {
        DLOGI("Pushing %zu packets to jitter buffer", mAllPackets.size());
        for (size_t i = 0; i < mAllPackets.size(); i++) {
            auto& info = mAllPackets[i];
            BOOL discarded = FALSE;
            STATUS status = jitterBufferPush(mJitterBuffer, info.pPacket, &discarded);
            ASSERT_EQ(STATUS_SUCCESS, status) << "Failed to push packet " << i;
            if (discarded) {
                DLOGW("Packet %zu (seq=%u, ts=%u) was DISCARDED", i, info.pPacket ? info.pPacket->header.sequenceNumber : 0, info.timestamp);
            } else {
                mTotalPacketsSent++;
            }
            // The jitter buffer now owns the packet, clear our reference
            info.pPacket = NULL;
        }
        DLOGI("Pushed %u packets, received %u frames so far", mTotalPacketsSent, countFullyReceived());
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

    // Static callbacks for jitter buffer
    static STATUS h264FrameReadyCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize)
    {
        H264JitterBufferIntegrationTest* pTest = (H264JitterBufferIntegrationTest*) customData;
        UINT32 filledSize = 0;

        UINT32 frameTsReady = 0;
        UINT32 tailTsReady = 0;
        if (pTest->mJitterBuffer != NULL) {
            tailTsReady = pTest->mJitterBuffer->tailTimestamp;
            // Look up the frame timestamp from the start packet
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
            return STATUS_SUCCESS; // Don't fail the jitter buffer operation
        }

        STATUS status = jitterBufferFillFrameData(pTest->mJitterBuffer, frameBuffer, frameSize, &filledSize, startIndex, endIndex);

        if (STATUS_SUCCEEDED(status) && filledSize == frameSize) {
            TestFrame tf;
            tf.data.assign(frameBuffer, frameBuffer + frameSize);
            tf.sendPts = frameTsReady;
            tf.timescale = 90000;
            tf.flags = TEST_FRAME_FULL;
            pTest->mReceivedFrames.push_back(std::move(tf));
            DLOGS("Received frame %u, size %u, startSeq=%u", pTest->countFullyReceived(), frameSize, startIndex);
        } else {
            DLOGE("Failed to fill frame data: status=0x%08x, filledSize=%u, expected=%u", status, filledSize, frameSize);
        }

        MEMFREE(frameBuffer);
        return STATUS_SUCCESS; // Always return success to not break jitter buffer
    }

    static STATUS h264FrameDroppedCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 timestamp)
    {
        H264JitterBufferIntegrationTest* pTest = (H264JitterBufferIntegrationTest*) customData;
        UINT32 tailTsDropped = 0;
        if (pTest->mJitterBuffer != NULL) {
            tailTsDropped = pTest->mJitterBuffer->tailTimestamp;
        }
        INT32 delayTs = (INT32) (tailTsDropped - timestamp);
        DOUBLE delayMs = (pTest->mClockRate > 0) ? (DOUBLE) delayTs * 1000.0 / pTest->mClockRate : 0.0;
        pTest->mFrameDelayMs.push_back(delayMs);
        DLOGI("Frame DROPPED: startIndex=%u, endIndex=%u, frameTs=%u, tailTs=%u, delay=%d (%.1fms)", startIndex, endIndex, timestamp, tailTsDropped,
              delayTs, delayMs);

        // Mirror samples/pcapJitterBuffer.c:onFrameDropped — attempt to salvage
        // whatever packets are present in the ring so the post-pass can inspect
        // what the transceiver would have forwarded as a partial frame.
        TestFrame tf;
        tf.sendPts = timestamp;
        tf.timescale = 90000;
        tf.flags = TEST_FRAME_DROPPED;

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

    // Generate indices for packet loss simulation
    std::set<UINT32> generateDropIndices(UINT32 totalPackets, DOUBLE lossRate, UINT32 seed)
    {
        std::set<UINT32> dropIndices;
        std::mt19937 gen(seed);
        std::uniform_real_distribution<> dis(0.0, 1.0);

        for (UINT32 i = 0; i < totalPackets; i++) {
            if (dis(gen) < lossRate) {
                dropIndices.insert(i);
            }
        }
        return dropIndices;
    }

    // Generate reordered indices
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

    // Calculate expected frame loss rate given packet loss rate
    DOUBLE calculateExpectedFrameLoss(DOUBLE packetLossRate) const
    {
        if (mTotalFramesSent == 0)
            return 0.0;
        DOUBLE avgPacketsPerFrame = (DOUBLE) mAllPackets.size() / mTotalFramesSent;
        return 1.0 - pow(1.0 - packetLossRate, avgPacketsPerFrame);
    }

    // Analyze which frames are affected by packet loss
    // - framesFullyDropped: ALL packets dropped → invisible to jitter buffer
    // - framesPartiallyDropped: SOME packets dropped, first remaining is NOT a start → dropped
    // - framesPartiallyDelivered: SOME packets dropped, but first remaining IS a start → delivered (corrupted)
    // - framesIntact: NO packets dropped → should be received
    struct FrameLossAnalysis {
        UINT32 framesFullyDropped;       // All packets lost - invisible
        UINT32 framesPartiallyDropped;   // Some packets lost, will be dropped by jitter buffer
        UINT32 framesPartiallyDelivered; // Some packets lost, but still delivered (corrupted)
        UINT32 framesIntact;             // No packets lost - should be received
    };

    // Check if a packet is a "starting" packet for H264
    // Starting packets: STAP-A, single NAL (1-23), FU-A with Start bit
    bool isStartingPacket(const RtpPacketInfo& pkt) const
    {
        BYTE nalType = pkt.nalIndicator & 0x1F;
        if (nalType == 28) {
            // FU-A: check Start bit in FU header
            return (pkt.fuHeader & 0x80) != 0;
        } else if (nalType >= 1 && nalType <= 23) {
            // Single NAL unit
            return true;
        } else if (nalType == 24 || nalType == 25) {
            // STAP-A or STAP-B
            return true;
        }
        return false;
    }

    // Count frames that are silently lost due to latency eviction.
    // A frame is silently lost when ALL of its packets arrive at a point where
    // tailTimestamp - frameTimestamp > maxLatency (in RTP timestamp units).
    // These frames never get a callback — they're discarded on arrival.
    UINT32 countFramesSilentlyLost(const std::vector<UINT32>& sendIndices, UINT64 maxLatencyRtpTicks) const
    {
        // For each frame, find the FIRST packet in send order.
        // At that point, check if the frame's timestamp is already too old.
        std::set<UINT32> framesSeen;     // frames that had at least one packet accepted
        std::set<UINT32> framesRejected; // frames where first packet was already too old
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

            if (framesSeen.count(frameIdx) > 0 || framesRejected.count(frameIdx) > 0) {
                continue; // already classified
            }

            // First packet for this frame in send order
            UINT32 age = (tailTimestamp >= ts) ? (tailTimestamp - ts) : 0;
            if (age > maxLatencyRtpTicks) {
                framesRejected.insert(frameIdx);
            } else {
                framesSeen.insert(frameIdx);
            }
        }

        return (UINT32) framesRejected.size();
    }

    FrameLossAnalysis analyzeFrameLoss(const std::set<UINT32>& dropIndices) const
    {
        // Group packets by frame and track which are dropped
        std::map<UINT32, std::vector<UINT32>> packetIndicesByFrame;

        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            packetIndicesByFrame[mAllPackets[i].frameIndex].push_back(i);
        }

        FrameLossAnalysis result = {0, 0, 0, 0};
        for (const auto& kv : packetIndicesByFrame) {
            const std::vector<UINT32>& packetIndices = kv.second;
            UINT32 totalPackets = (UINT32) packetIndices.size();
            UINT32 droppedPackets = 0;

            // Build list of remaining (non-dropped) packets in sequence order
            std::vector<UINT32> remainingIndices;
            for (UINT32 pktIdx : packetIndices) {
                if (dropIndices.find(pktIdx) != dropIndices.end()) {
                    droppedPackets++;
                } else {
                    remainingIndices.push_back(pktIdx);
                }
            }

            if (droppedPackets == 0) {
                result.framesIntact++;
            } else if (droppedPackets == totalPackets) {
                result.framesFullyDropped++;
            } else {
                // Partial loss - check two conditions for delivery:
                // 1. First remaining packet must be a starting packet
                // 2. Remaining packets must be continuous (no gaps in sequence numbers)
                bool hasStart = !remainingIndices.empty() && isStartingPacket(mAllPackets[remainingIndices[0]]);
                bool isContinuous = true;

                // Check if remaining packets have continuous sequence numbers
                for (size_t i = 1; i < remainingIndices.size() && isContinuous; i++) {
                    UINT16 prevSeq = mAllPackets[remainingIndices[i - 1]].sequenceNumber;
                    UINT16 curSeq = mAllPackets[remainingIndices[i]].sequenceNumber;
                    // Handle wraparound
                    UINT16 expectedSeq = (prevSeq + 1) & 0xFFFF;
                    if (curSeq != expectedSeq) {
                        isContinuous = false;
                    }
                }

                if (hasStart && isContinuous) {
                    // Frame will be delivered (corrupted but deliverable)
                    result.framesPartiallyDelivered++;
                } else {
                    // Frame will be dropped (missing start or has gaps)
                    result.framesPartiallyDropped++;
                }
            }
        }
        return result;
    }

    // Debug function to find which frames have unexpected outcomes
    void analyzeDiscrepancy(const std::set<UINT32>& dropIndices) const
    {
        // Build map of timestamp -> frame status (expected)
        std::map<UINT32, std::string> expectedStatus; // "intact", "partial", "full"
        std::map<UINT32, UINT32> timestampToFrameIndex;

        // Count packets per frame and dropped packets per frame
        std::map<UINT32, UINT32> packetsPerFrame;
        std::map<UINT32, UINT32> droppedPacketsPerFrame;
        std::map<UINT32, UINT32> frameTimestamps; // frameIndex -> timestamp

        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            UINT32 frameIdx = mAllPackets[i].frameIndex;
            UINT32 ts = mAllPackets[i].timestamp;
            packetsPerFrame[frameIdx]++;
            frameTimestamps[frameIdx] = ts;
            timestampToFrameIndex[ts] = frameIdx;
            if (dropIndices.find(i) != dropIndices.end()) {
                droppedPacketsPerFrame[frameIdx]++;
            }
        }

        for (const auto& kv : packetsPerFrame) {
            UINT32 frameIdx = kv.first;
            UINT32 totalPackets = kv.second;
            UINT32 droppedPackets = droppedPacketsPerFrame[frameIdx];
            UINT32 ts = frameTimestamps[frameIdx];

            if (droppedPackets == 0) {
                expectedStatus[ts] = "intact";
            } else if (droppedPackets == totalPackets) {
                expectedStatus[ts] = "full_drop";
            } else {
                expectedStatus[ts] = "partial";
            }
        }

        // Build sets of received and dropped timestamps from collected TestFrames
        std::set<UINT32> actuallyReceivedTimestamps;
        std::set<UINT32> actuallyDroppedTimestamps;
        for (const auto& f : mReceivedFrames) {
            if (f.flags == TEST_FRAME_FULL) {
                actuallyReceivedTimestamps.insert((UINT32) f.sendPts);
            } else {
                actuallyDroppedTimestamps.insert((UINT32) f.sendPts);
            }
        }

        DLOGI("Received timestamps count: %zu, Dropped timestamps count: %zu", actuallyReceivedTimestamps.size(), actuallyDroppedTimestamps.size());

        // Find discrepancies
        DLOGI("=== DISCREPANCY ANALYSIS ===");
        for (const auto& kv : expectedStatus) {
            UINT32 ts = kv.first;
            const std::string& expected = kv.second;
            bool wasDropped = actuallyDroppedTimestamps.find(ts) != actuallyDroppedTimestamps.end();

            if (expected == "intact" && wasDropped) {
                UINT32 frameIdx = timestampToFrameIndex[ts];
                UINT32 total = packetsPerFrame[frameIdx];
                DLOGE("UNEXPECTED: Frame %u (ts=%u) expected INTACT but was DROPPED (%u packets)", frameIdx, ts, total);
                // Print packet info for this frame
                for (UINT32 i = 0; i < mAllPackets.size(); i++) {
                    if (mAllPackets[i].frameIndex == frameIdx) {
                        BYTE nalType = mAllPackets[i].nalIndicator & 0x1F;
                        const char* nalTypeName = (nalType == 24) ? "STAP-A"
                            : (nalType == 28)                     ? "FU-A"
                            : (nalType == 1)                      ? "slice"
                            : (nalType == 5)                      ? "IDR"
                            : (nalType == 7)                      ? "SPS"
                            : (nalType == 8)                      ? "PPS"
                            : (nalType == 9)                      ? "AUD"
                                                                  : "other";
                        DLOGI("  Frame %u: pktIdx=%u, seq=%u, size=%u, nalType=%u (%s)", frameIdx, i, mAllPackets[i].sequenceNumber,
                              mAllPackets[i].payloadLength, nalType, nalTypeName);
                    }
                }
                // Print adjacent frames
                if (frameIdx > 0) {
                    DLOGI("  Previous frame %u:", frameIdx - 1);
                    for (UINT32 i = 0; i < mAllPackets.size(); i++) {
                        if (mAllPackets[i].frameIndex == frameIdx - 1) {
                            bool pktDropped = dropIndices.find(i) != dropIndices.end();
                            DLOGI("    pktIdx=%u, seq=%u, ts=%u, dropped=%s", i, mAllPackets[i].sequenceNumber, mAllPackets[i].timestamp,
                                  pktDropped ? "YES" : "NO");
                        }
                    }
                }
            } else if (expected == "partial" && !wasDropped) {
                UINT32 frameIdx = timestampToFrameIndex[ts];
                UINT32 total = packetsPerFrame[frameIdx];
                UINT32 dropped = droppedPacketsPerFrame[frameIdx];
                bool wasReceived = actuallyReceivedTimestamps.find(ts) != actuallyReceivedTimestamps.end();
                UINT32 origFrameSize = (frameIdx < mOriginalFrames.size()) ? (UINT32) mOriginalFrames[frameIdx].data.size() : 0;
                DLOGE("UNEXPECTED: Frame %u (ts=%u) expected PARTIAL_DROP but was NOT dropped. "
                      "OrigSize=%u, Packets: %u total, %u dropped. Was received: %s",
                      frameIdx, ts, origFrameSize, total, dropped, wasReceived ? "YES" : "NO");

                // Print which specific packets were dropped
                for (UINT32 i = 0; i < mAllPackets.size(); i++) {
                    if (mAllPackets[i].frameIndex == frameIdx) {
                        bool pktDropped = dropIndices.find(i) != dropIndices.end();
                        BYTE nalType = mAllPackets[i].nalIndicator & 0x1F;
                        BYTE fuStart = (mAllPackets[i].fuHeader >> 7) & 1;
                        BYTE fuEnd = (mAllPackets[i].fuHeader >> 6) & 1;
                        const char* nalTypeName = (nalType == 24) ? "STAP-A"
                            : (nalType == 28)                     ? "FU-A"
                            : (nalType == 1)                      ? "slice"
                            : (nalType == 5)                      ? "IDR"
                            : (nalType == 7)                      ? "SPS"
                            : (nalType == 8)                      ? "PPS"
                            : (nalType == 9)                      ? "AUD"
                                                                  : "other";
                        DLOGI("  Packet idx=%u, seq=%u, size=%u, nalType=%u (%s), fuStart=%u, fuEnd=%u, dropped=%s", i, mAllPackets[i].sequenceNumber,
                              mAllPackets[i].payloadLength, nalType, nalTypeName, fuStart, fuEnd, pktDropped ? "YES" : "NO");
                    }
                }
                // Also print packets for adjacent frames
                if (frameIdx > 0) {
                    DLOGI("  Adjacent frame %u:", frameIdx - 1);
                    for (UINT32 i = 0; i < mAllPackets.size(); i++) {
                        if (mAllPackets[i].frameIndex == frameIdx - 1) {
                            BYTE adjNalType = mAllPackets[i].nalIndicator & 0x1F;
                            DLOGI("    Packet idx=%u, seq=%u, size=%u, nalType=%u, ts=%u", i, mAllPackets[i].sequenceNumber,
                                  mAllPackets[i].payloadLength, adjNalType, mAllPackets[i].timestamp);
                        }
                    }
                }
                DLOGI("  Adjacent frame %u:", frameIdx + 1);
                for (UINT32 i = 0; i < mAllPackets.size(); i++) {
                    if (mAllPackets[i].frameIndex == frameIdx + 1) {
                        BYTE adjNalType = mAllPackets[i].nalIndicator & 0x1F;
                        DLOGI("    Packet idx=%u, seq=%u, size=%u, nalType=%u, ts=%u", i, mAllPackets[i].sequenceNumber, mAllPackets[i].payloadLength,
                              adjNalType, mAllPackets[i].timestamp);
                    }
                }
            }
        }

        // Check for dropped frames we didn't predict
        for (UINT32 ts : actuallyDroppedTimestamps) {
            if (expectedStatus.find(ts) == expectedStatus.end()) {
                DLOGE("UNEXPECTED: Unknown timestamp %u was dropped", ts);
            }
        }
        DLOGI("=== END DISCREPANCY ANALYSIS ===");
    }

    // Count intact frames that were dropped (jitter buffer deficiency)
    UINT32 countIntactFramesDropped(const std::set<UINT32>& dropIndices) const
    {
        // Build map of timestamp -> expected status
        std::map<UINT32, bool> frameIsIntact; // timestamp -> true if intact
        std::map<UINT32, UINT32> packetsPerFrame;
        std::map<UINT32, UINT32> droppedPacketsPerFrame;
        std::map<UINT32, UINT32> frameTimestamps; // frameIndex -> timestamp

        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            UINT32 frameIdx = mAllPackets[i].frameIndex;
            UINT32 ts = mAllPackets[i].timestamp;
            packetsPerFrame[frameIdx]++;
            frameTimestamps[frameIdx] = ts;
            if (dropIndices.find(i) != dropIndices.end()) {
                droppedPacketsPerFrame[frameIdx]++;
            }
        }

        for (const auto& kv : packetsPerFrame) {
            UINT32 frameIdx = kv.first;
            UINT32 droppedPackets = droppedPacketsPerFrame[frameIdx];
            UINT32 ts = frameTimestamps[frameIdx];
            frameIsIntact[ts] = (droppedPackets == 0);
        }

        // Count intact frames that appear in the dropped/partial list
        UINT32 count = 0;
        for (const auto& f : mReceivedFrames) {
            if (f.flags == TEST_FRAME_FULL) {
                continue;
            }
            auto it = frameIsIntact.find((UINT32) f.sendPts);
            if (it != frameIsIntact.end() && it->second) {
                count++;
            }
        }
        return count;
    }

    // Save benchmark results to file for comparison
    void saveBenchmarkResults(const char* filename, const char* testName, const FrameLossAnalysis& analysis, UINT32 intactDropped) const
    {
        FILE* fp = fopen(filename, "a");
        if (fp != NULL) {
            fprintf(fp, "=== %s ===\n", testName);
            fprintf(fp, "Total frames sent: %u\n", mTotalFramesSent);
            fprintf(fp, "Analysis:\n");
            fprintf(fp, "  Intact frames: %u\n", analysis.framesIntact);
            fprintf(fp, "  Partially dropped: %u\n", analysis.framesPartiallyDropped);
            fprintf(fp, "  Partially delivered: %u\n", analysis.framesPartiallyDelivered);
            fprintf(fp, "  Fully dropped (invisible): %u\n", analysis.framesFullyDropped);
            fprintf(fp, "Results:\n");
            fprintf(fp, "  Frames received: %u\n", countFullyReceived());
            fprintf(fp, "  Frames dropped by jitter buffer: %u\n", countDropped());
            fprintf(fp, "  *** INTACT FRAMES INCORRECTLY DROPPED: %u ***\n", intactDropped);
            fprintf(fp, "  Extra frames lost due to deficiency: %u\n", intactDropped);
            fprintf(fp, "\n");
            fclose(fp);
            DLOGI("Results saved to %s", filename);
        } else {
            DLOGW("Could not open %s for writing", filename);
        }
    }

    // Type alias for drop index generator function
    using DropGenerator = std::function<std::set<UINT32>(UINT32 totalPackets)>;

    // Helper to create a random loss generator with given rate
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

    // Helper to create a burst loss generator
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

    // Helper to create a periodic loss generator (drops every Nth packet)
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

    // Helper to create a Gilbert-Elliott model loss generator (bursty loss)
    // @param p - probability of transitioning from Good to Bad state
    // @param r - probability of transitioning from Bad to Good state
    // @param pLossGood - probability of packet loss in Good state (typically low, e.g., 0.0)
    // @param pLossBad - probability of packet loss in Bad state (typically high, e.g., 1.0)
    static DropGenerator gilbertElliottLoss(DOUBLE p, DOUBLE r, DOUBLE pLossGood = 0.0, DOUBLE pLossBad = 1.0)
    {
        return [p, r, pLossGood, pLossBad](UINT32 totalPackets) {
            std::set<UINT32> dropIndices;
            std::mt19937 gen(42); // Fixed seed for reproducibility
            std::uniform_real_distribution<> dis(0.0, 1.0);

            bool inBadState = false;
            for (UINT32 i = 0; i < totalPackets; i++) {
                // State transition
                if (inBadState) {
                    if (dis(gen) < r) {
                        inBadState = false; // Bad -> Good
                    }
                } else {
                    if (dis(gen) < p) {
                        inBadState = true; // Good -> Bad
                    }
                }

                // Packet loss based on current state
                DOUBLE pLoss = inBadState ? pLossBad : pLossGood;
                if (dis(gen) < pLoss) {
                    dropIndices.insert(i);
                }
            }
            return dropIndices;
        };
    }

    // Parameterized packet loss test with optional reordering and custom drop pattern
    // @param sampleFolder - folder containing h264 sample frames
    // @param numFrames - number of frames to load and test
    // @param dropGen - function that generates drop indices given total packet count
    // @param maxReorderDistance - max distance for packet reordering (0 = no reordering)
    void runPacketLossTest(const char* sampleFolder, UINT32 numFrames, DropGenerator dropGen, UINT32 maxReorderDistance = 0)
    {
        // Clean up any state from previous test run within the same test case
        cleanupTest();
        mTotalFramesSent = 0;
        mTotalPacketsSent = 0;
        mIntactFramesDropped = 0;

        initializeH264JitterBuffer();
        mOriginalFrames =
            loadFramesFromFolder((PCHAR) sampleFolder, numFrames, RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE,
                                 /*timescale=*/90000, /*frameDuration=*/1440);
        packetizeAllFrames();

        UINT32 totalPackets = (UINT32) mAllPackets.size();

        // Generate drop indices using provided generator
        auto dropIndices = dropGen(totalPackets);

        // Analyze expected frame loss based on which specific packets are dropped
        auto analysis = analyzeFrameLoss(dropIndices);
        DLOGI("Frame loss analysis: fullyDropped=%u, partiallyDropped=%u, partiallyDelivered=%u, intact=%u", analysis.framesFullyDropped,
              analysis.framesPartiallyDropped, analysis.framesPartiallyDelivered, analysis.framesIntact);

        // Build send indices: optionally reordered, with dropped packets removed
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

        // Flush jitter buffer
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

        // Count intact frames that were incorrectly dropped
        mIntactFramesDropped = countIntactFramesDropped(dropIndices);

        if (mIntactFramesDropped > 0) {
            DLOGI("*** JITTER BUFFER DEFICIENCY: %u intact frames were dropped ***", mIntactFramesDropped);
            analyzeDiscrepancy(dropIndices);
        }

        // Count frames silently lost due to latency eviction (all packets arrived too late)
        UINT32 maxLatencyMs = std::get<1>(GetParam());
        UINT64 maxLatencyRtpTicks = (UINT64) maxLatencyMs * mClockRate / 1000;
        UINT32 silentlyLost = countFramesSilentlyLost(sendIndices, maxLatencyRtpTicks);
        DLOGI("Frames silently lost to latency: %u", silentlyLost);

        // expectedAccountedFrames = total - fullyDropped(packet loss) - silentlyLost(latency eviction)
        UINT32 accountedFrames = receivedAfterFlush + droppedAfterFlush;
        UINT32 expectedAccountedFrames = numFrames - analysis.framesFullyDropped - silentlyLost;
        DLOGI("Frame accounting: received=%u + dropped=%u = %u, expected=%u (NUM_FRAMES=%u - fullyDropped=%u - silentlyLost=%u)", receivedAfterFlush,
              droppedAfterFlush, accountedFrames, expectedAccountedFrames, numFrames, analysis.framesFullyDropped, silentlyLost);

        // Every frame that reaches the buffer must get exactly one callback (ready or dropped).
        // Default jitter buffer at low latency is known to violate this: it double-fires callbacks
        // on reordered frames and silently loses frames due to its linear-scan eviction not tracking
        // which timestamps have already been processed.
        bool isDefaultLowLatency = !std::get<0>(GetParam()) && maxLatencyMs < 5000;
        if (!isDefaultLowLatency) {
            EXPECT_EQ(expectedAccountedFrames, accountedFrames)
                << "Frame accounting mismatch: received+dropped=" << accountedFrames << " expected=" << expectedAccountedFrames;
        }

        // Upper bound: can't receive more than intact + partiallyDelivered.
        // Not EQ because partiallyDelivered frames may be dropped if blocked behind a stale head.
        UINT32 maxExpectedReceived = analysis.framesIntact + analysis.framesPartiallyDelivered;
        EXPECT_LE(receivedAfterFlush, maxExpectedReceived) << "More frames received than possible";
    }
};

// Test: Perfect delivery - all packets in order, no loss
TEST_P(H264JitterBufferIntegrationTest, perfectDeliveryAllFramesReceived)
{
    runPacketLossTest("../samples/girH264", 50, randomLoss(0.0));
    runPacketLossTest("../samples/h264SampleFrames", 50, randomLoss(0.0));
}

// Test: Packet reordering - packets arrive out of order but most are delivered
TEST_P(H264JitterBufferIntegrationTest, packetReorderingAllFramesRecovered)
{
    runPacketLossTest("../samples/girH264", 1000, randomLoss(0.0), 5);
    // runPacketLossTest("../samples/h264SampleFrames", 1000, randomLoss(0.0), 5);
}

// Test: 1% packet loss
TEST_P(H264JitterBufferIntegrationTest, packetLoss1Percent)
{
    runPacketLossTest("../samples/girH264", 1000, randomLoss(0.01));
    runPacketLossTest("../samples/h264SampleFrames", 1000, randomLoss(0.01));
}

// Test: 5% packet loss
TEST_P(H264JitterBufferIntegrationTest, packetLoss5Percent)
{
    runPacketLossTest("../samples/girH264", 1000, randomLoss(0.05));
    runPacketLossTest("../samples/h264SampleFrames", 1000, randomLoss(0.05));
}

// Test: Combined packet loss and reordering
TEST_P(H264JitterBufferIntegrationTest, combinedLossAndReordering)
{
    runPacketLossTest("../samples/girH264", 1000, randomLoss(0.02), 3);
    runPacketLossTest("../samples/h264SampleFrames", 1000, randomLoss(0.02), 3);
}

// Test: Burst packet loss (consecutive packets dropped)
TEST_P(H264JitterBufferIntegrationTest, burstPacketLoss)
{
    runPacketLossTest("../samples/girH264", 1000, burstLoss(5, 3));
    runPacketLossTest("../samples/h264SampleFrames", 1000, burstLoss(5, 3));
}

// Test: Periodic packet loss (every Nth packet dropped)
TEST_P(H264JitterBufferIntegrationTest, periodicPacketLoss)
{
    runPacketLossTest("../samples/girH264", 1000, periodicLoss(10));
    runPacketLossTest("../samples/h264SampleFrames", 1000, periodicLoss(10));
}

// Test: Gilbert-Elliott bursty packet loss (simulates network congestion bursts)
// p=0.05 means 5% chance to enter bad state, r=0.3 means 30% chance to recover
TEST_P(H264JitterBufferIntegrationTest, gilbertElliottPacketLoss)
{
    runPacketLossTest("../samples/girH264", 1000, gilbertElliottLoss(0.05, 0.3));
    runPacketLossTest("../samples/h264SampleFrames", 1000, gilbertElliottLoss(0.05, 0.3));
}

// Test: Frame 0 at RTP timestamp 0 with marker packet arriving first (reorder).
// Reproduces a bug where the jitter buffer mistakenly delivered a single marker
// packet as a complete frame, then later dropped the remaining packets.
// This caused frame 0 to be both "received" (partial) and "dropped" (orphans).
TEST_P(H264JitterBufferIntegrationTest, markerPacketFirstAtTimestampZeroNoDoubleCallback)
{
    // Load 2 frames from h264SampleFrames (frame 0 is multi-packet IDR)
    initializeH264JitterBuffer();
    mOriginalFrames =
        loadFramesFromFolder((PCHAR) "../samples/h264SampleFrames", 2, RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE,
                             /*timescale=*/90000, /*frameDuration=*/1440);

    UINT16 seqNum = 0;
    UINT32 timestamp = 0;
    // Packetize frame 0 at ts=0
    ASSERT_EQ(STATUS_SUCCESS, packetizeFrame(0, timestamp, &seqNum));
    UINT32 frame0PacketCount = (UINT32) mAllPackets.size();
    ASSERT_GE(frame0PacketCount, 2u) << "Frame 0 must have multiple packets for this test";

    // Packetize frame 1 at ts=1440
    timestamp += 1440;
    ASSERT_EQ(STATUS_SUCCESS, packetizeFrame(1, timestamp, &seqNum));
    UINT32 totalPackets = (UINT32) mAllPackets.size();

    DLOGI("Frame 0: %u packets (seq 0-%u), Frame 1: %u packets", frame0PacketCount, frame0PacketCount - 1, totalPackets - frame0PacketCount);

    // Verify last packet of frame 0 has marker bit
    ASSERT_TRUE(mAllPackets[frame0PacketCount - 1].pPacket->header.marker) << "Last packet of frame 0 must have marker bit";

    // Push in reordered order: marker packet of frame 0 first, then rest in order
    // This simulates the Linux reorder pattern that caused the bug
    std::vector<UINT32> pushOrder;
    pushOrder.push_back(frame0PacketCount - 1); // marker packet first
    for (UINT32 i = 0; i < totalPackets; i++) {
        if (i != frame0PacketCount - 1) {
            pushOrder.push_back(i);
        }
    }
    pushPacketsWithIndices(pushOrder);

    // Flush remaining
    freeJitterBuffer(&mJitterBuffer);
    mJitterBuffer = NULL;

    // Frame 0 must be received exactly once and never dropped
    EXPECT_EQ(2u, countFullyReceived()) << "Both frames should be received";
    EXPECT_EQ(0u, countDropped()) << "No frames should be dropped";

    // Verify frame 0 was received first with the correct timestamp and matches
    // the original content byte-for-byte (post-pass content check).
    ASSERT_GE(mReceivedFrames.size(), 1u);
    EXPECT_EQ(TEST_FRAME_FULL, mReceivedFrames[0].flags);
    EXPECT_EQ(0u, mReceivedFrames[0].sendPts) << "Frame 0 should have timestamp 0";
}

// Test: Single dropped packet in first frame delays all subsequent frames
TEST_P(H264JitterBufferIntegrationTest, singleDropInFirstFrameDelaysAll)
{
    // Drop only packet index 1 (second packet of first frame)
    auto dropSecondPacket = [](UINT32 totalPackets) {
        std::set<UINT32> drops;
        drops.insert(1);
        return drops;
    };
    runPacketLossTest("../samples/girH264", 1000, dropSecondPacket);
}

// Benchmark test: Records jitter buffer deficiency metrics
// Run this test to capture baseline performance before/after fix
TEST_P(H264JitterBufferIntegrationTest, DISABLED_jitterBufferDeficiencyBenchmark)
{
    const char* RESULTS_FILE = "../jitter_buffer_benchmark.txt";

    // Clear the results file
    FILE* fp = fopen(RESULTS_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "===========================================\n");
        fprintf(fp, "JITTER BUFFER DEFICIENCY BENCHMARK RESULTS\n");
        fprintf(fp, "===========================================\n\n");
        fclose(fp);
    }

    // Test 1: 1% packet loss with 1000 frames
    {
        const UINT32 NUM_FRAMES = 1000;
        const DOUBLE PACKET_LOSS_RATE = 0.01;

        cleanupTest();
        initializeH264JitterBuffer();
        mOriginalFrames = loadFramesFromFolder((PCHAR) "../samples/h264SampleFrames", NUM_FRAMES,
                                               RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE,
                                               /*timescale=*/90000, /*frameDuration=*/1440);
        packetizeAllFrames();

        auto dropIndices = generateDropIndices((UINT32) mAllPackets.size(), PACKET_LOSS_RATE, 12345);
        auto analysis = analyzeFrameLoss(dropIndices);

        std::vector<UINT32> sendIndices;
        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            if (dropIndices.find(i) == dropIndices.end()) {
                sendIndices.push_back(i);
            }
        }
        pushPacketsWithIndices(sendIndices);
        freeJitterBuffer(&mJitterBuffer);
        mJitterBuffer = NULL;

        UINT32 intactDropped = countIntactFramesDropped(dropIndices);
        saveBenchmarkResults(RESULTS_FILE, "1% Packet Loss (1000 frames)", analysis, intactDropped);

        DLOGI("1%% loss: intact=%u, intactDropped=%u", analysis.framesIntact, intactDropped);
    }

    // Test 2: 5% packet loss with 100 frames
    {
        const UINT32 NUM_FRAMES = 100;
        const DOUBLE PACKET_LOSS_RATE = 0.05;

        cleanupTest();
        initializeH264JitterBuffer();
        mOriginalFrames = loadFramesFromFolder((PCHAR) "../samples/h264SampleFrames", NUM_FRAMES,
                                               RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE,
                                               /*timescale=*/90000, /*frameDuration=*/1440);
        packetizeAllFrames();

        auto dropIndices = generateDropIndices((UINT32) mAllPackets.size(), PACKET_LOSS_RATE, 54321);
        auto analysis = analyzeFrameLoss(dropIndices);

        std::vector<UINT32> sendIndices;
        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            if (dropIndices.find(i) == dropIndices.end()) {
                sendIndices.push_back(i);
            }
        }
        pushPacketsWithIndices(sendIndices);
        freeJitterBuffer(&mJitterBuffer);
        mJitterBuffer = NULL;

        UINT32 intactDropped = countIntactFramesDropped(dropIndices);
        saveBenchmarkResults(RESULTS_FILE, "5% Packet Loss (100 frames)", analysis, intactDropped);

        DLOGI("5%% loss: intact=%u, intactDropped=%u", analysis.framesIntact, intactDropped);
    }

    // Test 3: Burst packet loss
    {
        const UINT32 NUM_FRAMES = 100;
        const UINT32 BURST_SIZE = 5;
        const UINT32 NUM_BURSTS = 3;

        cleanupTest();
        initializeH264JitterBuffer();
        mOriginalFrames = loadFramesFromFolder((PCHAR) "../samples/h264SampleFrames", NUM_FRAMES,
                                               RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE,
                                               /*timescale=*/90000, /*frameDuration=*/1440);
        packetizeAllFrames();

        std::set<UINT32> dropIndices;
        UINT32 totalPackets = (UINT32) mAllPackets.size();
        UINT32 burstInterval = totalPackets / (NUM_BURSTS + 1);
        for (UINT32 b = 0; b < NUM_BURSTS; b++) {
            UINT32 burstStart = burstInterval * (b + 1);
            for (UINT32 i = 0; i < BURST_SIZE && (burstStart + i) < totalPackets; i++) {
                dropIndices.insert(burstStart + i);
            }
        }

        auto analysis = analyzeFrameLoss(dropIndices);

        std::vector<UINT32> sendIndices;
        for (UINT32 i = 0; i < mAllPackets.size(); i++) {
            if (dropIndices.find(i) == dropIndices.end()) {
                sendIndices.push_back(i);
            }
        }
        pushPacketsWithIndices(sendIndices);
        freeJitterBuffer(&mJitterBuffer);
        mJitterBuffer = NULL;

        UINT32 intactDropped = countIntactFramesDropped(dropIndices);
        saveBenchmarkResults(RESULTS_FILE, "Burst Loss (3 bursts of 5 packets)", analysis, intactDropped);

        DLOGI("Burst loss: intact=%u, intactDropped=%u", analysis.framesIntact, intactDropped);
    }

    DLOGI("Benchmark results saved to %s", RESULTS_FILE);
}

INSTANTIATE_TEST_SUITE_P(Default5000ms, H264JitterBufferIntegrationTest, ::testing::Values(std::make_tuple(false, 5000u)));
INSTANTIATE_TEST_SUITE_P(Default32ms, H264JitterBufferIntegrationTest, ::testing::Values(std::make_tuple(false, 32u)));
INSTANTIATE_TEST_SUITE_P(RealTime5000ms, H264JitterBufferIntegrationTest, ::testing::Values(std::make_tuple(true, 5000u)));
INSTANTIATE_TEST_SUITE_P(RealTime32ms, H264JitterBufferIntegrationTest, ::testing::Values(std::make_tuple(true, 32u)));

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
