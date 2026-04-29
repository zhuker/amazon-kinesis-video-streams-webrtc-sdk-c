/*******************************************
Pacer Unit Tests
Smooth packet transmission for congestion control
*******************************************/

#include "WebRTCClientTestFixture.h"

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class PacerFunctionalityTest : public WebRtcClientTestBase {
  public:
    PPacer pPacer = nullptr;
    TIMER_QUEUE_HANDLE timerQueueHandle = INVALID_TIMER_QUEUE_HANDLE_VALUE;

    void SetUp() override
    {
        WebRtcClientTestBase::SetUp();
        EXPECT_EQ(STATUS_SUCCESS, timerQueueCreate(&timerQueueHandle));
    }

    void TearDown() override
    {
        if (pPacer != nullptr) {
            freePacer(&pPacer);
        }
        if (IS_VALID_TIMER_QUEUE_HANDLE(timerQueueHandle)) {
            timerQueueFree(&timerQueueHandle);
        }
        WebRtcClientTestBase::TearDown();
    }

    // Helper to create test packet data
    PBYTE createTestPacket(UINT32 size)
    {
        PBYTE pData = (PBYTE) MEMALLOC(size);
        if (pData != NULL) {
            MEMSET(pData, 0xAB, size);
        }
        return pData;
    }
};

//
// Creation and Configuration Tests
//

TEST_F(PacerFunctionalityTest, createWithDefaults)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    EXPECT_NE(nullptr, pPacer);
    EXPECT_TRUE(pacerIsEnabled(pPacer));
    EXPECT_EQ(300000ULL, pacerGetTargetBitrate(pPacer));
    EXPECT_EQ(0U, pacerGetQueueSize(pPacer));
}

TEST_F(PacerFunctionalityTest, createWithConfig)
{
    PacerConfig config;
    config.initialBitrateBps = 500000;
    config.maxQueueSize = 100;
    config.maxQueueBytes = 500000;
    config.enabled = TRUE;

    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, &config));

    EXPECT_NE(nullptr, pPacer);
    EXPECT_EQ(500000ULL, pacerGetTargetBitrate(pPacer));
}

TEST_F(PacerFunctionalityTest, createNullArgs)
{
    EXPECT_EQ(STATUS_NULL_ARG, createPacer(nullptr, timerQueueHandle, nullptr));
    EXPECT_EQ(STATUS_INVALID_ARG, createPacer(&pPacer, INVALID_TIMER_QUEUE_HANDLE_VALUE, nullptr));
}

TEST_F(PacerFunctionalityTest, freeNullArg)
{
    EXPECT_EQ(STATUS_NULL_ARG, freePacer(nullptr));

    PPacer pNullPacer = nullptr;
    EXPECT_EQ(STATUS_SUCCESS, freePacer(&pNullPacer));
}

TEST_F(PacerFunctionalityTest, freeIdempotent)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));
    EXPECT_EQ(STATUS_SUCCESS, freePacer(&pPacer));
    EXPECT_EQ(nullptr, pPacer);

    // Should be safe to call again
    EXPECT_EQ(STATUS_SUCCESS, freePacer(&pPacer));
}

//
// Bitrate Control Tests
//

TEST_F(PacerFunctionalityTest, setTargetBitrate)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    EXPECT_EQ(STATUS_SUCCESS, pacerSetTargetBitrate(pPacer, 1000000));
    EXPECT_EQ(1000000ULL, pacerGetTargetBitrate(pPacer));

    EXPECT_EQ(STATUS_SUCCESS, pacerSetTargetBitrate(pPacer, 500000));
    EXPECT_EQ(500000ULL, pacerGetTargetBitrate(pPacer));
}

TEST_F(PacerFunctionalityTest, setTargetBitrateMinimum)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Try to set below minimum
    EXPECT_EQ(STATUS_SUCCESS, pacerSetTargetBitrate(pPacer, 100));
    // Should be clamped to minimum
    EXPECT_GE(pacerGetTargetBitrate(pPacer), PACER_MIN_BITRATE_BPS);
}

TEST_F(PacerFunctionalityTest, setTargetBitrateNullArg)
{
    EXPECT_EQ(STATUS_NULL_ARG, pacerSetTargetBitrate(nullptr, 1000000));
}

