#include "WebRTCClientTestFixture.h"

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class JitterBufferFunctionalityTest : public WebRtcClientTestBase, public ::testing::WithParamInterface<bool> {
  protected:
    PUINT32 mExpectedFrameSizeArr = nullptr;
    PBYTE* mPExpectedFrameArr = nullptr;
    UINT32 mExpectedFrameCount = 0;
    PUINT32 mExpectedDroppedFrameTimestampArr = nullptr;
    UINT32 mExpectedDroppedFrameCount = 0;
    PRtpPacket* mPRtpPackets = nullptr;
    UINT32 mRtpPacketCount = 0;
    PJitterBuffer mJitterBuffer = nullptr;
    PBYTE mFrame = nullptr;
    UINT32 mReadyFrameIndex = 0;
    UINT32 mDroppedFrameIndex = 0;

    static STATUS testFrameReadyFunc(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize)
    {
        JitterBufferFunctionalityTest* base = (JitterBufferFunctionalityTest*) customData;
        UINT32 filledSize;
        EXPECT_GT(base->mExpectedFrameCount, base->mReadyFrameIndex);
        EXPECT_EQ(base->mExpectedFrameSizeArr[base->mReadyFrameIndex], frameSize);
        if (base->mFrame != NULL) {
            MEMFREE(base->mFrame);
            base->mFrame = NULL;
        }
        base->mFrame = (PBYTE) MEMALLOC(frameSize);
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferFillFrameData(base->mJitterBuffer, base->mFrame, frameSize, &filledSize, startIndex, endIndex));
        EXPECT_EQ(frameSize, filledSize);
        EXPECT_EQ(0, MEMCMP(base->mPExpectedFrameArr[base->mReadyFrameIndex], base->mFrame, frameSize));
        base->mReadyFrameIndex++;
        return STATUS_SUCCESS;
    }

    static STATUS testFrameDroppedFunc(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 timestamp)
    {
        UNUSED_PARAM(startIndex);
        UNUSED_PARAM(endIndex);
        auto* base = (JitterBufferFunctionalityTest*) customData;
        EXPECT_GT(base->mExpectedDroppedFrameCount, base->mDroppedFrameIndex);
        EXPECT_EQ(base->mExpectedDroppedFrameTimestampArr[base->mDroppedFrameIndex], timestamp);
        base->mDroppedFrameIndex++;
        return STATUS_SUCCESS;
    }

    // Minimal fake depayloader: passes bytes through untouched and reports
    // isStart based on a sentinel byte written one past the payload
    // (tests allocate payloadLength + 1 and set that extra byte).
    static STATUS testDepayRtpFunc(PBYTE payload, UINT32 payloadLength, PBYTE outBuffer, PUINT32 pBufferSize, PBOOL pIsStart)
    {
        ENTERS();
        STATUS retStatus = STATUS_SUCCESS;
        UINT32 bufferSize = 0;
        BOOL sizeCalculationOnly = (outBuffer == NULL);

        CHK(payload != NULL && pBufferSize != NULL, STATUS_NULL_ARG);
        CHK(payloadLength > 0, retStatus);

        bufferSize = payloadLength;

        CHK(!sizeCalculationOnly, retStatus);
        CHK(payloadLength <= *pBufferSize, STATUS_BUFFER_TOO_SMALL);

        MEMCPY(outBuffer, payload, payloadLength);

    CleanUp:
        if (STATUS_FAILED(retStatus) && sizeCalculationOnly) {
            bufferSize = 0;
        }

        if (pBufferSize != NULL) {
            *pBufferSize = bufferSize;
        }

        if (pIsStart != NULL) {
            *pIsStart = (payload[payloadLength] != 0);
        }

        LEAVES();
        return retStatus;
    }

    VOID initializeJitterBuffer(UINT32 expectedFrameCount, UINT32 expectedDroppedFrameCount, UINT32 rtpPacketCount)
    {
        UINT32 i, timestamp;
        BOOL useRealTime = GetParam() ? TRUE : FALSE;
        if (useRealTime) {
            EXPECT_EQ(STATUS_SUCCESS,
                      createRealTimeJitterBuffer(testFrameReadyFunc, testFrameDroppedFunc, testDepayRtpFunc, DEFAULT_JITTER_BUFFER_MAX_LATENCY,
                                                 TEST_JITTER_BUFFER_CLOCK_RATE, (UINT64) this, FALSE, &mJitterBuffer));
        } else {
            EXPECT_EQ(STATUS_SUCCESS,
                      createJitterBuffer(testFrameReadyFunc, testFrameDroppedFunc, testDepayRtpFunc, DEFAULT_JITTER_BUFFER_MAX_LATENCY,
                                         TEST_JITTER_BUFFER_CLOCK_RATE, (UINT64) this, FALSE, &mJitterBuffer));
        }
        mExpectedFrameCount = expectedFrameCount;
        mFrame = NULL;
        if (expectedFrameCount > 0) {
            mPExpectedFrameArr = (PBYTE*) MEMALLOC(SIZEOF(PBYTE) * expectedFrameCount);
            mExpectedFrameSizeArr = (PUINT32) MEMALLOC(SIZEOF(UINT32) * expectedFrameCount);
        }
        mExpectedDroppedFrameCount = expectedDroppedFrameCount;
        if (expectedDroppedFrameCount > 0) {
            mExpectedDroppedFrameTimestampArr = (PUINT32) MEMALLOC(SIZEOF(UINT32) * expectedDroppedFrameCount);
        }

        mPRtpPackets = (PRtpPacket*) MEMALLOC(SIZEOF(PRtpPacket) * rtpPacketCount);
        mRtpPacketCount = rtpPacketCount;

        for (i = 0, timestamp = 0; i < rtpPacketCount; i++, timestamp += 200) {
            EXPECT_EQ(STATUS_SUCCESS,
                      createRtpPacket(2, FALSE, FALSE, 0, FALSE, 96, i, timestamp, 0x1234ABCD, NULL, 0, 0, NULL, NULL, 0, mPRtpPackets + i));
        }
    }

    // Populate packet i with a byte payload, a fake-depayloader isStart sentinel,
    // a timestamp, sequence number and optional marker bit. Allocates payloadLength+1
    // so the depayloader sentinel lives at payload[payloadLength].
    VOID setPacket(UINT32 i, std::initializer_list<BYTE> bytes, UINT32 timestamp, UINT32 seqNum, BOOL isStart, BOOL marker = FALSE)
    {
        UINT32 len = (UINT32) bytes.size();
        mPRtpPackets[i]->payloadLength = len;
        mPRtpPackets[i]->payload = (PBYTE) MEMALLOC(len + 1);
        UINT32 j = 0;
        for (BYTE b : bytes) {
            mPRtpPackets[i]->payload[j++] = b;
        }
        mPRtpPackets[i]->payload[len] = isStart ? 1 : 0;
        mPRtpPackets[i]->header.timestamp = timestamp;
        mPRtpPackets[i]->header.sequenceNumber = (UINT16) seqNum;
        mPRtpPackets[i]->header.marker = marker;
    }

    // Overload that leaves the sequence number at whatever initializeJitterBuffer assigned
    // (packet index i). Matches tests that never touched sequenceNumber.
    VOID setPacket(UINT32 i, std::initializer_list<BYTE> bytes, UINT32 timestamp, BOOL isStart)
    {
        setPacket(i, bytes, timestamp, i, isStart, FALSE);
    }

    VOID setExpectedFrame(UINT32 i, std::initializer_list<BYTE> bytes)
    {
        UINT32 len = (UINT32) bytes.size();
        mPExpectedFrameArr[i] = (PBYTE) MEMALLOC(len);
        UINT32 j = 0;
        for (BYTE b : bytes) {
            mPExpectedFrameArr[i][j++] = b;
        }
        mExpectedFrameSizeArr[i] = len;
    }

    VOID setPayloadToFree()
    {
        UINT32 i;
        for (i = 0; i < mRtpPacketCount; i++) {
            mPRtpPackets[i]->pRawPacket = mPRtpPackets[i]->payload;
        }
    }

    VOID clearJitterBufferForTest()
    {
        UINT32 i;
        EXPECT_EQ(STATUS_SUCCESS, freeJitterBuffer(&mJitterBuffer));
        if (mExpectedFrameCount > 0) {
            for (i = 0; i < mExpectedFrameCount; i++) {
                MEMFREE(mPExpectedFrameArr[i]);
            }
            MEMFREE(mPExpectedFrameArr);
            MEMFREE(mExpectedFrameSizeArr);
        }
        if (mExpectedDroppedFrameCount > 0) {
            MEMFREE(mExpectedDroppedFrameTimestampArr);
        }
        MEMFREE(mPRtpPackets);
        EXPECT_EQ(mExpectedFrameCount, mReadyFrameIndex);
        EXPECT_EQ(mExpectedDroppedFrameCount, mDroppedFrameIndex);
        if (mFrame != NULL) {
            MEMFREE(mFrame);
        }
    }
};

