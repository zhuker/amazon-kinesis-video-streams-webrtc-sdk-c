#include "WebRTCClientTestFixture.h"

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

#define NUMBER_OF_FRAME_FILES 403
#define NUMBER_OF_H265_FRAME_FILES 1500
#define DEFAULT_FPS_VALUE     25
BYTE start4ByteCode[] = {0x00, 0x00, 0x00, 0x01};

class RtpFunctionalityTest : public WebRtcClientTestBase {
  protected:
    void verifyH264PackingUnpacking(const char* sampleFolder, UINT32 numFrames);
};

TEST_F(RtpFunctionalityTest, packetUnderflow)
{
    BYTE rawPacket[] = {0x00, 0x00, 0x00, 0x00};
    RtpPacket rtpPacket;

    MEMSET(&rtpPacket, 0x00, SIZEOF(RtpPacket));

    for (auto i = 0; i <= 12; i++) {
        ASSERT_EQ(setRtpPacketFromBytes(rawPacket, SIZEOF(rawPacket), &rtpPacket), STATUS_RTP_INPUT_PACKET_TOO_SMALL);
    }
}

TEST_F(RtpFunctionalityTest, marshallUnmarshallGettingSameData)
{
    BYTE payload[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    UINT32 payloadLen = 6;
    PayloadArray payloadArray;
    PRtpPacket packetList = NULL;
    PRtpPacket pRtpPacket = NULL;
    PRtpPacket pNewRtpPacket = NULL;
    PBYTE rawPacket = NULL;
    UINT32 packetLen = 0;
    UINT32 i = 0;

    payloadArray.payloadBuffer = payload;
    payloadArray.payloadLength = payloadLen;
    payloadArray.payloadSubLength = &payloadLen;
    payloadArray.payloadSubLenSize = 1;

    packetList = (PRtpPacket) MEMALLOC(SIZEOF(RtpPacket));

    SRAND(GETTIME());
    EXPECT_EQ(STATUS_SUCCESS,
              constructRtpPackets(&payloadArray, 8, 1, 1324857487, 0x1234ABCD, (PRtpPacket) packetList, payloadArray.payloadSubLenSize));

    EXPECT_NE(NULL, (UINT64) packetList);

    for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
        pRtpPacket = packetList + i;

        EXPECT_EQ(STATUS_SUCCESS, createBytesFromRtpPacket(pRtpPacket, NULL, &packetLen));
        EXPECT_TRUE(NULL != (rawPacket = (PBYTE) MEMALLOC(packetLen)));
        EXPECT_EQ(STATUS_SUCCESS, createBytesFromRtpPacket(pRtpPacket, rawPacket, &packetLen));
        EXPECT_EQ(STATUS_SUCCESS, createRtpPacketFromBytes(rawPacket, packetLen, &pNewRtpPacket));
        // Verify the extracted header is the same as original header
        EXPECT_EQ(pRtpPacket->header.version, pNewRtpPacket->header.version);
        EXPECT_EQ(pRtpPacket->header.sequenceNumber, pNewRtpPacket->header.sequenceNumber);
        EXPECT_EQ(pRtpPacket->header.ssrc, pNewRtpPacket->header.ssrc);
        EXPECT_EQ(pRtpPacket->header.csrcArray, pNewRtpPacket->header.csrcArray);
        EXPECT_EQ(pRtpPacket->header.extensionPayload, pNewRtpPacket->header.extensionPayload);
        EXPECT_EQ(pRtpPacket->header.extension, pNewRtpPacket->header.extension);
        EXPECT_EQ(pRtpPacket->header.timestamp, pNewRtpPacket->header.timestamp);
        EXPECT_EQ(pRtpPacket->header.extensionProfile, pNewRtpPacket->header.extensionProfile);
        EXPECT_EQ(pRtpPacket->header.payloadType, pNewRtpPacket->header.payloadType);
        EXPECT_EQ(pRtpPacket->header.padding, pNewRtpPacket->header.padding);
        EXPECT_EQ(pRtpPacket->header.csrcCount, pNewRtpPacket->header.csrcCount);
        EXPECT_EQ(pRtpPacket->header.extensionLength, pNewRtpPacket->header.extensionLength);
        EXPECT_EQ(pRtpPacket->header.marker, pNewRtpPacket->header.marker);

        // Verify the extracted payload is the same as original payload
        EXPECT_EQ(pRtpPacket->payloadLength, pNewRtpPacket->payloadLength);
        EXPECT_TRUE(MEMCMP(pRtpPacket->payload, pNewRtpPacket->payload, pRtpPacket->payloadLength) == 0);

        EXPECT_EQ(STATUS_SUCCESS, freeRtpPacket(&pNewRtpPacket));
    }

    MEMFREE(packetList);
}