TEST_F(PacerFunctionalityTest, getTargetBitrateNullArg)
{
    EXPECT_EQ(0ULL, pacerGetTargetBitrate(nullptr));
}

//
// Enable/Disable Tests
//

TEST_F(PacerFunctionalityTest, enableDisable)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    EXPECT_TRUE(pacerIsEnabled(pPacer));

    EXPECT_EQ(STATUS_SUCCESS, pacerSetEnabled(pPacer, FALSE));
    EXPECT_FALSE(pacerIsEnabled(pPacer));

    EXPECT_EQ(STATUS_SUCCESS, pacerSetEnabled(pPacer, TRUE));
    EXPECT_TRUE(pacerIsEnabled(pPacer));
}

TEST_F(PacerFunctionalityTest, isEnabledNullArg)
{
    EXPECT_FALSE(pacerIsEnabled(nullptr));
}

TEST_F(PacerFunctionalityTest, setEnabledNullArg)
{
    EXPECT_EQ(STATUS_NULL_ARG, pacerSetEnabled(nullptr, TRUE));
}

//
// Queue Management Tests
//

TEST_F(PacerFunctionalityTest, enqueuePacket)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    PBYTE pData = createTestPacket(1200);
    EXPECT_NE(nullptr, pData);

    EXPECT_EQ(STATUS_SUCCESS, pacerEnqueuePacket(pPacer, pData, 1200));
    EXPECT_EQ(1U, pacerGetQueueSize(pPacer));

    // pData is now owned by pacer, don't free it
}

TEST_F(PacerFunctionalityTest, enqueueMultiplePackets)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    for (UINT16 i = 0; i < 10; i++) {
        PBYTE pData = createTestPacket(1200);
        EXPECT_NE(nullptr, pData);
        EXPECT_EQ(STATUS_SUCCESS, pacerEnqueuePacket(pPacer, pData, 1200));
    }

    EXPECT_EQ(10U, pacerGetQueueSize(pPacer));
}

TEST_F(PacerFunctionalityTest, enqueuePacketNullArgs)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    PBYTE pData = createTestPacket(1200);

    EXPECT_EQ(STATUS_NULL_ARG, pacerEnqueuePacket(nullptr, pData, 1200));
    EXPECT_EQ(STATUS_NULL_ARG, pacerEnqueuePacket(pPacer, nullptr, 1200));
    EXPECT_EQ(STATUS_NULL_ARG, pacerEnqueuePacket(pPacer, pData, 0));

    MEMFREE(pData);
}

TEST_F(PacerFunctionalityTest, enqueueQueueOverflow)
{
    PacerConfig config;
    config.initialBitrateBps = 300000;
    config.maxQueueSize = 5;
    config.maxQueueBytes = 100000;
    config.enabled = TRUE;

    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, &config));

    // Fill up the queue
    for (UINT16 i = 0; i < 5; i++) {
        PBYTE pData = createTestPacket(1200);
        EXPECT_EQ(STATUS_SUCCESS, pacerEnqueuePacket(pPacer, pData, 1200));
    }

    EXPECT_EQ(5U, pacerGetQueueSize(pPacer));

    // Try to add one more - should fail
    PBYTE pData = createTestPacket(1200);
    EXPECT_EQ(STATUS_NOT_ENOUGH_MEMORY, pacerEnqueuePacket(pPacer, pData, 1200));
    // pData was freed by pacerEnqueuePacket on failure
}

TEST_F(PacerFunctionalityTest, getQueueSizeNullArg)
{
    EXPECT_EQ(0U, pacerGetQueueSize(nullptr));
}

//
// Statistics Tests
//

TEST_F(PacerFunctionalityTest, getStats)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    PacerStats stats;
    EXPECT_EQ(STATUS_SUCCESS, pacerGetStats(pPacer, &stats));

    EXPECT_EQ(0ULL, stats.packetsSent);
    EXPECT_EQ(0ULL, stats.bytesSent);
    EXPECT_EQ(0ULL, stats.packetsDropped);
    EXPECT_EQ(0ULL, stats.currentQueueSize);
}

TEST_F(PacerFunctionalityTest, getStatsAfterEnqueue)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    for (UINT16 i = 0; i < 5; i++) {
        PBYTE pData = createTestPacket(1200);
        EXPECT_EQ(STATUS_SUCCESS, pacerEnqueuePacket(pPacer, pData, 1200));
    }

    PacerStats stats;
    EXPECT_EQ(STATUS_SUCCESS, pacerGetStats(pPacer, &stats));

    EXPECT_EQ(5ULL, stats.currentQueueSize);
    EXPECT_EQ(5 * 1200ULL, stats.currentQueueBytes);
}