// Also works as closeBufferWithSingleContinousPacket
TEST_P(JitterBufferFunctionalityTest, continousPacketsComeInOrder)
{
    UINT32 i;
    UINT32 pktCount = 5;
    UINT32 startingSequenceNumber = 0;
    srand(time(0));
    startingSequenceNumber = rand() % UINT16_MAX;

    initializeJitterBuffer(3, 0, pktCount);

    // Frame "1" at ts=100 — packet #0
    setPacket(0, {1}, 100, startingSequenceNumber++, TRUE);
    setExpectedFrame(0, {1});

    // Frame "234" at ts=200 — packets #1, #2
    setPacket(1, {2}, 200, startingSequenceNumber++, TRUE);
    setPacket(2, {3, 4}, 200, startingSequenceNumber++, FALSE);
    setExpectedFrame(1, {2, 3, 4});

    // Frame "567" at ts=300 — packets #3, #4
    setPacket(3, {5, 6}, 300, startingSequenceNumber++, TRUE);
    setPacket(4, {7}, 300, startingSequenceNumber++, FALSE);
    setExpectedFrame(2, {5, 6, 7});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 1:
            case 2:
                EXPECT_EQ(1, mReadyFrameIndex);
                break;
            case 3:
            case 4:
                EXPECT_EQ(2, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, continousPacketsComeOutOfOrder)
{
    UINT32 i;
    UINT32 pktCount = 5;
    UINT32 startingSequenceNumber = 0;

    srand(time(0));
    startingSequenceNumber = rand() % UINT16_MAX;
    initializeJitterBuffer(3, 0, pktCount);

    DLOGI("Starting sequence number: %u\n", startingSequenceNumber);

    // Frame "1" at ts=100 — packet #0
    setPacket(0, {1}, 100, startingSequenceNumber++, TRUE);
    setExpectedFrame(0, {1});

    // Frame "234" at ts=200 — slots #3 (first) then #1 (following)
    setPacket(3, {2}, 200, startingSequenceNumber++, TRUE);
    setPacket(1, {3, 4}, 200, startingSequenceNumber++, FALSE);
    setExpectedFrame(1, {2, 3, 4});

    // Frame "567" at ts=300 — slots #2, #4
    setPacket(2, {5, 6}, 300, startingSequenceNumber++, TRUE);
    setPacket(4, {7}, 300, startingSequenceNumber++, FALSE);
    setExpectedFrame(2, {5, 6, 7});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 3:
            case 4:
                EXPECT_EQ(2, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

// This also serves as closeBufferWithMultipleImcompletePackets
TEST_P(JitterBufferFunctionalityTest, gapBetweenTwoContinousPackets)
{
    UINT32 i;
    UINT32 pktCount = 4;
    initializeJitterBuffer(1, 2, pktCount);

    // Frame "1?3" at ts=100 — missing middle packet (seq 1)
    setPacket(0, {1}, 100, 0, TRUE);
    setPacket(1, {3}, 100, 2, FALSE);

    // Frame "4" at ts=200 — missing following packet (seq 4)
    setPacket(2, {4}, 200, 3, TRUE);

    // Frame "6" at ts=400 (frame at ts=300 is entirely missing)
    setPacket(3, {6}, 400, 5, TRUE);

    mExpectedDroppedFrameTimestampArr[0] = 100;
    mExpectedDroppedFrameTimestampArr[1] = 200;

    setExpectedFrame(0, {6});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        EXPECT_EQ(0, mDroppedFrameIndex);
        EXPECT_EQ(0, mReadyFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, expiredCompleteFrameGotReadyFunc)
{
    UINT32 i;
    UINT32 pktCount = 2;
    initializeJitterBuffer(2, 0, pktCount);

    setPacket(0, {1}, 100, TRUE);
    setExpectedFrame(0, {1});

    setPacket(1, {2}, 3200, TRUE);
    setExpectedFrame(1, {2});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 1:
                EXPECT_EQ(1, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }
    clearJitterBufferForTest();
}

// This also serves as closeBufferWithImcompletePacketsAndSingleContinousPacket
TEST_P(JitterBufferFunctionalityTest, expiredIncompleteFrameGotDropFunc)
{
    UINT32 i;
    UINT32 pktCount = 2;
    initializeJitterBuffer(1, 1, pktCount);

    // Frame "1?" at ts=100 — following packet never arrives
    setPacket(0, {1}, 100, 0, TRUE);
    mExpectedDroppedFrameTimestampArr[0] = 100;

    // Frame "3" at ts=3200 — seq jumps past the missing packet
    setPacket(1, {3}, 3200, 2, TRUE);
    setExpectedFrame(0, {3});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
                EXPECT_EQ(0, mDroppedFrameIndex);
                break;
            case 1:
                EXPECT_EQ(1, mDroppedFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mReadyFrameIndex);
    }
    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, closeBufferWithSingleImcompletePacket)
{
    UINT32 i;
    UINT32 pktCount = 2;
    initializeJitterBuffer(0, 1, pktCount);

    setPacket(0, {1}, 100, 0, TRUE);
    setPacket(1, {3}, 100, 2, FALSE);

    mExpectedDroppedFrameTimestampArr[0] = 100;

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        EXPECT_EQ(0, mDroppedFrameIndex);
        EXPECT_EQ(0, mReadyFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, fillDataGiveExpectedData)
{
    PBYTE buffer = (PBYTE) MEMALLOC(2);
    UINT32 filledSize = 0, i = 0;
    BYTE expectedBuffer[] = {1, 2};
    initializeJitterBuffer(1, 0, 2);

    setPacket(0, {1}, 100, TRUE);
    setPacket(1, {2}, 100, FALSE);
    setExpectedFrame(0, {1, 2});

    setPayloadToFree();

    for (i = 0; i < 2; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
    }

    EXPECT_EQ(STATUS_SUCCESS, jitterBufferFillFrameData(mJitterBuffer, buffer, 2, &filledSize, 0, 1));
    EXPECT_EQ(2, filledSize);
    EXPECT_EQ(0, MEMCMP(buffer, expectedBuffer, 2));

    clearJitterBufferForTest();
    MEMFREE(buffer);
}

TEST_P(JitterBufferFunctionalityTest, fillDataReturnErrorWithImcompleteFrame)
{
    PBYTE buffer = (PBYTE) MEMALLOC(2);
    UINT32 filledSize = 0, i = 0;
    initializeJitterBuffer(0, 1, 2);

    setPacket(0, {1}, 100, 0, TRUE);
    setPacket(1, {2}, 100, 2, FALSE);

    mExpectedDroppedFrameTimestampArr[0] = 100;

    setPayloadToFree();

    for (i = 0; i < 2; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
    }

    EXPECT_EQ(STATUS_HASH_KEY_NOT_PRESENT, jitterBufferFillFrameData(mJitterBuffer, buffer, 2, &filledSize, 0, 1));

    clearJitterBufferForTest();
    MEMFREE(buffer);
}

TEST_P(JitterBufferFunctionalityTest, fillDataReturnErrorWithNotEnoughOutputBuffer)
{
    PBYTE buffer = (PBYTE) MEMALLOC(1);
    UINT32 filledSize = 0, i = 0;
    initializeJitterBuffer(1, 0, 2);

    setPacket(0, {1}, 100, TRUE);
    setPacket(1, {2}, 100, FALSE);
    setExpectedFrame(0, {1, 2});

    setPayloadToFree();

    for (i = 0; i < 2; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
    }

    EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, jitterBufferFillFrameData(mJitterBuffer, buffer, 1, &filledSize, 0, 1));

    clearJitterBufferForTest();
    MEMFREE(buffer);
}

TEST_P(JitterBufferFunctionalityTest, dropDataGivenSmallStartAndLargeEnd)
{
    UINT32 i = 0;
    initializeJitterBuffer(0, 1, 2);

    setPacket(0, {1}, 100, TRUE);
    setPacket(1, {2}, 100, FALSE);

    mExpectedDroppedFrameTimestampArr[0] = 100;

    setPayloadToFree();

    for (i = 0; i < 2; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
    }

    EXPECT_EQ(STATUS_SUCCESS, jitterBufferDropBufferData(mJitterBuffer, 1, 2, 100));

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, dropDataGivenLargeStartAndSmallEnd)
{
    UINT32 i = 0;
    initializeJitterBuffer(0, 0, 2);

    setPacket(0, {1}, 100, TRUE);
    setPacket(1, {2, 0}, 100, FALSE);

    setPayloadToFree();

    for (i = 0; i < 2; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
    }

    // Directly drops all frames 10 - 65535 and 0 - 2, so no frame will be reported as ready/dropped callback
    EXPECT_EQ(STATUS_SUCCESS, jitterBufferDropBufferData(mJitterBuffer, 10, 2, 100));

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, continousPacketsComeInCycling)
{
    UINT32 i;
    UINT32 pktCount = 4;
    initializeJitterBuffer(4, 0, pktCount);

    setPacket(0, {1}, 100, 65534, TRUE);
    setExpectedFrame(0, {1});

    setPacket(1, {2}, 200, 65535, TRUE);
    setExpectedFrame(1, {2});

    setPacket(2, {3}, 300, 0, TRUE);
    setExpectedFrame(2, {3});

    setPacket(3, {4}, 400, 1, TRUE);
    setExpectedFrame(3, {4});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 1:
                EXPECT_EQ(1, mReadyFrameIndex);
                break;
            case 2:
                EXPECT_EQ(2, mReadyFrameIndex);
                break;
            case 3:
                EXPECT_EQ(3, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, getFrameReadyAfterDroppedFrame)
{
    UINT32 i = 0;
    initializeJitterBuffer(3, 1, 5);

    // Frame "1?" at ts=100 — following packet dropped
    setPacket(0, {1}, 100, 0, TRUE);
    mExpectedDroppedFrameTimestampArr[0] = 100;

    // Frame "34" at ts=200
    setPacket(1, {3}, 200, 3, TRUE);
    setPacket(2, {4}, 200, 4, FALSE);
    setExpectedFrame(0, {3, 4});

    // Frame "5" at ts=300
    setPacket(3, {5}, 300, 5, TRUE);
    setExpectedFrame(1, {5});

    // Frame "6" at ts=3000 — forces expiry
    setPacket(4, {6}, 3000, 6, TRUE);
    setExpectedFrame(2, {6});

    setPayloadToFree();

    for (i = 0; i < 5; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
                EXPECT_EQ(0, mReadyFrameIndex);
                EXPECT_EQ(0, mDroppedFrameIndex);
                break;
            case 4:
                EXPECT_EQ(2, mReadyFrameIndex);
                EXPECT_EQ(1, mDroppedFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, pushFrameArrivingLate)
{
    UINT32 i = 0;
    initializeJitterBuffer(1, 0, 2);

    // Frame "1" at ts=3000 — packet #1
    setPacket(0, {1}, 3000, 1, TRUE);
    setExpectedFrame(0, {1});

    // Late packet at ts=200 — not pushed into buffer but must still get freed
    setPacket(1, {0}, 200, 0, TRUE);

    setPayloadToFree();

    for (i = 0; i < 2; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        EXPECT_EQ(0, mReadyFrameIndex);
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, missingSecondPacketInSecondFrame)
{
    UINT32 i;
    UINT32 pktCount = 7;
    initializeJitterBuffer(2, 1, pktCount);

    // Frame "1273" at ts=100
    setPacket(0, {1}, 100, 0, TRUE);
    setPacket(1, {2}, 100, 1, FALSE);
    setPacket(2, {7}, 100, 2, FALSE);
    setPacket(3, {3}, 100, 3, FALSE);
    setExpectedFrame(0, {1, 2, 7, 3});

    // Frame "4?3" at ts=200 — missing seq 5
    setPacket(4, {4}, 200, 4, TRUE);
    setPacket(5, {3}, 200, 6, FALSE);

    // Frame "6" at ts=400
    setPacket(6, {6}, 400, 7, TRUE);

    mExpectedDroppedFrameTimestampArr[0] = 200;

    setExpectedFrame(1, {6});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 4:
            case 5:
            case 6:
                EXPECT_EQ(1, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }
    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, incompleteFirstFrame)
{
    UINT32 i;
    UINT32 pktCount = 5;
    initializeJitterBuffer(2, 1, pktCount);

    // Incomplete first frame at ts=100 — no start packet
    setPacket(0, {1}, 100, FALSE);

    // Frame "234" at ts=200
    setPacket(1, {2}, 200, TRUE);
    setPacket(2, {3, 4}, 200, FALSE);
    setExpectedFrame(0, {2, 3, 4});

    // Frame "567" at ts=300
    setPacket(3, {5, 6}, 300, TRUE);
    setPacket(4, {7}, 300, FALSE);
    setExpectedFrame(1, {5, 6, 7});

    mExpectedDroppedFrameTimestampArr[0] = 100;
    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        EXPECT_EQ(0, mReadyFrameIndex);
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, outOfOrderFirstFrame)
{
    UINT32 i;
    UINT32 pktCount = 7;
    initializeJitterBuffer(3, 0, pktCount);

    // Frame at ts=100 arrives reordered as: following(1), then following(2), then start(0)
    setPacket(0, {1}, 100, 1, FALSE);

    // Frame "234" at ts=200 — seqs 3, 4
    setPacket(1, {2}, 200, 3, TRUE);
    setPacket(2, {3, 4}, 200, 4, FALSE);

    setPacket(3, {9}, 100, 2, FALSE);
    setPacket(4, {8}, 100, 0, TRUE);

    // Expected frames "819" (ts=100) and "234" (ts=200)
    setExpectedFrame(0, {8, 1, 9});
    setExpectedFrame(1, {2, 3, 4});

    // Frame "567" at ts=300 — seqs 5, 6
    setPacket(5, {5, 6}, 300, 5, TRUE);
    setPacket(6, {7}, 300, 6, FALSE);
    setExpectedFrame(2, {5, 6, 7});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 4:
                EXPECT_EQ(1, mReadyFrameIndex);
                break;
            case 5:
                EXPECT_EQ(2, mReadyFrameIndex);
                break;
            case 6:
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, latePacketsOfAlreadyDroppedFrame)
{
    UINT32 i = 0;
    UINT32 pktCount = 4;
    initializeJitterBuffer(1, 1, pktCount);

    setPacket(0, {0}, 100, 0, TRUE);
    setPacket(1, {1, 1, 1, 1}, 100, 1, FALSE);

    // Frame "1" at ts=3000 — forces drop of earlier incomplete frame due to max-latency
    setPacket(2, {1}, 3000, 3, TRUE);
    setExpectedFrame(0, {1});

    setPacket(3, {2}, 100, 2, FALSE);

    mExpectedDroppedFrameTimestampArr[0] = 100;

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
                EXPECT_EQ(0, mReadyFrameIndex);
                EXPECT_EQ(0, mDroppedFrameIndex);
                break;
            case 2:
                EXPECT_EQ(0, mReadyFrameIndex);
                EXPECT_EQ(1, mDroppedFrameIndex);
                break;
            case 3:
                EXPECT_EQ(0, mReadyFrameIndex);
                EXPECT_EQ(1, mDroppedFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, timestampOverflowTest)
{
    UINT32 i;
    UINT32 pktCount = 7;
    UINT32 startingSequenceNumber = 0;
    UINT32 missingSequenceNumber = 0;
    initializeJitterBuffer(4, 0, pktCount);
    srand(time(0));
    startingSequenceNumber = rand() % UINT16_MAX;

    // Frame "1234" straddling the timestamp boundary — packet for "4" arrives last (packet #6)
    setPacket(0, {1}, MAX_RTP_TIMESTAMP - 500, startingSequenceNumber++, TRUE);
    setPacket(1, {2}, MAX_RTP_TIMESTAMP - 500, startingSequenceNumber++, FALSE);
    setPacket(2, {3}, MAX_RTP_TIMESTAMP - 500, startingSequenceNumber++, FALSE);
    missingSequenceNumber = startingSequenceNumber++;
    setExpectedFrame(0, {1, 2, 3, 4});

    setPacket(3, {2}, MAX_RTP_TIMESTAMP - 100, startingSequenceNumber++, TRUE);
    setExpectedFrame(1, {2});

    setPacket(4, {3}, 300, startingSequenceNumber++, TRUE);
    setExpectedFrame(2, {3});

    setPacket(5, {4}, 600, startingSequenceNumber++, TRUE);
    setExpectedFrame(3, {4});

    // Late packet completing frame 0
    setPacket(6, {4}, MAX_RTP_TIMESTAMP - 500, missingSequenceNumber, FALSE);

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 6:
                EXPECT_EQ(3, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, timestampUnderflowTest)
{
    UINT32 i;
    UINT32 pktCount = 8;
    UINT32 startingSequenceNumber = 0;
    UINT32 missingSequenceNumber = 0;
    UINT32 firstSequenceNumber = 0;
    srand(time(0));
    startingSequenceNumber = rand() % UINT16_MAX;
    firstSequenceNumber = startingSequenceNumber - 1;

    initializeJitterBuffer(5, 0, pktCount);

    // Frame "1234" at ts=0 — "4" arrives late
    setPacket(0, {1}, 0, startingSequenceNumber++, TRUE);
    setPacket(1, {2}, 0, startingSequenceNumber++, FALSE);
    setPacket(2, {3}, 0, startingSequenceNumber++, FALSE);
    missingSequenceNumber = startingSequenceNumber++;
    setExpectedFrame(1, {1, 2, 3, 4});

    setPacket(3, {2}, 300, startingSequenceNumber++, TRUE);
    setExpectedFrame(2, {2});

    setPacket(4, {3}, 600, startingSequenceNumber++, TRUE);
    setExpectedFrame(3, {3});

    setPacket(5, {4}, 900, startingSequenceNumber++, TRUE);
    setExpectedFrame(4, {4});

    // Actual first frame, underflow timestamp — seq number underflows too
    setPacket(6, {4}, MAX_RTP_TIMESTAMP - 300, firstSequenceNumber, TRUE);
    setExpectedFrame(0, {4});

    // Missing 4th packet in frame "1234"
    setPacket(7, {4}, 0, missingSequenceNumber, FALSE);

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 6:
                EXPECT_EQ(1, mReadyFrameIndex);
                break;
            case 7:
                EXPECT_EQ(4, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, SequenceNumberOverflowTest)
{
    UINT32 i;
    UINT32 pktCount = 7;
    initializeJitterBuffer(4, 0, pktCount);

    // Frame "1234" at ts=100 straddling seq wraparound 65534..0..1, seq 1 arrives last
    setPacket(0, {1}, 100, 65534, TRUE);
    setPacket(1, {2}, 100, 65535, FALSE);
    setPacket(2, {3}, 100, 0, FALSE);
    setExpectedFrame(0, {1, 2, 3, 4});

    setPacket(3, {2}, 200, 2, TRUE);
    setExpectedFrame(1, {2});

    setPacket(4, {3}, 300, 3, TRUE);
    setExpectedFrame(2, {3});

    setPacket(5, {4}, 400, 4, TRUE);
    setExpectedFrame(3, {4});

    // Late packet completing frame 0
    setPacket(6, {4}, 100, 1, FALSE);

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 6:
                EXPECT_EQ(3, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, SequenceNumberUnderflowTest)
{
    UINT32 i;
    UINT32 pktCount = 8;
    UINT32 startingSequenceNumber = 0;
    UINT32 missingSequenceNumber = 0;
    UINT32 firstSequenceNumber = MAX_RTP_SEQUENCE_NUM - 2;

    srand(time(0));
    UINT32 startingTimestamp = rand() % UINT32_MAX;

    initializeJitterBuffer(5, 0, pktCount);

    // Frame "1234" — arrives fourth in time order, "4" arrives last
    setPacket(0, {1}, startingTimestamp + 300, startingSequenceNumber++, TRUE);
    setPacket(1, {2}, startingTimestamp + 300, startingSequenceNumber++, FALSE);
    setPacket(2, {3}, startingTimestamp + 300, startingSequenceNumber++, FALSE);
    missingSequenceNumber = startingSequenceNumber++;
    setExpectedFrame(3, {1, 2, 3, 4});

    // First frame "2" — seq number underflow range
    setPacket(3, {2}, startingTimestamp, firstSequenceNumber++, TRUE);
    setExpectedFrame(0, {2});

    setPacket(4, {3}, startingTimestamp + 100, firstSequenceNumber++, TRUE);
    setExpectedFrame(1, {3});

    setPacket(5, {4}, startingTimestamp + 200, firstSequenceNumber++, TRUE);
    setExpectedFrame(2, {4});

    setPacket(6, {4}, startingTimestamp + 400, startingSequenceNumber++, TRUE);
    setExpectedFrame(4, {4});

    // Missing 4th packet in frame "1234"
    setPacket(7, {4}, startingTimestamp + 300, missingSequenceNumber, FALSE);

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 4:
                EXPECT_EQ(1, mReadyFrameIndex);
                break;
            case 5:
            case 6:
                EXPECT_EQ(3, mReadyFrameIndex);
                break;
            case 7:
                EXPECT_EQ(4, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, DoubleOverflowTest)
{
    UINT32 i;
    UINT32 pktCount = 7;
    initializeJitterBuffer(4, 0, pktCount);

    // Frame "1234" straddling both seq and timestamp wraparound
    setPacket(0, {1}, MAX_RTP_TIMESTAMP - 500, 65534, TRUE);
    setPacket(1, {2}, MAX_RTP_TIMESTAMP - 500, 65535, FALSE);
    setPacket(2, {3}, MAX_RTP_TIMESTAMP - 500, 0, FALSE);
    setExpectedFrame(0, {1, 2, 3, 4});

    setPacket(3, {2}, MAX_RTP_TIMESTAMP - 100, 2, TRUE);
    setExpectedFrame(1, {2});

    setPacket(4, {3}, 300, 3, TRUE);
    setExpectedFrame(2, {3});

    setPacket(5, {4}, 600, 4, TRUE);
    setExpectedFrame(3, {4});

    // Late packet completing frame 0
    setPacket(6, {4}, MAX_RTP_TIMESTAMP - 500, 1, FALSE);

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 6:
                EXPECT_EQ(3, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

#if 0
//TODO complete this test
TEST_P(JitterBufferFunctionalityTest, LongRunningWithDroppedPacketsTest)
{
    UINT32 timeStep = 100;
    UINT16 startingSequenceNumber = 0;
    UINT32 startingTimestamp = 0;
    UINT32 totalPackets = 0;
    UINT32 droppedPackets = 0;
    UINT32 rangeForNextDrop = 0;
    UINT32 nextDrop = 0;
    UINT32 dropSection = 0;
    UINT32 packetCount = 0;
    UINT32 frameCount = 0;
    UINT32 readyFrameCount = 0;
    UINT32 droppedFrameCount = 0;
    UINT32 currentDroppedPacket = 0;
    BOOL dropped = FALSE;
    std::vector<UINT32> dropNextPacket;
    std::vector<UINT32> packetsPerFrame;
    std::vector<UINT32> packetsOfFrameToDrop;
  
    srand(time(0));

    //between 1-3 UINT16 cycles
    totalPackets = rand()%(UINT16_MAX*2) + UINT16_MAX;
    //100-200 dropped OR very late packets
    droppedPackets = rand()%(100) + 100;
    startingSequenceNumber = rand()%UINT16_MAX;
    startingTimestamp = rand()%UINT32_MAX;

    dropSection = totalPackets/droppedPackets;
    rangeForNextDrop = dropSection;
    for(auto i = 0; i < droppedPackets; i++) {
        nextDrop = rand()%rangeForNextDrop;
        rangeForNextDrop += std::abs(dropSection-nextDrop);
        if(i == 0) {
            dropNextPacket.push_back(nextDrop);
        }
        else {
            dropNextPacket.push_back(dropNextPacket.back() + nextDrop);
        }
    }

    while(packetCount < totalPackets) {
        if((totalPackets - packetCount) <= 30) {
            packetsPerFrame.push_back(totalPackets-packetCount);
        }
        else {
            packetsPerFrame.push_back(rand()%29 + 2);
        }
        packetCount += packetsPerFrame.back();
        frameCount++;
        while(packetCount > dropNextPacket[currentDroppedPacket]) {
            dropped = TRUE;
            currentDroppedPacket++;
        }
        if(dropped) {
            droppedFrameCount++;
        }
        else {
            readyFrameCount++;
        }
        dropped = FALSE;
    }

    initializeJitterBuffer(readyFrameCount, droppedFrameCount, totalPackets);

    dropped = FALSE;
    packetCount = 0;
    droppedFrameCount = 0;
    currentDroppedPacket = 0;

    for(auto frame = 0; frame < frameCount; frame++) {
        //is frame missing a packet
        while((packetsPerFrame[frame] + packetCount ) > dropNextPacket[currentDroppedPacket]){
            dropped = TRUE;
            packetsOfFrameToDrop.push_back(dropNextPacket[currentDroppedPacket] - packetCount);
            currentDroppedPacket++;
        }
        if(dropped) {
            droppedFrameCount++;
        }
        else {
            mPExpectedFrameArr[frame-droppedFrameCount] = (PBYTE) MEMALLOC(packetsPerFrame[frame]);
            mExpectedFrameSizeArr[frame-droppedFrameCount] = packetsPerFrame[frame];
        }
        for(auto packet = 0; packet < packetsPerFrame[frame]; packet++) {
            if(!dropped) {
                mPExpectedFrameArr[frame-droppedFrameCount][packet] = packet;
            }

            mPRtpPackets[packetCount]->payloadLength = 1;
            mPRtpPackets[packetCount]->payload = (PBYTE) MEMALLOC(mPRtpPackets[packetCount]->payloadLength + 1);
            mPRtpPackets[packetCount]->payload[0] = packet;
            if(packet == 0) {
                mPRtpPackets[packetCount]->payload[1] = 1; // First packet of a frame
            }
            else {
                mPRtpPackets[packetCount]->payload[1] = 0;
            }
            mPRtpPackets[packetCount]->header.timestamp = startingTimestamp;
            mPRtpPackets[packetCount]->header.sequenceNumber = startingSequenceNumber++;

            packetCount++;
        }
        if(dropped) {
            mExpectedDroppedFrameTimestampArr[droppedFrameCount-1] = startingTimestamp;
        }
        startingTimestamp += timeStep;

    }

    setPayloadToFree();

    for(auto i = 0; i < totalPackets; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
    }
    clearJitterBufferForTest();
}
#endif

// Regression test: push sequential multi-packet frames (3 packets each) through
// two full seq-number cycles (~131K packets). Frames are fence-completed when
// the next frame's first packet arrives — matching real H.264 RTP behavior.
// Without the fix, stale head state after the buffer empties causes false
// timestamp-overflow detection, silently discarding all subsequent packets
// partway through the second seq cycle.
TEST_P(JitterBufferFunctionalityTest, deliveryContinuesAfterSeqWrapAndEmptyBuffer)
{
    if (!GetParam()) {
        // DefaultJitterBuffer has a pre-existing bug with long-running streams
        // across seq wraparound (delivers only ~half the frames). Skip until fixed.
        GTEST_SKIP();
    }

    constexpr UINT32 PACKETS_PER_FRAME = 3;
    constexpr UINT32 TOTAL_PACKETS = 131073; // 43691 * 3
    constexpr UINT32 FRAME_COUNT = TOTAL_PACKETS / PACKETS_PER_FRAME;

    initializeJitterBuffer(FRAME_COUNT, 0, TOTAL_PACKETS);

    UINT32 timestamp = 100;
    for (UINT32 frame = 0; frame < FRAME_COUNT; frame++) {
        setExpectedFrame(frame, {0, 1, 2});

        for (UINT32 p = 0; p < PACKETS_PER_FRAME; p++) {
            UINT32 pktIdx = frame * PACKETS_PER_FRAME + p;
            setPacket(pktIdx, {(BYTE) p}, timestamp, (UINT16) pktIdx, p == 0, p == PACKETS_PER_FRAME - 1);
        }
        timestamp += 200;
    }
    setPayloadToFree();

    for (UINT32 i = 0; i < TOTAL_PACKETS; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
    }

    // Marker bit on last packet completes each frame immediately
    EXPECT_EQ(FRAME_COUNT, mReadyFrameIndex);
    EXPECT_EQ(0, mDroppedFrameIndex);

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, markerBitTriggersImmediateDelivery)
{
    UINT32 i;
    UINT32 pktCount = 4;
    UINT32 startingSequenceNumber = 0;

    srand(time(0));
    startingSequenceNumber = rand() % UINT16_MAX;

    initializeJitterBuffer(2, 0, pktCount);

    // First frame: 3 packets with marker on the last, ts=100
    setPacket(0, {1}, 100, startingSequenceNumber++, TRUE);
    setPacket(1, {2}, 100, startingSequenceNumber++, FALSE);
    setPacket(2, {3}, 100, startingSequenceNumber++, FALSE, TRUE);
    setExpectedFrame(0, {1, 2, 3});

    // Second frame: single packet with marker, ts=200
    setPacket(3, {4}, 200, startingSequenceNumber++, TRUE, TRUE);
    setExpectedFrame(1, {4});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        switch (i) {
            case 0:
            case 1:
            case 2:
                // First frame not delivered yet: marker-bit delivery is skipped for the very first
                // frame to avoid false delivery when reordered packets look like complete single-packet
                // frames. The first frame is delivered via frame-boundary when the next frame arrives.
                EXPECT_EQ(0, mReadyFrameIndex);
                break;
            case 3:
                // Frame 1 delivered via frame-boundary (ts change 100->200), then frame 2
                // delivered via marker-bit (firstFrameProcessed is now TRUE)
                EXPECT_EQ(2, mReadyFrameIndex);
                break;
            default:
                ASSERT_TRUE(FALSE);
        }
        EXPECT_EQ(0, mDroppedFrameIndex);
    }

    clearJitterBufferForTest();
}

TEST_P(JitterBufferFunctionalityTest, markerBitOutOfOrderWaitsForCompletion)
{
    UINT32 i;
    UINT32 pktCount = 3;

    initializeJitterBuffer(1, 0, pktCount);

    // Frame with 3 packets, but marker arrives before middle packet.
    // Push order: seq 0 (start), seq 2 (marker), seq 1 (middle). Frame should NOT
    // be delivered until all packets arrive, even though the marker arrived early.
    setPacket(0, {1}, 100, 0, TRUE);
    setPacket(1, {3}, 100, 2, FALSE, TRUE);
    setPacket(2, {2}, 100, 1, FALSE);

    setExpectedFrame(0, {1, 2, 3});

    setPayloadToFree();

    for (i = 0; i < pktCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferPush(mJitterBuffer, mPRtpPackets[i], nullptr));
        // Marker-bit delivery is skipped for the very first frame to prevent false delivery
        // of reordered packets. This frame will be delivered during flush.
        EXPECT_EQ(0, mReadyFrameIndex);
    }

    // Frame is delivered during flush (freeJitterBuffer with bufferClosed=TRUE).
    // clearJitterBufferForTest verifies mReadyFrameIndex == mExpectedFrameCount.
    clearJitterBufferForTest();
}

INSTANTIATE_TEST_SUITE_P(DefaultJitterBuffer, JitterBufferFunctionalityTest, ::testing::Values(false));
INSTANTIATE_TEST_SUITE_P(RealTimeJitterBuffer, JitterBufferFunctionalityTest, ::testing::Values(true));

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