TEST_F(RtpFunctionalityTest, marshallUnmarshallH264Data)
{
    PBYTE payload = (PBYTE) MEMALLOC(200000); // Assuming this is enough
    UINT32 payloadLen = 0;
    PayloadArray payloadArray;
    PRtpPacket pPacketList = NULL;
    PRtpPacket pRtpPacket = NULL;
    PBYTE rawPacket = NULL;
    UINT32 packetLen = 0;
    UINT64 curTime = GETTIME();
    UINT32 fileIndex = 0;
    UINT16 seqNum = 0;
    UINT64 startTimeStamp = curTime;
    UINT32 i = 0;

    payloadArray.maxPayloadLength = 0;
    payloadArray.maxPayloadSubLenSize = 0;
    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    for (auto sentAllFrames = 0; sentAllFrames <= 5;) {
        if (fileIndex == NUMBER_OF_FRAME_FILES) {
            sentAllFrames++;
        }

        fileIndex = fileIndex % NUMBER_OF_FRAME_FILES + 1;
        EXPECT_EQ(STATUS_SUCCESS, readFrameData((PBYTE) payload, (PUINT32) &payloadLen, fileIndex, (PCHAR) "../samples/h264SampleFrames", RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE));

        // First call for payload size and sub payload length size
        EXPECT_EQ(STATUS_SUCCESS,
                  createPayloadForH264(DEFAULT_MTU_SIZE_BYTES, (PBYTE) payload, payloadLen, NULL, &payloadArray.payloadLength, NULL,
                                       &payloadArray.payloadSubLenSize));

        if (payloadArray.payloadLength > payloadArray.maxPayloadLength) {
            if (payloadArray.payloadBuffer != NULL) {
                MEMFREE(payloadArray.payloadBuffer);
            }
            payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
            payloadArray.maxPayloadLength = payloadArray.payloadLength;
        }
        if (payloadArray.payloadSubLenSize > payloadArray.maxPayloadSubLenSize) {
            if (payloadArray.payloadSubLength != NULL) {
                MEMFREE(payloadArray.payloadSubLength);
            }
            payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
            payloadArray.maxPayloadSubLenSize = payloadArray.payloadSubLenSize;
        }

        // Second call with actual buffer to fill in data
        EXPECT_EQ(STATUS_SUCCESS,
                  createPayloadForH264(DEFAULT_MTU_SIZE_BYTES, (PBYTE) payload, payloadLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                       payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

        EXPECT_LT(0, payloadArray.payloadSubLenSize);
        pPacketList = (PRtpPacket) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(RtpPacket));

        constructRtpPackets(&payloadArray, 96, seqNum, (UINT32)((curTime - startTimeStamp) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND), 0x1234ABCD,
                            pPacketList, payloadArray.payloadSubLenSize);

        seqNum = GET_UINT16_SEQ_NUM(seqNum + payloadArray.payloadSubLenSize);

        for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
            pRtpPacket = pPacketList + i;
            EXPECT_EQ(STATUS_SUCCESS, createBytesFromRtpPacket(pRtpPacket, NULL, &packetLen));
            EXPECT_TRUE(NULL != (rawPacket = (PBYTE) MEMALLOC(packetLen)));
            EXPECT_EQ(STATUS_SUCCESS, createBytesFromRtpPacket(pRtpPacket, rawPacket, &packetLen));

            MEMFREE(rawPacket);
            rawPacket = NULL;
        }
        curTime = GETTIME();

        MEMFREE(pPacketList);

        pPacketList = NULL;
    }

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(payload);
}

// Helper function to extract NAL units from Annex-B formatted data
// Returns the number of NAL units found, and populates naluOffsets and naluLengths arrays
static UINT32 extractNaluInfo(PBYTE data, UINT32 dataLen, PUINT32 naluOffsets, PUINT32 naluLengths, UINT32 maxNalus)
{
    UINT32 naluCount = 0;
    UINT32 i = 0;
    UINT32 startCodeLen = 0;
    UINT32 naluStart = 0;

    while (i < dataLen && naluCount < maxNalus) {
        // Look for start code (0x000001 or 0x00000001)
        if (i + 2 < dataLen && data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                startCodeLen = 3;
            } else if (i + 3 < dataLen && data[i + 2] == 0 && data[i + 3] == 1) {
                startCodeLen = 4;
            } else {
                i++;
                continue;
            }

            // If we had a previous NAL, record its length
            if (naluCount > 0) {
                naluLengths[naluCount - 1] = i - naluStart;
            }

            // Record start of new NAL (after start code)
            naluStart = i + startCodeLen;
            naluOffsets[naluCount] = naluStart;
            naluCount++;
            i += startCodeLen;
        } else {
            i++;
        }
    }

    // Record length of last NAL
    if (naluCount > 0) {
        naluLengths[naluCount - 1] = dataLen - naluStart;
    }

    return naluCount;
}