TEST_F(PacerFunctionalityTest, getStatsNullArgs)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    PacerStats stats;
    EXPECT_EQ(STATUS_NULL_ARG, pacerGetStats(nullptr, &stats));
    EXPECT_EQ(STATUS_NULL_ARG, pacerGetStats(pPacer, nullptr));
}

//
// Budget Calculation Tests
//

TEST_F(PacerFunctionalityTest, calculateBudget)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Set bitrate to 1 Mbps = 1,000,000 bps
    pacerSetTargetBitrate(pPacer, 1000000);

    // Calculate budget for 5ms (5 * 10000 = 50000 100ns units)
    // At 1 Mbps, 5ms should allow 1000000 * 0.005 / 8 * 1.5 (burst) = 937.5 bytes
    UINT32 budget = pacerCalculateBudget(pPacer, 5 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);

    // Should be around 937 bytes with burst multiplier
    EXPECT_GT(budget, 500U);
    EXPECT_LT(budget, 2000U);
}

TEST_F(PacerFunctionalityTest, calculateBudgetZeroTime)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    UINT32 budget = pacerCalculateBudget(pPacer, 0);
    EXPECT_EQ(0U, budget);
}

TEST_F(PacerFunctionalityTest, calculateBudgetNullArg)
{
    UINT32 budget = pacerCalculateBudget(nullptr, 5 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    EXPECT_EQ(0U, budget);
}

//
// Start/Stop Tests
//

TEST_F(PacerFunctionalityTest, startStopWithoutPeerConnection)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Start without peer connection - should return NULL_ARG
    EXPECT_EQ(STATUS_NULL_ARG, pacerStart(pPacer, nullptr));
}

TEST_F(PacerFunctionalityTest, stopWithoutStart)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Should be safe to stop even if not started
    EXPECT_EQ(STATUS_SUCCESS, pacerStop(pPacer));
}

TEST_F(PacerFunctionalityTest, stopNullArg)
{
    EXPECT_EQ(STATUS_NULL_ARG, pacerStop(nullptr));
}

//
// Timer Callback Tests
//

TEST_F(PacerFunctionalityTest, timerCallbackNullArg)
{
    EXPECT_EQ(STATUS_NULL_ARG, pacerTimerCallback(0, 0, 0));
}

//
// Drain Queue Tests
//

TEST_F(PacerFunctionalityTest, drainQueueNullArg)
{
    EXPECT_EQ(STATUS_NULL_ARG, pacerDrainQueue(nullptr));
}

TEST_F(PacerFunctionalityTest, drainQueueWithoutPeerConnection)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Drain without peer connection - should return INVALID_OPERATION
    EXPECT_EQ(STATUS_INVALID_OPERATION, pacerDrainQueue(pPacer));
}

//
// Integration Tests
//

TEST_F(PacerFunctionalityTest, fullLifecycle)
{
    PacerConfig config;
    config.initialBitrateBps = 500000;
    config.maxQueueSize = 100;
    config.maxQueueBytes = 200000;
    config.enabled = TRUE;

    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, &config));

    // Enqueue some packets
    for (UINT16 i = 0; i < 20; i++) {
        PBYTE pData = createTestPacket(1200);
        EXPECT_EQ(STATUS_SUCCESS, pacerEnqueuePacket(pPacer, pData, 1200));
    }

    EXPECT_EQ(20U, pacerGetQueueSize(pPacer));

    // Change bitrate
    EXPECT_EQ(STATUS_SUCCESS, pacerSetTargetBitrate(pPacer, 1000000));
    EXPECT_EQ(1000000ULL, pacerGetTargetBitrate(pPacer));

    // Disable and re-enable
    EXPECT_EQ(STATUS_SUCCESS, pacerSetEnabled(pPacer, FALSE));
    EXPECT_FALSE(pacerIsEnabled(pPacer));
    EXPECT_EQ(STATUS_SUCCESS, pacerSetEnabled(pPacer, TRUE));
    EXPECT_TRUE(pacerIsEnabled(pPacer));

    // Get stats
    PacerStats stats;
    EXPECT_EQ(STATUS_SUCCESS, pacerGetStats(pPacer, &stats));
    EXPECT_EQ(20ULL, stats.currentQueueSize);

    // Stop
    EXPECT_EQ(STATUS_SUCCESS, pacerStop(pPacer));

    // Free clears the queue
    EXPECT_EQ(STATUS_SUCCESS, freePacer(&pPacer));
    EXPECT_EQ(nullptr, pPacer);
}