void RtpFunctionalityTest::verifyH264PackingUnpacking(const char* sampleFolder, UINT32 numFrames)
{
    PBYTE payload = (PBYTE) MEMCALLOC(1, 500000);
    PBYTE depayloadBuffer = (PBYTE) MEMCALLOC(1, 500000);
    PBYTE depayloadTemp = (PBYTE) MEMCALLOC(1, 500000);
    UINT32 payloadLen = 0;
    UINT32 fileIndex = 0;
    PayloadArray payloadArray;
    UINT32 i = 0;
    UINT32 offset = 0;
    UINT32 depayloadOffset = 0;
    UINT32 newPayloadSubLen = 0;
    BOOL isStartPacket = FALSE;
    const UINT32 MAX_NALUS = 128;
    UINT32 origNaluOffsets[MAX_NALUS];
    UINT32 origNaluLengths[MAX_NALUS];
    UINT32 depayNaluOffsets[MAX_NALUS];
    UINT32 depayNaluLengths[MAX_NALUS];
    UINT32 origNaluCount, depayNaluCount;

    payloadArray.maxPayloadLength = 0;
    payloadArray.maxPayloadSubLenSize = 0;
    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    for (fileIndex = 1; fileIndex <= numFrames; fileIndex++) {
        EXPECT_EQ(STATUS_SUCCESS,
                  readFrameData(payload, &payloadLen, fileIndex, (PCHAR) sampleFolder,
                                RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE));
        EXPECT_EQ(STATUS_SUCCESS,
                  createPayloadForH264(DEFAULT_MTU_SIZE_BYTES, payload, payloadLen, NULL, &payloadArray.payloadLength, NULL,
                                       &payloadArray.payloadSubLenSize));
        if (payloadArray.payloadLength > payloadArray.maxPayloadLength) {
            if (payloadArray.payloadBuffer != NULL) {
                MEMFREE(payloadArray.payloadBuffer);
            }
            payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
            payloadArray.maxPayloadLength = payloadArray.payloadLength;
        }
        if (payloadArray.payloadSubLenSize > payloadArray.maxPayloadSubLenSize) {
            if (payloadArray.payloadSubLength != NULL) {
                MEMFREE(payloadArray.payloadSubLength);
            }
            payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
            payloadArray.maxPayloadSubLenSize = payloadArray.payloadSubLenSize;
        }
        EXPECT_EQ(STATUS_SUCCESS,
                  createPayloadForH264(DEFAULT_MTU_SIZE_BYTES, payload, payloadLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                       payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));
        EXPECT_LT(0, payloadArray.payloadSubLenSize);

        offset = 0;
        depayloadOffset = 0;
        for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
            newPayloadSubLen = 500000 - depayloadOffset;
            EXPECT_EQ(STATUS_SUCCESS,
                      depayH264FromRtpPayload(payloadArray.payloadBuffer + offset, payloadArray.payloadSubLength[i], depayloadTemp,
                                              &newPayloadSubLen, &isStartPacket));
            EXPECT_LT(0, newPayloadSubLen);
            MEMCPY(depayloadBuffer + depayloadOffset, depayloadTemp, newPayloadSubLen);
            depayloadOffset += newPayloadSubLen;
            offset += payloadArray.payloadSubLength[i];
        }

        origNaluCount = extractNaluInfo(payload, payloadLen, origNaluOffsets, origNaluLengths, MAX_NALUS);
        depayNaluCount = extractNaluInfo(depayloadBuffer, depayloadOffset, depayNaluOffsets, depayNaluLengths, MAX_NALUS);
        EXPECT_EQ(origNaluCount, depayNaluCount) << "Frame " << fileIndex << " NAL count mismatch";
        for (i = 0; i < origNaluCount && i < depayNaluCount; i++) {
            EXPECT_EQ(origNaluLengths[i], depayNaluLengths[i]) << "Frame " << fileIndex << " NAL " << i << " length mismatch";
            if (origNaluLengths[i] == depayNaluLengths[i]) {
                EXPECT_TRUE(MEMCMP(payload + origNaluOffsets[i], depayloadBuffer + depayNaluOffsets[i], origNaluLengths[i]) == 0)
                    << "Frame " << fileIndex << " NAL " << i << " data mismatch";
            }
        }
    }

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(payload);
    MEMFREE(depayloadBuffer);
    MEMFREE(depayloadTemp);
}
TEST_F(RtpFunctionalityTest, packingUnpackingVerifySameH264Frame)
{
    verifyH264PackingUnpacking("../samples/h264SampleFrames", NUMBER_OF_FRAME_FILES);
}

TEST_F(RtpFunctionalityTest, packingUnpackingVerifyBbbH264)
{
    verifyH264PackingUnpacking("../samples/bbbH264", 1500);
}

TEST_F(RtpFunctionalityTest, packingUnpackingVerifyGirH264)
{
    verifyH264PackingUnpacking("../samples/girH264", 1500);
}