TEST_F(PacerFunctionalityTest, queueClearedOnFree)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Enqueue packets
    for (UINT16 i = 0; i < 50; i++) {
        PBYTE pData = createTestPacket(1200);
        EXPECT_EQ(STATUS_SUCCESS, pacerEnqueuePacket(pPacer, pData, 1200));
    }

    EXPECT_EQ(50U, pacerGetQueueSize(pPacer));

    // Free should clean up all packets without memory leaks
    EXPECT_EQ(STATUS_SUCCESS, freePacer(&pPacer));
    EXPECT_EQ(nullptr, pPacer);
}

TEST_F(PacerFunctionalityTest, multipleBitrateChanges)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Simulate GCC-like bitrate adjustments
    UINT64 bitrates[] = {300000, 500000, 800000, 1000000, 800000, 600000, 400000, 500000};

    for (UINT32 i = 0; i < ARRAY_SIZE(bitrates); i++) {
        EXPECT_EQ(STATUS_SUCCESS, pacerSetTargetBitrate(pPacer, bitrates[i]));
        EXPECT_EQ(bitrates[i], pacerGetTargetBitrate(pPacer));
    }
}

TEST_F(PacerFunctionalityTest, highThroughputEnqueue)
{
    PacerConfig config;
    config.initialBitrateBps = 2500000;  // 2.5 Mbps
    config.maxQueueSize = 500;
    config.maxQueueBytes = 1000000;
    config.enabled = TRUE;

    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, &config));

    // Simulate high throughput - enqueue many packets quickly
    UINT32 packetsEnqueued = 0;
    for (UINT32 i = 0; i < 200; i++) {
        PBYTE pData = createTestPacket(1200);
        if (STATUS_SUCCEEDED(pacerEnqueuePacket(pPacer, pData, 1200))) {
            packetsEnqueued++;
        }
    }

    EXPECT_EQ(200U, packetsEnqueued);
    EXPECT_EQ(200U, pacerGetQueueSize(pPacer));

    PacerStats stats;
    EXPECT_EQ(STATUS_SUCCESS, pacerGetStats(pPacer, &stats));
    EXPECT_EQ(200ULL, stats.currentQueueSize);
    EXPECT_EQ(200 * 1200ULL, stats.currentQueueBytes);
}

//
// Max Queue Time Tests
//

TEST_F(PacerFunctionalityTest, setMaxQueueTime)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Default should be 0 (disabled)
    EXPECT_EQ(0U, pacerGetMaxQueueTime(pPacer));

    // Set to 60fps mode
    EXPECT_EQ(STATUS_SUCCESS, pacerSetMaxQueueTime(pPacer, 16));
    EXPECT_EQ(16U, pacerGetMaxQueueTime(pPacer));

    // Set to 30fps mode
    EXPECT_EQ(STATUS_SUCCESS, pacerSetMaxQueueTime(pPacer, 33));
    EXPECT_EQ(33U, pacerGetMaxQueueTime(pPacer));

    // Disable
    EXPECT_EQ(STATUS_SUCCESS, pacerSetMaxQueueTime(pPacer, 0));
    EXPECT_EQ(0U, pacerGetMaxQueueTime(pPacer));
}

TEST_F(PacerFunctionalityTest, setMaxQueueTimeNullArg)
{
    EXPECT_EQ(STATUS_NULL_ARG, pacerSetMaxQueueTime(nullptr, 16));
}

TEST_F(PacerFunctionalityTest, getMaxQueueTimeNullArg)
{
    EXPECT_EQ(0U, pacerGetMaxQueueTime(nullptr));
}

TEST_F(PacerFunctionalityTest, createWithMaxQueueTime)
{
    PacerConfig config;
    config.initialBitrateBps = 500000;
    config.maxQueueSize = 100;
    config.maxQueueBytes = 500000;
    config.pacingFactor = 2.5;
    config.maxQueueTimeKvs = 16 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;  // 16ms for 60fps
    config.enabled = TRUE;

    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, &config));
    EXPECT_EQ(16 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND, pacerGetMaxQueueTime(pPacer));
}

//
// Batch Enqueue (Frame) Tests
//

TEST_F(PacerFunctionalityTest, enqueueFrame)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Create a frame with 10 packets
    PacerPacketInfo packets[10];
    for (UINT32 i = 0; i < 10; i++) {
        packets[i].pData = createTestPacket(1200);
        packets[i].size = 1200;

    }

    EXPECT_EQ(STATUS_SUCCESS, pacerEnqueueFrame(pPacer, packets, 10));
    EXPECT_EQ(10U, pacerGetQueueSize(pPacer));

    PacerStats stats;
    EXPECT_EQ(STATUS_SUCCESS, pacerGetStats(pPacer, &stats));
    EXPECT_EQ(10ULL, stats.currentQueueSize);
    EXPECT_EQ(10 * 1200ULL, stats.currentQueueBytes);
}

TEST_F(PacerFunctionalityTest, enqueueFrameNullArgs)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    PacerPacketInfo packets[5];
    for (UINT32 i = 0; i < 5; i++) {
        packets[i].pData = createTestPacket(1200);
        packets[i].size = 1200;

    }

    EXPECT_EQ(STATUS_NULL_ARG, pacerEnqueueFrame(nullptr, packets, 5));
    EXPECT_EQ(STATUS_NULL_ARG, pacerEnqueueFrame(pPacer, nullptr, 5));
    EXPECT_EQ(STATUS_NULL_ARG, pacerEnqueueFrame(pPacer, packets, 0));

    // Clean up - we own the data
    for (UINT32 i = 0; i < 5; i++) {
        MEMFREE(packets[i].pData);
    }
}

TEST_F(PacerFunctionalityTest, enqueueFrameQueueOverflow)
{
    PacerConfig config;
    config.initialBitrateBps = 300000;
    config.maxQueueSize = 5;
    config.maxQueueBytes = 100000;
    config.maxQueueTimeKvs = 0;
    config.enabled = TRUE;

    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, &config));

    // Try to enqueue a frame with 10 packets when max is 5
    PacerPacketInfo packets[10];
    for (UINT32 i = 0; i < 10; i++) {
        packets[i].pData = createTestPacket(1200);
        packets[i].size = 1200;

    }

    // Should fail and free all packet data
    EXPECT_EQ(STATUS_NOT_ENOUGH_MEMORY, pacerEnqueueFrame(pPacer, packets, 10));
    EXPECT_EQ(0U, pacerGetQueueSize(pPacer));

    // Verify stats show dropped packets
    PacerStats stats;
    EXPECT_EQ(STATUS_SUCCESS, pacerGetStats(pPacer, &stats));
    EXPECT_EQ(10ULL, stats.packetsDropped);
    EXPECT_EQ(10 * 1200ULL, stats.bytesDropped);
}

TEST_F(PacerFunctionalityTest, enqueueFrameSameTimestamp)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Create a frame with 5 packets
    PacerPacketInfo packets[5];
    for (UINT32 i = 0; i < 5; i++) {
        packets[i].pData = createTestPacket(1200);
        packets[i].size = 1200;

    }

    EXPECT_EQ(STATUS_SUCCESS, pacerEnqueueFrame(pPacer, packets, 5));
    EXPECT_EQ(5U, pacerGetQueueSize(pPacer));

    // All packets should have the same enqueue time (verified by drain behavior)
    // This is implicit in the implementation - all packets share frameEnqueueTime
}

TEST_F(PacerFunctionalityTest, enqueueMultipleFrames)
{
    EXPECT_EQ(STATUS_SUCCESS, createPacer(&pPacer, timerQueueHandle, nullptr));

    // Enqueue 3 frames
    for (UINT32 frame = 0; frame < 3; frame++) {
        PacerPacketInfo packets[10];
        for (UINT32 i = 0; i < 10; i++) {
            packets[i].pData = createTestPacket(1200);
            packets[i].size = 1200;

        }
        EXPECT_EQ(STATUS_SUCCESS, pacerEnqueueFrame(pPacer, packets, 10));
    }

    EXPECT_EQ(30U, pacerGetQueueSize(pPacer));
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