TEST_F(RtpFunctionalityTest, packingUnpackingVerifySameH265Frame)
{
    PBYTE payload = (PBYTE) MEMCALLOC(1, 200000); // Assuming this is enough
    PBYTE depayload = (PBYTE) MEMCALLOC(1, 1500); // This is more than max mtu
    UINT32 depayloadSize = 1500;
    UINT32 payloadLen = 0;
    UINT32 fileIndex = 0;
    PayloadArray payloadArray;
    UINT32 i = 0;
    UINT32 offset = 0;
    UINT32 newPayloadLen = 0, newPayloadSubLen = 0;
    BOOL isStartPacket = FALSE;
    PBYTE pCurPtrInPayload = NULL;
    UINT32 remainPayloadLen = 0;
    UINT32 startIndex = 0, naluLength = 0;
    UINT32 startLen = 0;

    payloadArray.maxPayloadLength = 0;
    payloadArray.maxPayloadSubLenSize = 0;
    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    for (fileIndex = 1; fileIndex <= NUMBER_OF_H265_FRAME_FILES; fileIndex++) {
        EXPECT_EQ(STATUS_SUCCESS, readFrameData((PBYTE) payload, (PUINT32) &payloadLen, fileIndex, (PCHAR) "../samples/h265SampleFrames", RTC_CODEC_H265));

        // First call for payload size and sub payload length size
        EXPECT_EQ(STATUS_SUCCESS,
                  createPayloadForH265(DEFAULT_MTU_SIZE_BYTES, (PBYTE) payload, payloadLen, NULL, &payloadArray.payloadLength, NULL,
                                       &payloadArray.payloadSubLenSize));

        if (payloadArray.payloadLength > payloadArray.maxPayloadLength) {
            if (payloadArray.payloadBuffer != NULL) {
                MEMFREE(payloadArray.payloadBuffer);
            }
            payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
            payloadArray.maxPayloadLength = payloadArray.payloadLength;
        }
        if (payloadArray.payloadSubLenSize > payloadArray.maxPayloadSubLenSize) {
            if (payloadArray.payloadSubLength != NULL) {
                MEMFREE(payloadArray.payloadSubLength);
            }
            payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
            payloadArray.maxPayloadSubLenSize = payloadArray.payloadSubLenSize;
        }

        // Second call with actual buffer to fill in data
        EXPECT_EQ(STATUS_SUCCESS,
                  createPayloadForH265(DEFAULT_MTU_SIZE_BYTES, (PBYTE) payload, payloadLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                       payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

        EXPECT_LT(0, payloadArray.payloadSubLenSize);

        offset = 0;

        for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
            EXPECT_EQ(STATUS_SUCCESS,
                      depayH265FromRtpPayload(payloadArray.payloadBuffer + offset, payloadArray.payloadSubLength[i], NULL, &newPayloadSubLen,
                                              &isStartPacket));
            newPayloadLen += newPayloadSubLen;
            if (isStartPacket) {
                newPayloadLen -= SIZEOF(start4ByteCode);
            }
            EXPECT_LT(0, newPayloadSubLen);
            offset += payloadArray.payloadSubLength[i];
        }
        EXPECT_LE(newPayloadLen, payloadLen);

        offset = 0;
        newPayloadLen = 0;
        isStartPacket = FALSE;
        pCurPtrInPayload = payload;
        remainPayloadLen = payloadLen;
        for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
            newPayloadSubLen = depayloadSize;
            EXPECT_EQ(STATUS_SUCCESS,
                      depayH265FromRtpPayload(payloadArray.payloadBuffer + offset, payloadArray.payloadSubLength[i], depayload, &newPayloadSubLen,
                                              &isStartPacket));
            if (isStartPacket) {
                EXPECT_EQ(STATUS_SUCCESS, getNextNaluLengthH265(pCurPtrInPayload, remainPayloadLen, &startIndex, &naluLength));
                pCurPtrInPayload += startIndex;
                startLen = SIZEOF(start4ByteCode);
            } else {
                startLen = 0;
            }
            EXPECT_TRUE(MEMCMP(pCurPtrInPayload, depayload + startLen, newPayloadSubLen - startLen) == 0);
            pCurPtrInPayload += newPayloadSubLen - startLen;
            remainPayloadLen -= newPayloadSubLen;
            offset += payloadArray.payloadSubLength[i];
        }
    }

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(payload);
    MEMFREE(depayload);
}

TEST_F(RtpFunctionalityTest, packingUnpackingVerifySameOpusFrame)
{
    BYTE payload[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    PBYTE depayload = (PBYTE) MEMALLOC(1500); // This is more than max mtu
    UINT32 depayloadSize = 1500;
    UINT32 payloadLen = 6;
    PayloadArray payloadArray;
    UINT32 newPayloadSubLen = 0;

    payloadArray.maxPayloadLength = 0;
    payloadArray.maxPayloadSubLenSize = 0;
    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    // First call for payload size and sub payload length size
    EXPECT_EQ(STATUS_SUCCESS,
              createPayloadForOpus(DEFAULT_MTU_SIZE_BYTES, (PBYTE) &payload, payloadLen, NULL, &payloadArray.payloadLength, NULL,
                                   &payloadArray.payloadSubLenSize));

    if (payloadArray.payloadLength > payloadArray.maxPayloadLength) {
        if (payloadArray.payloadBuffer != NULL) {
            MEMFREE(payloadArray.payloadBuffer);
        }
        payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
        payloadArray.maxPayloadLength = payloadArray.payloadLength;
    }
    if (payloadArray.payloadSubLenSize > payloadArray.maxPayloadSubLenSize) {
        if (payloadArray.payloadSubLength != NULL) {
            MEMFREE(payloadArray.payloadSubLength);
        }
        payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
        payloadArray.maxPayloadSubLenSize = payloadArray.payloadSubLenSize;
    }

    // Second call with actual buffer to fill in data
    EXPECT_EQ(STATUS_SUCCESS,
              createPayloadForOpus(DEFAULT_MTU_SIZE_BYTES, (PBYTE) &payload, payloadLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    EXPECT_EQ(1, payloadArray.payloadSubLenSize);
    EXPECT_EQ(6, payloadArray.payloadSubLength[0]);

    EXPECT_EQ(STATUS_SUCCESS, depayOpusFromRtpPayload(payloadArray.payloadBuffer, payloadArray.payloadSubLength[0], NULL, &newPayloadSubLen, NULL));
    EXPECT_EQ(6, newPayloadSubLen);

    newPayloadSubLen = depayloadSize;
    EXPECT_EQ(STATUS_SUCCESS,
              depayOpusFromRtpPayload(payloadArray.payloadBuffer, payloadArray.payloadSubLength[0], depayload, &newPayloadSubLen, NULL));
    EXPECT_TRUE(MEMCMP(payload, depayload, newPayloadSubLen) == 0);

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(depayload);
}

TEST_F(RtpFunctionalityTest, packingUnpackingVerifySameShortG711Frame)
{
    BYTE payload[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    PBYTE depayload = (PBYTE) MEMALLOC(1500); // This is more than max mtu
    UINT32 depayloadSize = 1500;
    UINT32 payloadLen = 6;
    PayloadArray payloadArray;
    UINT32 newPayloadSubLen = 0;

    payloadArray.maxPayloadLength = 0;
    payloadArray.maxPayloadSubLenSize = 0;
    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    // First call for payload size and sub payload length size
    EXPECT_EQ(STATUS_SUCCESS,
              createPayloadForG711(DEFAULT_MTU_SIZE_BYTES, (PBYTE) &payload, payloadLen, NULL, &payloadArray.payloadLength, NULL,
                                   &payloadArray.payloadSubLenSize));

    if (payloadArray.payloadLength > payloadArray.maxPayloadLength) {
        if (payloadArray.payloadBuffer != NULL) {
            MEMFREE(payloadArray.payloadBuffer);
        }
        payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
        payloadArray.maxPayloadLength = payloadArray.payloadLength;
    }
    if (payloadArray.payloadSubLenSize > payloadArray.maxPayloadSubLenSize) {
        if (payloadArray.payloadSubLength != NULL) {
            MEMFREE(payloadArray.payloadSubLength);
        }
        payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
        payloadArray.maxPayloadSubLenSize = payloadArray.payloadSubLenSize;
    }

    // Second call with actual buffer to fill in data
    EXPECT_EQ(STATUS_SUCCESS,
              createPayloadForG711(DEFAULT_MTU_SIZE_BYTES, (PBYTE) &payload, payloadLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    EXPECT_EQ(1, payloadArray.payloadSubLenSize);
    EXPECT_EQ(6, payloadArray.payloadSubLength[0]);

    EXPECT_EQ(STATUS_SUCCESS, depayG711FromRtpPayload(payloadArray.payloadBuffer, payloadArray.payloadSubLength[0], NULL, &newPayloadSubLen, NULL));
    EXPECT_EQ(6, newPayloadSubLen);

    newPayloadSubLen = depayloadSize;
    EXPECT_EQ(STATUS_SUCCESS,
              depayG711FromRtpPayload(payloadArray.payloadBuffer, payloadArray.payloadSubLength[0], depayload, &newPayloadSubLen, NULL));
    EXPECT_TRUE(MEMCMP(payload, depayload, newPayloadSubLen) == 0);

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(depayload);
}

TEST_F(RtpFunctionalityTest, packingUnpackingVerifySameLongG711Frame)
{
    BYTE payload[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    PBYTE depayload = (PBYTE) MEMALLOC(1500); // This is more than max mtu
    UINT32 depayloadSize = 1500;
    UINT32 payloadLen = 10;
    PayloadArray payloadArray;
    UINT32 i = 0;
    UINT32 newPayloadSubLen = 0;
    UINT32 newPayloadLen = 0;
    UINT32 offset = 0;
    PBYTE pCurPtrInPayload;

    payloadArray.maxPayloadLength = 0;
    payloadArray.maxPayloadSubLenSize = 0;
    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    // First call for payload size and sub payload length size
    EXPECT_EQ(STATUS_SUCCESS,
              createPayloadForG711(4, (PBYTE) &payload, payloadLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    if (payloadArray.payloadLength > payloadArray.maxPayloadLength) {
        if (payloadArray.payloadBuffer != NULL) {
            MEMFREE(payloadArray.payloadBuffer);
        }
        payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
        payloadArray.maxPayloadLength = payloadArray.payloadLength;
    }
    if (payloadArray.payloadSubLenSize > payloadArray.maxPayloadSubLenSize) {
        if (payloadArray.payloadSubLength != NULL) {
            MEMFREE(payloadArray.payloadSubLength);
        }
        payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
        payloadArray.maxPayloadSubLenSize = payloadArray.payloadSubLenSize;
    }

    // Second call with actual buffer to fill in data
    EXPECT_EQ(STATUS_SUCCESS,
              createPayloadForG711(4, (PBYTE) &payload, payloadLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    EXPECT_EQ(3, payloadArray.payloadSubLenSize);
    EXPECT_EQ(4, payloadArray.payloadSubLength[0]);
    EXPECT_EQ(4, payloadArray.payloadSubLength[1]);
    EXPECT_EQ(2, payloadArray.payloadSubLength[2]);

    offset = 0;

    for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
        EXPECT_EQ(STATUS_SUCCESS,
                  depayG711FromRtpPayload(payloadArray.payloadBuffer + offset, payloadArray.payloadSubLength[i], NULL, &newPayloadSubLen, NULL));
        newPayloadLen += newPayloadSubLen;
        EXPECT_LT(0, newPayloadSubLen);
        offset += payloadArray.payloadSubLength[i];
    }
    EXPECT_EQ(newPayloadLen, payloadLen);

    offset = 0;
    pCurPtrInPayload = payload;
    for (i = 0; i < payloadArray.payloadSubLenSize; i++) {
        newPayloadSubLen = depayloadSize;
        EXPECT_EQ(STATUS_SUCCESS,
                  depayG711FromRtpPayload(payloadArray.payloadBuffer + offset, payloadArray.payloadSubLength[i], depayload, &newPayloadSubLen, NULL));
        EXPECT_TRUE(MEMCMP(pCurPtrInPayload, depayload, newPayloadSubLen) == 0);
        pCurPtrInPayload += newPayloadSubLen;
        offset += payloadArray.payloadSubLength[i];
    }

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(depayload);
}

TEST_F(RtpFunctionalityTest, invalidNaluParse)
{
    BYTE data[] = {0x01, 0x00, 0x02};
    BYTE data1[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02};
    UINT32 startIndex = 0, naluLength = 0;
    EXPECT_EQ(STATUS_RTP_INVALID_NALU, getNextNaluLength(data, 3, &startIndex, &naluLength));
    EXPECT_EQ(STATUS_RTP_INVALID_NALU, getNextNaluLength(data1, 7, &startIndex, &naluLength));

    EXPECT_EQ(STATUS_RTP_INVALID_NALU, getNextNaluLengthH265(data, 3, &startIndex, &naluLength));
    EXPECT_EQ(STATUS_RTP_INVALID_NALU, getNextNaluLengthH265(data1, 7, &startIndex, &naluLength));
}

TEST_F(RtpFunctionalityTest, validNaluParse)
{
    BYTE data[] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x02};
    UINT32 startIndex = 0, naluLength = 0;
    
    EXPECT_EQ(STATUS_SUCCESS, getNextNaluLength(data, 6, &startIndex, &naluLength));
    EXPECT_EQ(4, startIndex);
    EXPECT_EQ(2, naluLength);

    startIndex = 0;
    naluLength = 0;

    EXPECT_EQ(STATUS_SUCCESS, getNextNaluLengthH265(data, 6, &startIndex, &naluLength));
    EXPECT_EQ(4, startIndex);
    EXPECT_EQ(2, naluLength);
}

TEST_F(RtpFunctionalityTest, validMultipleNaluParse)
{
    BYTE nalus[] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x0b, 0x0c};
    UINT32 startIndex = 0, naluLength = 0;
    UINT32 nalusLength = 13;
    EXPECT_EQ(STATUS_SUCCESS, getNextNaluLength(nalus, nalusLength, &startIndex, &naluLength));
    EXPECT_EQ(4, startIndex);
    EXPECT_EQ(2, naluLength);
    EXPECT_EQ(STATUS_SUCCESS, getNextNaluLength(&nalus[startIndex + naluLength], nalusLength - startIndex - naluLength, &startIndex, &naluLength));
    EXPECT_EQ(4, startIndex);
    EXPECT_EQ(3, naluLength);
}

TEST_F(RtpFunctionalityTest, trailingZerosWouldBeReturned)
{
    BYTE nalus[] = {0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    UINT32 startIndex = 0, naluLength = 0;
    UINT32 nalusLength = 11;
    EXPECT_EQ(STATUS_SUCCESS, getNextNaluLength(nalus, nalusLength, &startIndex, &naluLength));
    EXPECT_EQ(4, startIndex);
    EXPECT_EQ(7, naluLength);
}

// https://tools.ietf.org/html/rfc3550#section-5.3.1
TEST_F(RtpFunctionalityTest, createPacketWithExtension)
{
    BYTE payload[10] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
    BYTE extpayload[8] = {0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49};
    BYTE rawbytes[1024] = {0};
    PBYTE ptr = reinterpret_cast<PBYTE>(&rawbytes);
    PRtpPacket pRtpPacket = NULL;
    PRtpPacket pRtpPacket2 = NULL;

    EXPECT_EQ(STATUS_SUCCESS,
              createRtpPacket(2, FALSE, TRUE, 0, FALSE, 96, 42, 100, 0x1234ABCD, NULL, 0x4243, 8, extpayload, payload, 10, &pRtpPacket));
    EXPECT_EQ(STATUS_SUCCESS, setBytesFromRtpPacket(pRtpPacket, ptr, 1024));

    auto len = RTP_GET_RAW_PACKET_SIZE(pRtpPacket);
    EXPECT_EQ(STATUS_SUCCESS, createRtpPacketFromBytes(ptr, len, &pRtpPacket2));
    EXPECT_TRUE(pRtpPacket2->header.extension);
    EXPECT_EQ(0x4243, pRtpPacket2->header.extensionProfile);
    EXPECT_EQ(0x44, pRtpPacket2->header.extensionPayload[2]);
    EXPECT_EQ(0x15, pRtpPacket2->payload[5]);
    freeRtpPacket(&pRtpPacket);
    pRtpPacket2->pRawPacket = NULL;
    freeRtpPacket(&pRtpPacket2);
}

TEST_F(RtpFunctionalityTest, twccPayload)
{
    UINT32 extpayload = TWCC_PAYLOAD(4u, 420u);
    auto ptr = reinterpret_cast<PBYTE>(&extpayload);
    auto seqNum = TWCC_SEQNUM(ptr);
    EXPECT_EQ(4, (ptr[0] >> 4));
    EXPECT_EQ(1, (ptr[0] & 0xfu));
    EXPECT_EQ(420, seqNum);
    EXPECT_EQ(0, ptr[3]);
}

// STAP-A Tests
// STAP-A format: [1 byte header: type=24|NRI] [2 byte size][NAL1] [2 byte size][NAL2] ...
#define STAP_A_INDICATOR 24
#define STAP_A_HEADER_SIZE 1

TEST_F(RtpFunctionalityTest, stapAPacketCreation)
{
    // Create frame with two small NAL units that should be aggregated
    // Frame format: [start code][NAL1][start code][NAL2]
    // NAL1: 5 bytes (type=1, non-IDR slice, NRI=0x60)
    // NAL2: 4 bytes (type=5, IDR slice, NRI=0x60)
    BYTE frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x61, 0x01, 0x02, 0x03, 0x04,  // NAL1: 5 bytes (0x61 = NRI 0x60 | type 1)
        0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x12, 0x13         // NAL2: 4 bytes (0x65 = NRI 0x60 | type 5)
    };
    UINT32 frameLen = sizeof(frame);
    PayloadArray payloadArray;
    UINT32 mtu = 1200; // Large enough to fit both NALs in one STAP-A

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    // First call to get sizes
    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    // With STAP-A: 1 (header) + 2 (size1) + 5 (NAL1) + 2 (size2) + 4 (NAL2) = 14 bytes, 1 packet
    EXPECT_EQ(14, payloadArray.payloadLength);
    EXPECT_EQ(1, payloadArray.payloadSubLenSize);

    // Allocate and fill
    payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
    payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    // Verify STAP-A structure
    PBYTE payload = payloadArray.payloadBuffer;

    // Check header: type=24 (STAP-A) | NRI=0x60 (max of both NALs)
    EXPECT_EQ(STAP_A_INDICATOR | 0x60, payload[0]);

    // Check first NAL size (big-endian)
    UINT16 nal1Size = (payload[1] << 8) | payload[2];
    EXPECT_EQ(5, nal1Size);

    // Check first NAL data starts at offset 3
    EXPECT_EQ(0x61, payload[3]); // NAL1 header

    // Check second NAL size (at offset 3 + 5 = 8)
    UINT16 nal2Size = (payload[8] << 8) | payload[9];
    EXPECT_EQ(4, nal2Size);

    // Check second NAL data
    EXPECT_EQ(0x65, payload[10]); // NAL2 header

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
}

TEST_F(RtpFunctionalityTest, stapADepayloadRoundTrip)
{
    // Create frame with multiple small NAL units
    // Using convention: 4-byte start code for first NAL, 3-byte for subsequent NALs in same frame
    BYTE frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB,        // NAL1: 3 bytes (SPS, NRI=0x60) - 4-byte start
        0x00, 0x00, 0x01, 0x68, 0xCC, 0xDD, 0xEE,        // NAL2: 4 bytes (PPS, NRI=0x60) - 3-byte start
        0x00, 0x00, 0x01, 0x06, 0x11, 0x22               // NAL3: 3 bytes (SEI, NRI=0x00) - 3-byte start
    };
    UINT32 frameLen = sizeof(frame);
    PayloadArray payloadArray;
    UINT32 mtu = 1200;

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    // Get sizes
    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    // Should be aggregated: 1 + (2+3) + (2+4) + (2+3) = 17 bytes, 1 packet
    EXPECT_EQ(17, payloadArray.payloadLength);
    EXPECT_EQ(1, payloadArray.payloadSubLenSize);

    payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
    payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    // Verify it's a STAP-A packet
    EXPECT_EQ(STAP_A_INDICATOR, payloadArray.payloadBuffer[0] & 0x1F);

    // Depayload and verify we get back the original NALs
    PBYTE depayload = (PBYTE) MEMALLOC(frameLen + 100);
    UINT32 depayloadLen = frameLen + 100;
    BOOL isStart = FALSE;

    EXPECT_EQ(STATUS_SUCCESS, depayH264FromRtpPayload(payloadArray.payloadBuffer, payloadArray.payloadSubLength[0],
                                                      depayload, &depayloadLen, &isStart));
    EXPECT_TRUE(isStart);

    // Depayloaded data should match original frame
    // First NAL gets 4-byte start code, subsequent NALs get 3-byte (same-frame convention)
    EXPECT_EQ(frameLen, depayloadLen);
    EXPECT_EQ(0, MEMCMP(frame, depayload, frameLen));

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(depayload);
}

TEST_F(RtpFunctionalityTest, stapAMixedWithFuA)
{
    // Create frame with small NALs followed by a large NAL
    // Small NALs should be aggregated, large NAL should use FU-A
    BYTE smallNals[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x01, 0x02,              // NAL1: 3 bytes (SPS)
        0x00, 0x00, 0x00, 0x01, 0x68, 0x03, 0x04               // NAL2: 3 bytes (PPS)
    };

    // Create a large NAL that exceeds MTU
    UINT32 largeNalSize = 2000;
    UINT32 totalFrameLen = sizeof(smallNals) + 4 + largeNalSize; // small NALs + start code + large NAL
    PBYTE frame = (PBYTE) MEMALLOC(totalFrameLen);

    MEMCPY(frame, smallNals, sizeof(smallNals));
    // Add start code for large NAL
    frame[sizeof(smallNals)] = 0x00;
    frame[sizeof(smallNals) + 1] = 0x00;
    frame[sizeof(smallNals) + 2] = 0x00;
    frame[sizeof(smallNals) + 3] = 0x01;
    // Large NAL header (type=1, non-IDR slice, NRI=0x60)
    frame[sizeof(smallNals) + 4] = 0x61;
    // Fill rest with data
    MEMSET(frame + sizeof(smallNals) + 5, 0xAA, largeNalSize - 1);

    PayloadArray payloadArray;
    UINT32 mtu = 1200;

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, totalFrameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    // Should have: 1 STAP-A packet for small NALs + multiple FU-A packets for large NAL
    // STAP-A: 1 + (2+3) + (2+3) = 11 bytes
    // FU-A: ceil((2000-1) / (1200-2)) = ceil(1999/1198) = 2 packets
    EXPECT_GT(payloadArray.payloadSubLenSize, 1); // More than one packet

    payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
    payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, totalFrameLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    // First packet should be STAP-A
    EXPECT_EQ(STAP_A_INDICATOR, payloadArray.payloadBuffer[0] & 0x1F);

    // Second packet should be FU-A (type 28)
    UINT32 offset = payloadArray.payloadSubLength[0];
    EXPECT_EQ(28, payloadArray.payloadBuffer[offset] & 0x1F);

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
    MEMFREE(frame);
}

TEST_F(RtpFunctionalityTest, stapASingleNalNoAggregation)
{
    // Single NAL should NOT create STAP-A, should use single NAL packet
    BYTE frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x61, 0x01, 0x02, 0x03, 0x04  // Single NAL: 5 bytes
    };
    UINT32 frameLen = sizeof(frame);
    PayloadArray payloadArray;
    UINT32 mtu = 1200;

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    // Single NAL packet: just the NAL data (5 bytes), 1 packet
    EXPECT_EQ(5, payloadArray.payloadLength);
    EXPECT_EQ(1, payloadArray.payloadSubLenSize);

    payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
    payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    // Should be single NAL packet (type 1, not STAP-A type 24)
    EXPECT_EQ(1, payloadArray.payloadBuffer[0] & 0x1F);
    EXPECT_EQ(0x61, payloadArray.payloadBuffer[0]); // Original NAL header

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
}

TEST_F(RtpFunctionalityTest, stapAMaxMtuBoundary)
{
    // Test NALs that exactly fill MTU when aggregated
    // MTU = 20, STAP-A header = 1, per-NAL overhead = 2
    // So: 1 + (2+NAL1) + (2+NAL2) = 20 => NAL1 + NAL2 = 15
    BYTE frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x61, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,  // NAL1: 8 bytes
        0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16         // NAL2: 7 bytes
    };
    UINT32 frameLen = sizeof(frame);
    PayloadArray payloadArray;
    UINT32 mtu = 20; // Exactly fits: 1 + (2+8) + (2+7) = 20

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    // Should fit in one STAP-A
    EXPECT_EQ(20, payloadArray.payloadLength);
    EXPECT_EQ(1, payloadArray.payloadSubLenSize);

    payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
    payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    EXPECT_EQ(STAP_A_INDICATOR, payloadArray.payloadBuffer[0] & 0x1F);

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
}

TEST_F(RtpFunctionalityTest, stapAExceedsMtuSplitsToMultiplePackets)
{
    // Test NALs that exceed MTU - should create separate packets
    BYTE frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x61, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,  // NAL1: 8 bytes
        0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17   // NAL2: 8 bytes
    };
    UINT32 frameLen = sizeof(frame);
    PayloadArray payloadArray;
    UINT32 mtu = 15; // Too small: 1 + (2+8) + (2+8) = 21 > 15

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    // Each NAL should be in its own packet (single NAL packets, not STAP-A)
    // NAL1: 8 bytes, NAL2: 8 bytes = 16 total, 2 packets
    EXPECT_EQ(16, payloadArray.payloadLength);
    EXPECT_EQ(2, payloadArray.payloadSubLenSize);

    payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
    payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));

    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                                   payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    // Both should be single NAL packets (not STAP-A)
    EXPECT_EQ(1, payloadArray.payloadBuffer[0] & 0x1F); // NAL type 1
    EXPECT_EQ(5, payloadArray.payloadBuffer[payloadArray.payloadSubLength[0]] & 0x1F); // NAL type 5

    MEMFREE(payloadArray.payloadBuffer);
    MEMFREE(payloadArray.payloadSubLength);
}

TEST_F(RtpFunctionalityTest, exceedMaxNalusPerFrameReturnsError)
{
    // Create a frame with more than MAX_NALUS_PER_FRAME (64) NAL units
    // Each NAL: 4-byte start code + 2 bytes data = 6 bytes
    // Total: 65 NALs * 6 bytes = 390 bytes
    const UINT32 NUM_NALUS = 65;
    const UINT32 NAL_SIZE = 2;
    const UINT32 START_CODE_SIZE = 4;
    BYTE frame[NUM_NALUS * (START_CODE_SIZE + NAL_SIZE)];

    // Fill frame with 65 small NAL units
    PBYTE ptr = frame;
    for (UINT32 i = 0; i < NUM_NALUS; i++) {
        // 4-byte start code
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0x01;
        // NAL data: type 1 (non-IDR slice) with NRI
        *ptr++ = 0x61;  // NRI=0x60 | type=1
        *ptr++ = (BYTE) i;  // payload byte
    }

    UINT32 frameLen = NUM_NALUS * (START_CODE_SIZE + NAL_SIZE);
    PayloadArray payloadArray;
    UINT32 mtu = 1200;

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    // Should fail with STATUS_INVALID_ARG because we have more than 64 NALs
    EXPECT_EQ(STATUS_INVALID_ARG, createPayloadForH264(mtu, frame, frameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));
}

TEST_F(RtpFunctionalityTest, exactlyMaxNalusPerFrameSucceeds)
{
    // Create a frame with exactly MAX_NALUS_PER_FRAME (64) NAL units - should succeed
    const UINT32 NUM_NALUS = 64;
    const UINT32 NAL_SIZE = 2;
    const UINT32 START_CODE_SIZE = 4;
    BYTE frame[NUM_NALUS * (START_CODE_SIZE + NAL_SIZE)];

    // Fill frame with 64 small NAL units
    PBYTE ptr = frame;
    for (UINT32 i = 0; i < NUM_NALUS; i++) {
        // 4-byte start code
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0x01;
        // NAL data: type 1 (non-IDR slice) with NRI
        *ptr++ = 0x61;  // NRI=0x60 | type=1
        *ptr++ = (BYTE) i;  // payload byte
    }

    UINT32 frameLen = NUM_NALUS * (START_CODE_SIZE + NAL_SIZE);
    PayloadArray payloadArray;
    UINT32 mtu = 1200;

    payloadArray.payloadBuffer = NULL;
    payloadArray.payloadSubLength = NULL;

    // Should succeed with exactly 64 NALs
    EXPECT_EQ(STATUS_SUCCESS, createPayloadForH264(mtu, frame, frameLen, NULL, &payloadArray.payloadLength, NULL, &payloadArray.payloadSubLenSize));

    // All 64 small NALs should fit into STAP-A packets
    // 64 NALs * (2 bytes size + 2 bytes data) = 256 bytes + headers
    EXPECT_GT(payloadArray.payloadLength, 0);
    EXPECT_GT(payloadArray.payloadSubLenSize, 0);
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
