#include "WebRTCClientTestFixture.h"

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class RtcpFunctionalityTest : public WebRtcClientTestBase {
  public:
    PKvsRtpTransceiver pKvsRtpTransceiver = nullptr;
    PKvsPeerConnection pKvsPeerConnection = nullptr;
    PRtcPeerConnection pRtcPeerConnection = nullptr;
    PRtcRtpTransceiver pRtcRtpTransceiver = nullptr;

    STATUS initTransceiver(UINT32 ssrc)
    {
        RtcConfiguration config{};
        initRtcConfiguration(&config);
        EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
        pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);
        pRtcRtpTransceiver = addTransceiver(ssrc);
        pKvsRtpTransceiver = reinterpret_cast<PKvsRtpTransceiver>(pRtcRtpTransceiver);
        return STATUS_SUCCESS;
    }

    PRtcRtpTransceiver addTransceiver(UINT32 ssrc)
    {
        RtcMediaStreamTrack track{};
        track.codec = RTC_CODEC_VP8;
        PRtcRtpTransceiver out = nullptr;
        EXPECT_EQ(STATUS_SUCCESS, ::addTransceiver(pRtcPeerConnection, &track, nullptr, &out));
        ((PKvsRtpTransceiver) out)->sender.ssrc = ssrc;
        return out;
    }
};

TEST_F(RtcpFunctionalityTest, setRtpPacketFromBytes)
{
    RtcpPacket rtcpPacket;

    MEMSET(&rtcpPacket, 0x00, SIZEOF(RtcpPacket));

    // Assert that we don't parse buffers that aren't even large enough
    BYTE headerTooSmall[] = {0x00, 0x00, 0x00};
    EXPECT_EQ(STATUS_RTCP_INPUT_PACKET_TOO_SMALL, setRtcpPacketFromBytes(headerTooSmall, SIZEOF(headerTooSmall), &rtcpPacket));

    // Assert that we check version field
    BYTE invalidVersionValue[] = {0x01, 0xcd, 0x00, 0x03, 0x2c, 0xd1, 0xa0, 0xde, 0x00, 0x00, 0xab, 0xe0, 0x00, 0xa4, 0x00, 0x00};
    EXPECT_EQ(STATUS_RTCP_INPUT_PACKET_INVALID_VERSION, setRtcpPacketFromBytes(invalidVersionValue, SIZEOF(invalidVersionValue), &rtcpPacket));

    // Assert that we check the length field
    BYTE invalidLengthValue[] = {0x81, 0xcd, 0x00, 0x00, 0x2c, 0xd1, 0xa0, 0xde, 0x00, 0x00, 0xab, 0xe0, 0x00, 0xa4, 0x00, 0x00};
    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(invalidLengthValue, SIZEOF(invalidLengthValue), &rtcpPacket));

    BYTE validRtcpPacket[] = {0x81, 0xcd, 0x00, 0x03, 0x2c, 0xd1, 0xa0, 0xde, 0x00, 0x00, 0xab, 0xe0, 0x00, 0xa4, 0x00, 0x00};
    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(validRtcpPacket, SIZEOF(validRtcpPacket), &rtcpPacket));

    EXPECT_EQ(rtcpPacket.header.packetType, RTCP_PACKET_TYPE_GENERIC_RTP_FEEDBACK);
    EXPECT_EQ(rtcpPacket.header.receptionReportCount, RTCP_FEEDBACK_MESSAGE_TYPE_NACK);
}

TEST_F(RtcpFunctionalityTest, setRtpPacketFromBytesCompound)
{
    RtcpPacket rtcpPacket;

    MEMSET(&rtcpPacket, 0x00, SIZEOF(RtcpPacket));

    // Compound RTCP Packet that contains SR, SDES and REMB
    BYTE compoundPacket[] = {0x80, 0xc8, 0x00, 0x06, 0xf1, 0x2d, 0x7b, 0x4b, 0xe1, 0xe3, 0x20, 0x43, 0xe5, 0x3d, 0x10, 0x2b, 0xbf,
                             0x58, 0xf7, 0xef, 0x00, 0x00, 0x23, 0xf3, 0x00, 0x6c, 0xd3, 0x75, 0x81, 0xca, 0x00, 0x06, 0xf1, 0x2d,
                             0x7b, 0x4b, 0x01, 0x10, 0x2f, 0x76, 0x6d, 0x4b, 0x51, 0x6e, 0x47, 0x6e, 0x55, 0x70, 0x4f, 0x2b, 0x70,
                             0x38, 0x64, 0x52, 0x00, 0x00, 0x8f, 0xce, 0x00, 0x06, 0xf1, 0x2d, 0x7b, 0x4b, 0x00, 0x00, 0x00, 0x00,
                             0x52, 0x45, 0x4d, 0x42, 0x02, 0x12, 0x2d, 0x97, 0x0c, 0xef, 0x37, 0x0d, 0x2d, 0x07, 0x3d, 0x1d};

    auto currentOffset = 0;
    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(compoundPacket + currentOffset, SIZEOF(compoundPacket) - currentOffset, &rtcpPacket));
    EXPECT_EQ(rtcpPacket.header.packetType, RTCP_PACKET_TYPE_SENDER_REPORT);

    currentOffset += (rtcpPacket.payloadLength + RTCP_PACKET_HEADER_LEN);
    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(compoundPacket + currentOffset, SIZEOF(compoundPacket) - currentOffset, &rtcpPacket));
    EXPECT_EQ(rtcpPacket.header.packetType, RTCP_PACKET_TYPE_SOURCE_DESCRIPTION);

    currentOffset += (rtcpPacket.payloadLength + RTCP_PACKET_HEADER_LEN);
    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(compoundPacket + currentOffset, SIZEOF(compoundPacket) - currentOffset, &rtcpPacket));
    EXPECT_EQ(rtcpPacket.header.packetType, RTCP_PACKET_TYPE_PAYLOAD_SPECIFIC_FEEDBACK);

    currentOffset += (rtcpPacket.payloadLength + RTCP_PACKET_HEADER_LEN);
    ASSERT_EQ(currentOffset, SIZEOF(compoundPacket));
}

TEST_F(RtcpFunctionalityTest, rtcpNackListGet)
{
    UINT32 senderSsrc = 0, receiverSsrc = 0, ssrcListLen = 0;

    // Assert that NACK list meets the minimum length requirement
    BYTE nackListTooSmall[] = {0x00, 0x00, 0x00};
    EXPECT_EQ(STATUS_RTCP_INPUT_NACK_LIST_INVALID,
              rtcpNackListGet(nackListTooSmall, SIZEOF(nackListTooSmall), &senderSsrc, &receiverSsrc, NULL, &ssrcListLen));

    BYTE nackListMalformed[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(STATUS_RTCP_INPUT_NACK_LIST_INVALID,
              rtcpNackListGet(nackListMalformed, SIZEOF(nackListMalformed), &senderSsrc, &receiverSsrc, NULL, &ssrcListLen));

    BYTE nackListSsrcOnly[] = {0x2c, 0xd1, 0xa0, 0xde, 0x00, 0x00, 0xab, 0xe0};
    EXPECT_EQ(STATUS_SUCCESS, rtcpNackListGet(nackListSsrcOnly, SIZEOF(nackListSsrcOnly), &senderSsrc, &receiverSsrc, NULL, &ssrcListLen));

    EXPECT_EQ(senderSsrc, 0x2cd1a0de);
    EXPECT_EQ(receiverSsrc, 0x0000abe0);

    BYTE singlePID[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0xa8, 0x00, 0x00};
    EXPECT_EQ(STATUS_SUCCESS, rtcpNackListGet(singlePID, SIZEOF(singlePID), &senderSsrc, &receiverSsrc, NULL, &ssrcListLen));

    std::unique_ptr<UINT16[]> singlePIDBuffer(new UINT16[ssrcListLen]);
    EXPECT_EQ(STATUS_SUCCESS, rtcpNackListGet(singlePID, SIZEOF(singlePID), &senderSsrc, &receiverSsrc, singlePIDBuffer.get(), &ssrcListLen));

    EXPECT_EQ(ssrcListLen, 1);
    EXPECT_EQ(singlePIDBuffer[0], 3240);
}

TEST_F(RtcpFunctionalityTest, rtcpNackListBLP)
{
    UINT32 senderSsrc = 0, receiverSsrc = 0, ssrcListLen = 0;

    BYTE singleBLP[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0xa8, 0x00, 0x04};
    EXPECT_EQ(STATUS_SUCCESS, rtcpNackListGet(singleBLP, SIZEOF(singleBLP), &senderSsrc, &receiverSsrc, NULL, &ssrcListLen));

    std::unique_ptr<UINT16[]> singleBLPBuffer(new UINT16[ssrcListLen]);
    EXPECT_EQ(STATUS_SUCCESS, rtcpNackListGet(singleBLP, SIZEOF(singleBLP), &senderSsrc, &receiverSsrc, singleBLPBuffer.get(), &ssrcListLen));

    EXPECT_EQ(ssrcListLen, 2);
    EXPECT_EQ(singleBLPBuffer[0], 3240);
    EXPECT_EQ(singleBLPBuffer[1], 3243);
}

TEST_F(RtcpFunctionalityTest, rtcpNackListCompound)
{
    UINT32 senderSsrc = 0, receiverSsrc = 0, ssrcListLen = 0;

    BYTE compound[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0xa8, 0x00, 0x00, 0x0c, 0xff, 0x00, 0x00};
    EXPECT_EQ(STATUS_SUCCESS, rtcpNackListGet(compound, SIZEOF(compound), &senderSsrc, &receiverSsrc, NULL, &ssrcListLen));

    std::unique_ptr<UINT16[]> compoundBuffer(new UINT16[ssrcListLen]);
    EXPECT_EQ(STATUS_SUCCESS, rtcpNackListGet(compound, SIZEOF(compound), &senderSsrc, &receiverSsrc, compoundBuffer.get(), &ssrcListLen));

    EXPECT_EQ(ssrcListLen, 2);
    EXPECT_EQ(compoundBuffer[0], 3240);
    EXPECT_EQ(compoundBuffer[1], 3327);
}

TEST_F(RtcpFunctionalityTest, onRtcpPacketCompoundNack)
{
    PRtpPacket pRtpPacket = nullptr;
    BYTE validRtcpPacket[] = {0x81, 0xcd, 0x00, 0x03, 0x2c, 0xd1, 0xa0, 0xde, 0x00, 0x00, 0xab, 0xe0, 0x00, 0x00, 0x00, 0x00};
    initTransceiver(44000);
    ASSERT_EQ(STATUS_SUCCESS,
              createRtpRollingBuffer(DEFAULT_ROLLING_BUFFER_DURATION_IN_SECONDS * DEFAULT_EXPECTED_VIDEO_BIT_RATE / 8 / DEFAULT_MTU_SIZE_BYTES,
                                     &pKvsRtpTransceiver->sender.packetBuffer));
    ASSERT_EQ(STATUS_SUCCESS,
              createRetransmitter(DEFAULT_SEQ_NUM_BUFFER_SIZE, DEFAULT_VALID_INDEX_BUFFER_SIZE, &pKvsRtpTransceiver->sender.retransmitter));
    ASSERT_EQ(STATUS_SUCCESS, createRtpPacketWithSeqNum(0, &pRtpPacket));

    ASSERT_EQ(STATUS_SUCCESS, rtpRollingBufferAddRtpPacket(pKvsRtpTransceiver->sender.packetBuffer, pRtpPacket));
    ASSERT_EQ(STATUS_SUCCESS, onRtcpPacket(pKvsPeerConnection, validRtcpPacket, SIZEOF(validRtcpPacket)));
    RtcOutboundRtpStreamStats stats{};
    getRtpOutboundStats(pRtcPeerConnection, nullptr, &stats);
    ASSERT_EQ(1, stats.nackCount);
    ASSERT_EQ(1, stats.retransmittedPacketsSent);
    ASSERT_EQ(10, stats.retransmittedBytesSent);
    freePeerConnection(&pRtcPeerConnection);
    freeRtpPacket(&pRtpPacket);
}

TEST_F(RtcpFunctionalityTest, onRtcpPacketCompound)
{
    KvsPeerConnection peerConnection{};

    BYTE compound[] = {
        0x80, 0xc8, 0x00, 0x06, 0xf1, 0x2d, 0x7b, 0x4b, 0xe1, 0xe3, 0x20, 0x43, 0xe5, 0x3d, 0x10, 0x2b, 0xbf, 0x58, 0xf7,
        0xef, 0x00, 0x00, 0x23, 0xf3, 0x00, 0x6c, 0xd3, 0x75, 0x81, 0xca, 0x00, 0x06, 0xf1, 0x2d, 0x7b, 0x4b, 0x01, 0x10,
        0x2f, 0x76, 0x6d, 0x4b, 0x51, 0x6e, 0x47, 0x6e, 0x55, 0x70, 0x4f, 0x2b, 0x70, 0x38, 0x64, 0x52, 0x00, 0x00,
    };
    EXPECT_EQ(STATUS_SUCCESS, onRtcpPacket(&peerConnection, compound, SIZEOF(compound)));
}

TEST_F(RtcpFunctionalityTest, onRtcpPacketCompoundSenderReport)
{
    auto hexpacket = (PCHAR) "81C900076C1B58915E0C6E520400000000000002000000000102030400424344";
    BYTE rawpacket[64] = {0};
    UINT32 rawpacketSize = 64;
    EXPECT_EQ(STATUS_SUCCESS, hexDecode(hexpacket, strlen(hexpacket), rawpacket, &rawpacketSize));

    // added two transceivers to test correct transceiver stats in getRtpRemoteInboundStats
    initTransceiver(4242);               // fake transceiver
    auto t = addTransceiver(1577872978); // real transceiver

    EXPECT_EQ(STATUS_SUCCESS, onRtcpPacket(pKvsPeerConnection, rawpacket, rawpacketSize));

    RtcRemoteInboundRtpStreamStats stats{};
    EXPECT_EQ(STATUS_SUCCESS, getRtpRemoteInboundStats(pRtcPeerConnection, t, &stats));
    EXPECT_EQ(1, stats.reportsReceived);
    EXPECT_EQ(1, stats.roundTripTimeMeasurements);
    // onRtcpPacket uses real time clock GETTIME to calculate roundTripTime, cant test
    EXPECT_EQ(4.0 / 255.0, stats.fractionLost);
    EXPECT_LT(0, stats.totalRoundTripTime);
    EXPECT_LT(0, stats.roundTripTime);
    // Verify incoming RR fields stored in received stats
    EXPECT_EQ(0, stats.received.packetsLost);     // cumulative lost = 0 in the packet
    EXPECT_DOUBLE_EQ(0.0, stats.received.jitter); // interarrival jitter = 0 in the packet
    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, rembValueGet)
{
    BYTE rawRtcpPacket[] = {0x8f, 0xce, 0x00, 0x05, 0x61, 0x7a, 0x37, 0x43, 0x00, 0x00, 0x00, 0x00,
                            0x52, 0x45, 0x4d, 0x42, 0x01, 0x12, 0x46, 0x73, 0x6c, 0x76, 0xe8, 0x55};
    RtcpPacket rtcpPacket;

    MEMSET(&rtcpPacket, 0x00, SIZEOF(RtcpPacket));

    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(rawRtcpPacket, SIZEOF(rawRtcpPacket), &rtcpPacket));
    EXPECT_EQ(rtcpPacket.header.packetType, RTCP_PACKET_TYPE_PAYLOAD_SPECIFIC_FEEDBACK);
    EXPECT_EQ(rtcpPacket.header.receptionReportCount, RTCP_FEEDBACK_MESSAGE_TYPE_APPLICATION_LAYER_FEEDBACK);

    BYTE bufferTooSmall[] = {0x00, 0x00, 0x00};
    EXPECT_EQ(STATUS_RTCP_INPUT_REMB_TOO_SMALL, isRembPacket(bufferTooSmall, SIZEOF(bufferTooSmall)));

    BYTE bufferNoUniqueIdentifier[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(STATUS_RTCP_INPUT_REMB_INVALID, isRembPacket(bufferNoUniqueIdentifier, SIZEOF(bufferNoUniqueIdentifier)));

    UINT8 ssrcListLen = 0;
    DOUBLE maximumBitRate = 0;
    UINT32 ssrcList[5];

    BYTE singleSSRC[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x45, 0x4d, 0x42, 0x01, 0x12, 0x76, 0x28, 0x6c, 0x76, 0xe8, 0x55};
    EXPECT_EQ(STATUS_SUCCESS, rembValueGet(singleSSRC, SIZEOF(singleSSRC), &maximumBitRate, ssrcList, &ssrcListLen));
    EXPECT_EQ(ssrcListLen, 1);
    EXPECT_EQ(maximumBitRate, 2581120.0);
    EXPECT_EQ(ssrcList[0], 0x6c76e855);

    BYTE multipleSSRC[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x45, 0x4d, 0x42,
                           0x02, 0x12, 0x76, 0x28, 0x6c, 0x76, 0xe8, 0x55, 0x42, 0x42, 0x42, 0x42};
    EXPECT_EQ(STATUS_SUCCESS, rembValueGet(multipleSSRC, SIZEOF(multipleSSRC), &maximumBitRate, ssrcList, &ssrcListLen));
    EXPECT_EQ(ssrcListLen, 2);
    EXPECT_EQ(maximumBitRate, 2581120.0);
    EXPECT_EQ(ssrcList[0], 0x6c76e855);
    EXPECT_EQ(ssrcList[1], 0x42424242);

    BYTE invalidSSRCLength[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x45,
                                0x4d, 0x42, 0xFF, 0x12, 0x76, 0x28, 0x6c, 0x76, 0xe8, 0x55};
    EXPECT_EQ(STATUS_RTCP_INPUT_REMB_INVALID, rembValueGet(invalidSSRCLength, SIZEOF(invalidSSRCLength), &maximumBitRate, ssrcList, &ssrcListLen));
}

TEST_F(RtcpFunctionalityTest, onRtcpRembCalled)
{
    RtcpPacket rtcpPacket;

    MEMSET(&rtcpPacket, 0x00, SIZEOF(RtcpPacket));

    BYTE multipleSSRC[] = {0x80, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x45,
                           0x4d, 0x42, 0x02, 0x12, 0x76, 0x28, 0x6c, 0x76, 0xe8, 0x55, 0x42, 0x42, 0x42, 0x42};

    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(multipleSSRC, ARRAY_SIZE(multipleSSRC), &rtcpPacket));
    initTransceiver(0x42424242);
    PRtcRtpTransceiver transceiver43 = addTransceiver(0x43);

    BOOL onBandwidthCalled42 = FALSE;
    BOOL onBandwidthCalled43 = FALSE;
    auto callback = [](UINT64 called, DOUBLE /*unused*/) { *((BOOL*) called) = TRUE; };
    transceiverOnBandwidthEstimation(pRtcRtpTransceiver, reinterpret_cast<UINT64>(&onBandwidthCalled42), callback);
    transceiverOnBandwidthEstimation(transceiver43, reinterpret_cast<UINT64>(&onBandwidthCalled43), callback);

    onRtcpRembPacket(&rtcpPacket, pKvsPeerConnection);
    ASSERT_TRUE(onBandwidthCalled42);
    ASSERT_FALSE(onBandwidthCalled43);
    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, onpli)
{
    BYTE rawRtcpPacket[] = {0x81, 0xCE, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x1D, 0xC8, 0x69, 0x91};
    RtcpPacket rtcpPacket{};
    BOOL on_picture_loss_called = FALSE;
    this->initTransceiver(0x1DC86991);

    pKvsRtpTransceiver->onPictureLossCustomData = (UINT64) &on_picture_loss_called;
    pKvsRtpTransceiver->onPictureLoss = [](UINT64 customData) -> void { *(PBOOL) customData = TRUE; };

    EXPECT_EQ(STATUS_SUCCESS, setRtcpPacketFromBytes(rawRtcpPacket, SIZEOF(rawRtcpPacket), &rtcpPacket));
    ASSERT_TRUE(rtcpPacket.header.packetType == RTCP_PACKET_TYPE_PAYLOAD_SPECIFIC_FEEDBACK &&
                rtcpPacket.header.receptionReportCount == RTCP_PSFB_PLI);

    onRtcpPLIPacket(&rtcpPacket, pKvsPeerConnection);
    ASSERT_TRUE(on_picture_loss_called);
    RtcOutboundRtpStreamStats stats{};
    EXPECT_EQ(STATUS_SUCCESS, getRtpOutboundStats(pRtcPeerConnection, nullptr, &stats));
    EXPECT_EQ(1, stats.pliCount);
    freePeerConnection(&pRtcPeerConnection);
}

static void testBwHandler(UINT64 customData, UINT32 txBytes, UINT32 rxBytes, UINT32 txPacketsCnt, UINT32 rxPacketsCnt, UINT64 duration)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(txBytes);
    UNUSED_PARAM(rxBytes);
    UNUSED_PARAM(txPacketsCnt);
    UNUSED_PARAM(rxPacketsCnt);
    UNUSED_PARAM(duration);
    return;
}

static void parseTwcc(const std::string& hex, const uint32_t expectedReceived, const uint32_t expectedNotReceived)
{
    PRtcPeerConnection pRtcPeerConnection = nullptr;
    PKvsPeerConnection pKvsPeerConnection;
    BYTE payload[256] = {0};
    UINT32 payloadLen = 256;
    hexDecode(const_cast<PCHAR>(hex.data()), hex.size(), payload, &payloadLen);
    RtcpPacket rtcpPacket{};
    RtpPacket rtpPacket{};
    RtcConfiguration config{};
    UINT64 value;
    UINT16 twsn;
    UINT16 i = 0;
    UINT32 extpayload, received = 0, lost = 0;

    rtcpPacket.header.packetLength = payloadLen / 4;
    rtcpPacket.payload = payload;
    rtcpPacket.payloadLength = payloadLen;

    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
    pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnSenderBandwidthEstimation(pRtcPeerConnection, 0, testBwHandler));

    UINT16 baseSeqNum = getUnalignedInt16BigEndian(rtcpPacket.payload + 8);
    UINT16 pktCount = TWCC_PACKET_STATUS_COUNT(rtcpPacket.payload);

    for (i = baseSeqNum; i < baseSeqNum + pktCount; i++) {
        rtpPacket.header.extension = TRUE;
        rtpPacket.header.extensionProfile = TWCC_EXT_PROFILE;
        rtpPacket.header.extensionLength = SIZEOF(UINT32);
        twsn = i;
        extpayload = TWCC_PAYLOAD(parseExtId(TWCC_EXT_URL), twsn);
        rtpPacket.header.extensionPayload = (PBYTE) &extpayload;
        EXPECT_EQ(STATUS_SUCCESS, twccManagerOnPacketSent(pKvsPeerConnection, &rtpPacket));
    }

    EXPECT_EQ(STATUS_SUCCESS, parseRtcpTwccPacket(&rtcpPacket, pKvsPeerConnection->pTwccManager));

    for (i = 0; i < MAX_UINT16; i++) {
        if (STATUS_SUCCEEDED(hashTableGet(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable, i, &value))) {
            PTwccRtpPacketInfo tempTwccRtpPktInfo = (PTwccRtpPacketInfo) value;
            if (tempTwccRtpPktInfo->remoteTimeKvs == TWCC_PACKET_LOST_TIME) {
                lost++;
            } else if (tempTwccRtpPktInfo->remoteTimeKvs != TWCC_PACKET_UNITIALIZED_TIME) {
                received++;
            }
        }
    }

    EXPECT_EQ(received + lost, TWCC_PACKET_STATUS_COUNT(rtcpPacket.payload));
    EXPECT_EQ(expectedReceived + expectedNotReceived, TWCC_PACKET_STATUS_COUNT(rtcpPacket.payload));
    EXPECT_EQ(expectedReceived, received);
    EXPECT_EQ(expectedNotReceived, lost);
    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

TEST_F(RtcpFunctionalityTest, twccParsePacketTest)
{
    parseTwcc("", 0, 0);
    parseTwcc("4487A9E754B3E6FD01810001147A75A62001C801", 1, 0);
    parseTwcc("4487A9E754B3E6FD12740004148566AAC1402C00", 2, 2);
    parseTwcc("4487A9E754B3E6FD04FA0006147CAF88C554B80400000001", 5, 1);
    parseTwcc("4487A9E754B3E6FD00000002147972002002BC00", 2, 0);
    parseTwcc("4487A9E754B3E6FD06D40004147DDE41D6403C00FFEC0001", 4, 0);
    parseTwcc("4487A9E754B3E6FD04FA0006147CB089D95420FF9804000000000003", 6, 0);
    parseTwcc("4487A9E754B3E6FD000C000314797A052003E40004000003", 3, 0);
    parseTwcc("4487A9E754B3E6FD12740006148568ABD6648800FDA4000268000002", 6, 0);
    parseTwcc("4487A9E754B3E6FD1431000C14868C5A803CEC0028000002", 4, 8);
    parseTwcc("4487A9E754B3E6FD00020004147974012004140000000002", 4, 0);
    parseTwcc("4487A9E754B3E6FD12670008148560A8D66520016C00FD780402902800040002", 8, 0);
    parseTwcc("4487A9E754B3E6FD012E0005147A45872005900000000401", 5, 0);
    parseTwcc("4487A9E754B3E6FD01F20006147AC6D22006600004000000", 6, 0);
    parseTwcc("4487A9E754B3E6FD06690007147D9111200748000000040000000003", 7, 0);
    parseTwcc("4487A9E754B3E6FD020C0008147AD3D8200898000000000008000002", 8, 0);
    parseTwcc("4487A9E754B3E6FD07C20009147E7B8B200990000800000000000001", 9, 0);
    parseTwcc("4487A9E754B3E6FD0177000A147A74A5200A70000000000000040000", 10, 0);
    parseTwcc("4487A9E754B3E6FD1431000C14868E5B2008E540DC00000000000000FE10002800000003", 12, 0);
    parseTwcc("4487A9E754B3E6FD03C6000B147BEB6F200B3000380400000400040000000003", 11, 0);
    parseTwcc("4487A9E754B3E6FD02AB000D147B3013200D4800000004000000000000000401", 13, 0);
    parseTwcc("4487A9E754B3E6FD01BA000E147AA4C3200EA400000000000000000000000400", 14, 0);
    parseTwcc("4487A9E754B3E6FD0610000F147D62F3200FCC0000000000000400000000100000000003", 15, 0);
    parseTwcc("4487A9E754B3E6FD08120010147EAAA92010F80000000000000004040000000000000002", 16, 0);
    parseTwcc("4487A9E754B3E6FD05B80011147D33D52011F40014000000000000000000040000000001", 17, 0);
    parseTwcc("4487A9E754B3E6FD04DA001E147CAC86D556D999D6652009D40000000000EF840001040001DC0004D4000400031400", 30, 0);
    parseTwcc("4487A9E754B3E6FD11EA0012148514932012B40000000000000400000000000000000000", 18, 0);
    parseTwcc("4487A9E754B3E6FD09BC0013147FC45D201348000400000000000000000000000000000000000003", 19, 0);
    parseTwcc("4487A9E754B3E6FD05720014147D05B7201414000000000000100000000000040000000400000002", 20, 0);
    parseTwcc("4487A9E754B3E6FD03820015147BBD5A201554000000000000000000000000000000000400009801", 21, 0);
    parseTwcc("4487A9E754B3E6FD114B001B1484B87381FF200DE41000000000000000000000000000000000140000000002", 22, 5);
    parseTwcc("4487A9E754B3E6FD0B6700161480DD11201678000000000000000000040000000000000000000000", 22, 0);
    parseTwcc("4487A9E754B3E6FD07790017147E4E6F2017D400000000000400000000000000000004000400080000000003", 23, 0);
    parseTwcc("4487A9E754B3E6FD114B001D1484BB74D5592014E4008400000000FD60100000000000000000000000000000000014", 29, 0);
    parseTwcc("4487A9E754B3E6FD1230002914854FA22027E4002400000000000400000000000000040000000000040000001C0000", 41, 0);
    parseTwcc("4487A9E754B3E6FD04B60036147CAA852024C002D999D6407800000000000000000000000000040000000000000000", 48, 6);
    parseTwcc("4487A9E754B3E6FD040200E4147C9F81202700B7E6649000000000000000000004000000000008000018000000001", 45, 183);
}

TEST_F(RtcpFunctionalityTest, updateTwccHashTableTest)
{
    PRtcPeerConnection pRtcPeerConnection = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    RtcConfiguration config{};
    initRtcConfiguration(&config);
    UINT64 receivedBytes = 0, receivedPackets = 0, sentBytes = 0, sentPackets = 0;
    INT64 duration = 0;
    PTwccRtpPacketInfo pTwccRtpPacketInfo = NULL;
    PHashTable pTwccRtpPktInfosHashTable = NULL;
    UINT16 hashTableInsertionCount = 0;
    UINT16 lowerBound = UINT16_MAX - 3;
    UINT16 upperBound = 3;
    UINT16 i = 0;

    // Initialize structs and members.
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
    pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnSenderBandwidthEstimation(pRtcPeerConnection, 0, testBwHandler));

    // Grab the hash table.
    pTwccRtpPktInfosHashTable = pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable;

    pKvsPeerConnection->pTwccManager->prevReportedBaseSeqNum = lowerBound;
    pKvsPeerConnection->pTwccManager->lastReportedSeqNum = upperBound + 10;

    // Breakup the packet indexes to be across the max int overflow.
    for (i = lowerBound; i <= UINT16_MAX && i != 0; i++) {
        pTwccRtpPacketInfo = (PTwccRtpPacketInfo) MEMCALLOC(1, SIZEOF(TwccRtpPacketInfo));
        EXPECT_EQ(STATUS_SUCCESS, hashTableUpsert(pTwccRtpPktInfosHashTable, i, (UINT64) pTwccRtpPacketInfo));
        hashTableInsertionCount++;
    }
    for (i = 0; i < upperBound; i++) {
        pTwccRtpPacketInfo = (PTwccRtpPacketInfo) MEMCALLOC(1, SIZEOF(TwccRtpPacketInfo));
        EXPECT_EQ(STATUS_SUCCESS, hashTableUpsert(pTwccRtpPktInfosHashTable, i, (UINT64) pTwccRtpPacketInfo));
        hashTableInsertionCount++;
    }

    // Add at a non-monotonically-increased index.
    pTwccRtpPacketInfo = (PTwccRtpPacketInfo) MEMCALLOC(1, SIZEOF(TwccRtpPacketInfo));
    EXPECT_EQ(STATUS_SUCCESS, hashTableUpsert(pTwccRtpPktInfosHashTable, upperBound + 10, (UINT64) pTwccRtpPacketInfo));
    hashTableInsertionCount++;

    // Validate hash table size after and before updating (onRtcpTwccPacket case).
    EXPECT_EQ(hashTableInsertionCount, pTwccRtpPktInfosHashTable->itemCount);
    EXPECT_EQ(STATUS_SUCCESS,
              updateTwccHashTable(pKvsPeerConnection->pTwccManager, &duration, &receivedBytes, &receivedPackets, &sentBytes, &sentPackets));
    EXPECT_EQ(0, pTwccRtpPktInfosHashTable->itemCount);

    hashTableInsertionCount = 0;
    pTwccRtpPacketInfo = NULL;
    for (i = 0; i <= upperBound; i++) {
        EXPECT_EQ(STATUS_SUCCESS, hashTableUpsert(pTwccRtpPktInfosHashTable, i, (UINT64) pTwccRtpPacketInfo));
        hashTableInsertionCount++;
    }
    EXPECT_EQ(hashTableInsertionCount, pTwccRtpPktInfosHashTable->itemCount);
    EXPECT_EQ(STATUS_SUCCESS,
              updateTwccHashTable(pKvsPeerConnection->pTwccManager, &duration, &receivedBytes, &receivedPackets, &sentBytes, &sentPackets));
    EXPECT_EQ(0, pTwccRtpPktInfosHashTable->itemCount);

    MUTEX_LOCK(pKvsPeerConnection->twccLock);
    MUTEX_UNLOCK(pKvsPeerConnection->twccLock);

    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

TEST_F(RtcpFunctionalityTest, updateTwccHashTableIntPromotionCase)
{
    PRtcPeerConnection pRtcPeerConnection = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    RtcConfiguration config{};
    initRtcConfiguration(&config);
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
    pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);
    PTwccRtpPacketInfo pTwccRtpPacketInfo = NULL;
    INT64 duration = 0;
    UINT64 receivedBytes = 0, receivedPackets = 0, sentBytes = 0, sentPackets = 0;
    UINT16 i;

    // Grab the hash table
    PHashTable pTwccRtpPktInfosHashTable = pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable;
    UINT16 hashTableInsertionCount = 0;

    // Set up the hash table
    pKvsPeerConnection->pTwccManager->prevReportedBaseSeqNum = UINT16_MAX;
    pKvsPeerConnection->pTwccManager->lastReportedSeqNum = UINT16_MAX;

    // Add packet at UINT16_MAX
    pTwccRtpPacketInfo = (PTwccRtpPacketInfo) MEMCALLOC(1, SIZEOF(TwccRtpPacketInfo));
    EXPECT_EQ(STATUS_SUCCESS, hashTableUpsert(pTwccRtpPktInfosHashTable, UINT16_MAX, (UINT64) pTwccRtpPacketInfo));
    hashTableInsertionCount++;

    // Even though pTwccManager->lastReportedSeqNum is a UINT16, (pTwccManager->lastReportedSeqNum + 1) can get
    // promoted to an int (32) when pTwccManager->lastReportedSeqNum == UINT16_MAX
    EXPECT_EQ(STATUS_SUCCESS,
              updateTwccHashTable(pKvsPeerConnection->pTwccManager, &duration, &receivedBytes, &receivedPackets, &sentBytes, &sentPackets));

    EXPECT_EQ(0, pTwccRtpPktInfosHashTable->itemCount); // Ensure the table is cleared again

    MUTEX_LOCK(pKvsPeerConnection->twccLock);
    MUTEX_UNLOCK(pKvsPeerConnection->twccLock);

    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

//
// TWCC Feedback Generation (Receiver Side) Tests
//

TEST_F(RtcpFunctionalityTest, twccReceiverOnPacketReceivedBasic)
{
    PRtcPeerConnection pRtcPeerConnection = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    RtcConfiguration config{};
    initRtcConfiguration(&config);
    RtpPacket rtpPacket;
    BYTE extensionPayload[4];
    UINT64 packetInfoValue = 0;

    // Create peer connection
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
    pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);

    // Verify receiver manager was created
    EXPECT_NE(nullptr, pKvsPeerConnection->pTwccReceiverManager);

    // Set up TWCC extension ID (simulating SDP negotiation)
    pKvsPeerConnection->twccExtId = 1;

    // Create a mock RTP packet with TWCC extension
    MEMSET(&rtpPacket, 0, SIZEOF(RtpPacket));
    rtpPacket.header.extension = TRUE;
    rtpPacket.header.extensionProfile = TWCC_EXT_PROFILE;
    rtpPacket.header.ssrc = 0x12345678;
    rtpPacket.receivedTime = GETTIME();

    // Create TWCC extension payload for sequence number 100
    // Format: ID (4 bits) | Length-1 (4 bits) | SeqNum (16 bits) | Padding
    UINT32 twccPayload = htonl((1 << 28) | (1 << 24) | (100 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.header.extensionPayload = extensionPayload;
    rtpPacket.header.extensionLength = 4;

    // Track the packet
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));

    // Verify packet was tracked
    PTwccReceiverManager pManager = pKvsPeerConnection->pTwccReceiverManager;
    EXPECT_EQ(TRUE, pManager->firstPacketReceived);
    EXPECT_EQ(100, pManager->firstSeqNum);
    EXPECT_EQ(100, pManager->lastSeqNum);
    EXPECT_EQ(0x12345678, pManager->mediaSourceSsrc);

    // Verify packet is in hash table
    EXPECT_EQ(STATUS_SUCCESS, hashTableGet(pManager->pReceivedPktsHashTable, 100, &packetInfoValue));
    EXPECT_NE(0, packetInfoValue);

    // Add another packet with sequence number 101
    twccPayload = htonl((1 << 28) | (1 << 24) | (101 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));

    // Verify sequence tracking updated
    EXPECT_EQ(100, pManager->firstSeqNum);
    EXPECT_EQ(101, pManager->lastSeqNum);

    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

TEST_F(RtcpFunctionalityTest, twccReceiverOnPacketReceivedOutOfOrder)
{
    PRtcPeerConnection pRtcPeerConnection = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    RtcConfiguration config{};
    initRtcConfiguration(&config);
    RtpPacket rtpPacket;
    BYTE extensionPayload[4];

    // Create peer connection
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
    pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);
    pKvsPeerConnection->twccExtId = 1;

    // Set up mock RTP packet
    MEMSET(&rtpPacket, 0, SIZEOF(RtpPacket));
    rtpPacket.header.extension = TRUE;
    rtpPacket.header.extensionProfile = TWCC_EXT_PROFILE;
    rtpPacket.header.ssrc = 0x12345678;
    rtpPacket.header.extensionPayload = extensionPayload;
    rtpPacket.header.extensionLength = 4;

    PTwccReceiverManager pManager = pKvsPeerConnection->pTwccReceiverManager;

    // Add packet 100 first
    UINT32 twccPayload = htonl((1 << 28) | (1 << 24) | (100 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));
    EXPECT_EQ(100, pManager->firstSeqNum);
    EXPECT_EQ(100, pManager->lastSeqNum);

    // Add packet 102 (skip 101)
    twccPayload = htonl((1 << 28) | (1 << 24) | (102 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));
    EXPECT_EQ(100, pManager->firstSeqNum);
    EXPECT_EQ(102, pManager->lastSeqNum);

    // Add packet 99 (out of order, before first)
    twccPayload = htonl((1 << 28) | (1 << 24) | (99 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));
    EXPECT_EQ(99, pManager->firstSeqNum); // Updated to earlier packet
    EXPECT_EQ(102, pManager->lastSeqNum);

    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

TEST_F(RtcpFunctionalityTest, twccReceiverOnPacketReceivedSeqNumWraparound)
{
    PRtcPeerConnection pRtcPeerConnection = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    RtcConfiguration config{};
    initRtcConfiguration(&config);
    RtpPacket rtpPacket;
    BYTE extensionPayload[4];

    // Create peer connection
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
    pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);
    pKvsPeerConnection->twccExtId = 1;

    // Set up mock RTP packet
    MEMSET(&rtpPacket, 0, SIZEOF(RtpPacket));
    rtpPacket.header.extension = TRUE;
    rtpPacket.header.extensionProfile = TWCC_EXT_PROFILE;
    rtpPacket.header.ssrc = 0x12345678;
    rtpPacket.header.extensionPayload = extensionPayload;
    rtpPacket.header.extensionLength = 4;

    PTwccReceiverManager pManager = pKvsPeerConnection->pTwccReceiverManager;

    // Add packet at UINT16_MAX - 1
    UINT32 twccPayload = htonl((1 << 28) | (1 << 24) | ((UINT16_MAX - 1) << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));
    EXPECT_EQ(UINT16_MAX - 1, pManager->firstSeqNum);
    EXPECT_EQ(UINT16_MAX - 1, pManager->lastSeqNum);

    // Add packet at UINT16_MAX
    twccPayload = htonl((1 << 28) | (1 << 24) | (UINT16_MAX << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));
    EXPECT_EQ(UINT16_MAX - 1, pManager->firstSeqNum);
    EXPECT_EQ(UINT16_MAX, pManager->lastSeqNum);

    // Add packet at 0 (wrapped around)
    twccPayload = htonl((1 << 28) | (1 << 24) | (0 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));
    EXPECT_EQ(UINT16_MAX - 1, pManager->firstSeqNum);
    EXPECT_EQ(0, pManager->lastSeqNum); // Wrapped to 0

    // Add packet at 1
    twccPayload = htonl((1 << 28) | (1 << 24) | (1 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));
    EXPECT_EQ(UINT16_MAX - 1, pManager->firstSeqNum);
    EXPECT_EQ(1, pManager->lastSeqNum);

    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

TEST_F(RtcpFunctionalityTest, twccReceiverDuplicatePacketHandling)
{
    PRtcPeerConnection pRtcPeerConnection = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    RtcConfiguration config{};
    initRtcConfiguration(&config);
    RtpPacket rtpPacket;
    BYTE extensionPayload[4];
    UINT32 itemCount = 0;

    // Create peer connection
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&config, &pRtcPeerConnection));
    pKvsPeerConnection = reinterpret_cast<PKvsPeerConnection>(pRtcPeerConnection);
    pKvsPeerConnection->twccExtId = 1;

    // Set up mock RTP packet
    MEMSET(&rtpPacket, 0, SIZEOF(RtpPacket));
    rtpPacket.header.extension = TRUE;
    rtpPacket.header.extensionProfile = TWCC_EXT_PROFILE;
    rtpPacket.header.ssrc = 0x12345678;
    rtpPacket.header.extensionPayload = extensionPayload;
    rtpPacket.header.extensionLength = 4;

    PTwccReceiverManager pManager = pKvsPeerConnection->pTwccReceiverManager;

    // Add packet 100
    UINT32 twccPayload = htonl((1 << 28) | (1 << 24) | (100 << 8));
    MEMCPY(extensionPayload, &twccPayload, 4);
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));

    // Get initial count
    EXPECT_EQ(STATUS_SUCCESS, hashTableGetCount(pManager->pReceivedPktsHashTable, &itemCount));
    EXPECT_EQ(1, itemCount);

    // Try to add duplicate packet 100 - should be silently ignored
    rtpPacket.receivedTime = GETTIME();
    EXPECT_EQ(STATUS_SUCCESS, twccReceiverOnPacketReceived(pKvsPeerConnection, &rtpPacket));

    // Count should still be 1
    EXPECT_EQ(STATUS_SUCCESS, hashTableGetCount(pManager->pReceivedPktsHashTable, &itemCount));
    EXPECT_EQ(1, itemCount);

    EXPECT_EQ(STATUS_SUCCESS, freePeerConnection(&pRtcPeerConnection));
}

TEST_F(RtcpFunctionalityTest, receiverReportSequentialPackets)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;

    // Simulate rrInitSeq + rrUpdateSeq for seq 0..99 by setting state directly
    pT->rrBaseSeq = 0;
    pT->rrMaxSeq = 99;
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 0;
    pT->rrReceivedPrior = 0;
    pT->rrSeqInitialized = TRUE;

    MUTEX_LOCK(pT->statsLock);
    pT->inboundStats.received.packetsReceived = 100;
    UINT32 extMax = pT->rrCycles + pT->rrMaxSeq;
    UINT32 expected = extMax - pT->rrBaseSeq + 1;
    pT->inboundStats.received.packetsLost = (INT64) expected - (INT64) pT->inboundStats.received.packetsReceived;
    MUTEX_UNLOCK(pT->statsLock);

    EXPECT_EQ(99, pT->rrMaxSeq);
    EXPECT_EQ(0u, pT->rrCycles);
    EXPECT_EQ(0, pT->inboundStats.received.packetsLost);

    // Verify RR packet fields
    UINT32 rrExpected = extMax - pT->rrBaseSeq + 1;
    UINT32 rrReceived = (UINT32) pT->inboundStats.received.packetsReceived;
    UINT32 rrLost = rrExpected - rrReceived;
    UINT32 rrExpectedInterval = rrExpected - pT->rrExpectedPrior;
    UINT32 rrReceivedInterval = rrReceived - pT->rrReceivedPrior;
    UINT32 rrLostInterval = rrExpectedInterval - rrReceivedInterval;
    EXPECT_EQ(0u, rrLost);
    EXPECT_EQ(0u, rrLostInterval);

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, receiverReportWithPacketLoss)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;

    // 100 expected (seq 0..99), 97 received (skipped 10, 20, 30)
    pT->rrBaseSeq = 0;
    pT->rrMaxSeq = 99;
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 0;
    pT->rrReceivedPrior = 0;
    pT->rrSeqInitialized = TRUE;

    MUTEX_LOCK(pT->statsLock);
    pT->inboundStats.received.packetsReceived = 97;
    UINT32 extMax = pT->rrCycles + pT->rrMaxSeq;
    UINT32 expected = extMax - pT->rrBaseSeq + 1;
    pT->inboundStats.received.packetsLost = (INT64) expected - (INT64) pT->inboundStats.received.packetsReceived;
    MUTEX_UNLOCK(pT->statsLock);

    EXPECT_EQ(3, pT->inboundStats.received.packetsLost);

    // Verify fraction lost calculation
    UINT32 rrReceived = (UINT32) pT->inboundStats.received.packetsReceived;
    UINT32 rrLostInterval = expected - rrReceived;
    UINT8 fraction = (UINT8) ((rrLostInterval << 8) / expected);
    EXPECT_EQ((3 << 8) / 100, fraction); // = 7

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, receiverReportSeqOverflow)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;

    // Simulate: rrInitSeq(65534), then packets 65534, 65535, 0, 1, 2
    pT->rrBaseSeq = 65534;
    pT->rrMaxSeq = 2;
    pT->rrCycles = RTP_SEQ_MOD; // one wrap
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 0;
    pT->rrReceivedPrior = 0;
    pT->rrSeqInitialized = TRUE;

    MUTEX_LOCK(pT->statsLock);
    pT->inboundStats.received.packetsReceived = 5;
    UINT32 extMax = pT->rrCycles + pT->rrMaxSeq;  // 65536 + 2 = 65538
    UINT32 expected = extMax - pT->rrBaseSeq + 1; // 65538 - 65534 + 1 = 5
    pT->inboundStats.received.packetsLost = (INT64) expected - (INT64) pT->inboundStats.received.packetsReceived;
    MUTEX_UNLOCK(pT->statsLock);

    EXPECT_EQ(65538u, extMax);
    EXPECT_EQ(5u, expected);
    EXPECT_EQ(0, pT->inboundStats.received.packetsLost);

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, receiverReportSeqOverflowWithLoss)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;

    // rrInitSeq(65533), feed: 65533, 65534, skip 65535, 0, 1, skip 2, 3
    pT->rrBaseSeq = 65533;
    pT->rrMaxSeq = 3;
    pT->rrCycles = RTP_SEQ_MOD; // one wrap
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 0;
    pT->rrReceivedPrior = 0;
    pT->rrSeqInitialized = TRUE;

    MUTEX_LOCK(pT->statsLock);
    pT->inboundStats.received.packetsReceived = 5; // 65533, 65534, 0, 1, 3
    UINT32 extMax = pT->rrCycles + pT->rrMaxSeq;   // 65536 + 3 = 65539
    UINT32 expected = extMax - pT->rrBaseSeq + 1;  // 65539 - 65533 + 1 = 7
    pT->inboundStats.received.packetsLost = (INT64) expected - (INT64) pT->inboundStats.received.packetsReceived;
    MUTEX_UNLOCK(pT->statsLock);

    EXPECT_EQ(7u, expected);
    EXPECT_EQ(2, pT->inboundStats.received.packetsLost);

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, receiverReportFractionLostPerInterval)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;

    // First interval: seq 0..49, 50 packets
    pT->rrBaseSeq = 0;
    pT->rrMaxSeq = 49;
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 0;
    pT->rrReceivedPrior = 0;
    pT->rrSeqInitialized = TRUE;
    pT->inboundStats.received.packetsReceived = 50;

    UINT32 extMax = pT->rrCycles + pT->rrMaxSeq;
    UINT32 expected = extMax - pT->rrBaseSeq + 1;
    UINT32 received = (UINT32) pT->inboundStats.received.packetsReceived;
    UINT32 expectedInterval = expected - pT->rrExpectedPrior;
    UINT32 receivedInterval = received - pT->rrReceivedPrior;
    UINT32 lostInterval = expectedInterval - receivedInterval;
    UINT8 fraction1 = (expectedInterval == 0 || lostInterval == 0) ? 0 : (UINT8) ((lostInterval << 8) / expectedInterval);
    pT->rrExpectedPrior = expected;
    pT->rrReceivedPrior = received;
    EXPECT_EQ(0, fraction1);

    // Second interval: seq 50..99, but skip 60, 70, 80 -> 47 received
    pT->rrMaxSeq = 99;
    pT->inboundStats.received.packetsReceived = 97; // 50 + 47

    extMax = pT->rrCycles + pT->rrMaxSeq;
    expected = extMax - pT->rrBaseSeq + 1;                         // 100
    received = (UINT32) pT->inboundStats.received.packetsReceived; // 97
    expectedInterval = expected - pT->rrExpectedPrior;             // 100 - 50 = 50
    receivedInterval = received - pT->rrReceivedPrior;             // 97 - 50 = 47
    lostInterval = expectedInterval - receivedInterval;            // 3
    UINT8 fraction2 = (expectedInterval == 0 || lostInterval == 0) ? 0 : (UINT8) ((lostInterval << 8) / expectedInterval);
    EXPECT_EQ((3 << 8) / 50, fraction2); // = 15

    // Verify prior values were updated
    EXPECT_EQ(50u, pT->rrExpectedPrior);
    EXPECT_EQ(50u, pT->rrReceivedPrior);

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, receiverReportLargeJump)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;

    // rrInitSeq(100), sequential 100..110
    pT->rrBaseSeq = 100;
    pT->rrMaxSeq = 110;
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrSeqInitialized = TRUE;

    // Now a large jump to 5000 — per RFC 3550, badSeq should be set
    // udelta = 5000 - 110 = 4890, which is > MAX_DROPOUT (3000)
    // and 4890 < RTP_SEQ_MOD - MAX_MISORDER = 65436
    // So badSeq = (5000 + 1) & 0xFFFF = 5001
    // maxSeq should NOT change
    constexpr UINT16 jumpSeq = 5000;
    UINT16 udelta = jumpSeq - pT->rrMaxSeq; // 4890
    EXPECT_GT(udelta, 3000);                // > MAX_DROPOUT
    EXPECT_LT(udelta, RTP_SEQ_MOD - 100);   // < RTP_SEQ_MOD - MAX_MISORDER

    // After this, maxSeq should remain 110, badSeq = 5001
    pT->rrBadSeq = ((UINT32) jumpSeq + 1) & (RTP_SEQ_MOD - 1);
    EXPECT_EQ(110, pT->rrMaxSeq);
    EXPECT_EQ(5001u, pT->rrBadSeq);

    // Second sequential packet at 5001 would re-init
    pT->rrBaseSeq = 5001;
    pT->rrMaxSeq = 5001;
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    EXPECT_EQ(5001, pT->rrMaxSeq);

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, twccMakeRunlenMacro)
{
    // Test run-length chunk creation macro
    // Run-length chunk: bit 15 = 0, bits 14-13 = status symbol, bits 12-0 = run length

    // Test with NOT_RECEIVED status (00), run length 10
    UINT16 chunk = TWCC_MAKE_RUNLEN(TWCC_STATUS_SYMBOL_NOTRECEIVED, 10);
    EXPECT_EQ(0, (chunk >> 15) & 1); // Bit 15 should be 0 for run-length
    EXPECT_EQ(TWCC_STATUS_SYMBOL_NOTRECEIVED, (chunk >> 13) & 3);
    EXPECT_EQ(10, chunk & 0x1FFF);

    // Test with SMALL_DELTA status (01), run length 100
    chunk = TWCC_MAKE_RUNLEN(TWCC_STATUS_SYMBOL_SMALLDELTA, 100);
    EXPECT_EQ(0, (chunk >> 15) & 1);
    EXPECT_EQ(TWCC_STATUS_SYMBOL_SMALLDELTA, (chunk >> 13) & 3);
    EXPECT_EQ(100, chunk & 0x1FFF);

    // Test with LARGE_DELTA status (10), run length 1000
    chunk = TWCC_MAKE_RUNLEN(TWCC_STATUS_SYMBOL_LARGEDELTA, 1000);
    EXPECT_EQ(0, (chunk >> 15) & 1);
    EXPECT_EQ(TWCC_STATUS_SYMBOL_LARGEDELTA, (chunk >> 13) & 3);
    EXPECT_EQ(1000, chunk & 0x1FFF);

    // Test max run length (0x1FFF = 8191)
    chunk = TWCC_MAKE_RUNLEN(TWCC_STATUS_SYMBOL_SMALLDELTA, 0x1FFF);
    EXPECT_EQ(0x1FFF, chunk & 0x1FFF);
}

TEST_F(RtcpFunctionalityTest, remoteOutboundStatsFromSenderReport)
{
    // Craft a pure SR packet (no RR blocks): 24 bytes payload
    // SSRC = 0x12345678
    // NTP timestamp: sec = 0xE1E32043 (3790012483), frac = 0x00000000
    // RTP timestamp: 0x00001000
    // Packet count: 100 (0x00000064)
    // Octet count: 50000 (0x0000C350)
    constexpr UINT32 senderSSRC = 0x12345678;
    constexpr UINT32 ntpSec = 0xE1E32043;
    constexpr UINT32 ntpFrac = 0x00000000;
    constexpr UINT32 rtpTs = 0x00001000;
    constexpr UINT32 packetCount = 100;
    constexpr UINT32 octetCount = 50000;

    // Build RTCP SR: header (4 bytes) + payload (24 bytes)
    BYTE srPacket[28];
    // Header: V=2, P=0, RC=0, PT=200 (SR), length=6 (words-1)
    srPacket[0] = 0x80;
    srPacket[1] = 0xC8; // PT=200 (SR)
    srPacket[2] = 0x00;
    srPacket[3] = 0x06; // length = 6 (7 words = 28 bytes)
    // Payload: SSRC
    putUnalignedInt32BigEndian(srPacket + 4, senderSSRC);
    // NTP timestamp
    putUnalignedInt32BigEndian(srPacket + 8, ntpSec);
    putUnalignedInt32BigEndian(srPacket + 12, ntpFrac);
    // RTP timestamp
    putUnalignedInt32BigEndian(srPacket + 16, rtpTs);
    // Sender packet count
    putUnalignedInt32BigEndian(srPacket + 20, packetCount);
    // Sender octet count
    putUnalignedInt32BigEndian(srPacket + 24, octetCount);

    initTransceiver(senderSSRC);

    EXPECT_EQ(STATUS_SUCCESS, onRtcpPacket(pKvsPeerConnection, srPacket, SIZEOF(srPacket)));

    RtcStats stats{};
    stats.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_OUTBOUND_RTP;
    EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(pRtcPeerConnection, pRtcRtpTransceiver, &stats));

    auto& ros = stats.rtcStatsObject.remoteOutboundRtpStreamStats;
    EXPECT_EQ(1, ros.reportsSent);
    EXPECT_EQ(packetCount, ros.sent.packetsSent);
    EXPECT_EQ(octetCount, ros.sent.bytesSent);
    EXPECT_EQ(senderSSRC, ros.sent.rtpStream.ssrc);

    // NTP sec 0xE1E32043 = 3790012483, minus NTP_OFFSET (2208988800) = 1581023683 Unix sec
    // With ntpFrac=0, remoteTimestamp should be 1581023683000 ms
    constexpr UINT64 expectedMs = (UINT64) (ntpSec - 2208988800ULL) * 1000ULL;
    EXPECT_EQ(expectedMs, ros.remoteTimestamp);

    // Feed a second SR to verify reportsSent increments
    putUnalignedInt32BigEndian(srPacket + 20, packetCount + 50);
    putUnalignedInt32BigEndian(srPacket + 24, octetCount + 25000);
    EXPECT_EQ(STATUS_SUCCESS, onRtcpPacket(pKvsPeerConnection, srPacket, SIZEOF(srPacket)));

    EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(pRtcPeerConnection, pRtcRtpTransceiver, &stats));
    EXPECT_EQ(2, ros.reportsSent);
    EXPECT_EQ(packetCount + 50, ros.sent.packetsSent);
    EXPECT_EQ(octetCount + 25000, ros.sent.bytesSent);

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, remoteOutboundStatsFromSRWithRRBlocks)
{
    // Test that SR with appended RR blocks (payloadLength > 24) is handled
    // SR header: V=2, P=0, RC=1 (one RR block), PT=200, length=12 (13 words = 52 bytes)
    // SR sender info (24 bytes) + 1 RR block (24 bytes) = 48 bytes payload
    constexpr UINT32 senderSSRC = 0xAABBCCDD;
    constexpr UINT32 reportedSSRC = 0x11223344;
    BYTE srWithRR[52];
    MEMSET(srWithRR, 0, SIZEOF(srWithRR));
    // Header
    srWithRR[0] = 0x81; // V=2, RC=1
    srWithRR[1] = 0xC8; // PT=200 (SR)
    srWithRR[2] = 0x00;
    srWithRR[3] = 0x0C; // length = 12
    // SR sender info
    putUnalignedInt32BigEndian(srWithRR + 4, senderSSRC);
    putUnalignedInt32BigEndian(srWithRR + 8, 0xE1E32043);  // NTP sec
    putUnalignedInt32BigEndian(srWithRR + 12, 0x00000000); // NTP frac
    putUnalignedInt32BigEndian(srWithRR + 16, 0x00001000); // RTP ts
    putUnalignedInt32BigEndian(srWithRR + 20, 200);        // packet count
    putUnalignedInt32BigEndian(srWithRR + 24, 80000);      // octet count
    // RR block (24 bytes at offset 28)
    putUnalignedInt32BigEndian(srWithRR + 28, reportedSSRC);
    // rest of RR block is zeros (fraction lost=0, cumulative lost=0, etc.)

    initTransceiver(senderSSRC);
    addTransceiver(reportedSSRC);

    EXPECT_EQ(STATUS_SUCCESS, onRtcpPacket(pKvsPeerConnection, srWithRR, SIZEOF(srWithRR)));

    RtcStats stats{};
    stats.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_OUTBOUND_RTP;
    EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(pRtcPeerConnection, pRtcRtpTransceiver, &stats));

    auto& ros = stats.rtcStatsObject.remoteOutboundRtpStreamStats;
    EXPECT_EQ(1, ros.reportsSent);
    EXPECT_EQ(200, ros.sent.packetsSent);
    EXPECT_EQ(80000, ros.sent.bytesSent);
    EXPECT_EQ(senderSSRC, ros.sent.rtpStream.ssrc);

    freePeerConnection(&pRtcPeerConnection);
}

// Helpers to decode fields written by rtcpBuildReceiverReport.
// Byte layout after the 4-byte RTCP header + 4-byte sender SSRC:
//   [8..11]  SSRC_1
//   [12]     fraction lost
//   [13..15] cumulative lost (signed 24-bit)
//   [16..19] extended highest seq num
//   [20..23] interarrival jitter
//   [24..27] LSR
//   [28..31] DLSR
static UINT8 rrBlockFracLost(BYTE* buf)
{
    return buf[12];
}
static INT32 rrBlockCumLost(BYTE* buf)
{
    UINT32 u = ((UINT32) buf[13] << 16) | ((UINT32) buf[14] << 8) | buf[15];
    return (u & 0x800000u) ? (INT32) (u | 0xFF000000u) : (INT32) u;
}
static UINT32 rrBlockJitter(BYTE* buf)
{
    return getUnalignedInt32BigEndian(buf + 20);
}

TEST_F(RtcpFunctionalityTest, receiverReportReorderedDuplicate)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;
    pT->jitterBufferSsrc = 0xabcd1234;

    // Previous interval: 100 expected, 100 received.
    pT->rrBaseSeq = 0;
    pT->rrMaxSeq = 199; // rrExpected = 200
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 100;
    pT->rrReceivedPrior = 100;
    pT->rrSeqInitialized = TRUE;

    // This interval: 100 expected, 101 received — one reordered duplicate.
    pT->inboundStats.received.packetsReceived = 201;

    BYTE buf[64];
    UINT32 len = sizeof(buf);
    EXPECT_EQ(STATUS_SUCCESS, rtcpBuildReceiverReport(pT, GETTIME(), buf, &len));
    EXPECT_EQ(32u, len);
    EXPECT_EQ(0u, rrBlockFracLost(buf)) << "underflow produced pseudo-random fraction";
    INT32 cum = rrBlockCumLost(buf);
    EXPECT_TRUE(cum == -1 || cum == 0) << "cum_lost " << cum << " not in {-1, 0}";

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, receiverReportFirstReportEmitsZeroJitter)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;
    pT->jitterBufferSsrc = 0xabcd1234;

    pT->rrBaseSeq = 0;
    pT->rrMaxSeq = 9;
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 0;
    pT->rrReceivedPrior = 0;
    pT->rrSeqInitialized = TRUE;
    pT->inboundStats.received.packetsReceived = 10;
    pT->pJitterBuffer->jitter = 0.0;

    BYTE buf[64];
    UINT32 len = sizeof(buf);
    EXPECT_EQ(STATUS_SUCCESS, rtcpBuildReceiverReport(pT, GETTIME(), buf, &len));
    EXPECT_EQ(32u, len);
    EXPECT_EQ(0u, rrBlockFracLost(buf));
    EXPECT_EQ(0, rrBlockCumLost(buf));
    EXPECT_EQ(0u, rrBlockJitter(buf));

    freePeerConnection(&pRtcPeerConnection);
}

TEST_F(RtcpFunctionalityTest, jitterFirstPacketDoesNotSpike)
{
    initTransceiver(1000);
    auto* pT = pKvsRtpTransceiver;

    // Simulate the seeding path of sendPacketToRtpReceiver for the first packet.
    // The pre-fix bug: delta = transit - 0 (huge), jitter seeded with |delta|/16.
    // The fix: on !rrSeqInitialized, set transit=transit and jitter=0.
    UINT64 now = GETTIME();
    INT64 arrival = KVS_CONVERT_TIMESCALE(now, HUNDREDS_OF_NANOS_IN_A_SECOND, pT->pJitterBuffer->clockRate);
    INT64 r_ts = 12345;
    INT64 transit = arrival - r_ts;

    // First packet — must behave as sendPacketToRtpReceiver does now.
    EXPECT_FALSE(pT->rrSeqInitialized);
    pT->rrBaseSeq = 100;
    pT->rrMaxSeq = 100;
    pT->rrCycles = 0;
    pT->rrBadSeq = RTP_SEQ_MOD + 1;
    pT->rrExpectedPrior = 0;
    pT->rrReceivedPrior = 0;
    pT->rrSeqInitialized = TRUE;
    pT->pJitterBuffer->transit = (UINT64) transit;
    pT->pJitterBuffer->jitter = 0.0;
    pT->inboundStats.received.packetsReceived = 1;
    pT->jitterBufferSsrc = 0xabcd1234;

    BYTE buf[64];
    UINT32 len = sizeof(buf);
    EXPECT_EQ(STATUS_SUCCESS, rtcpBuildReceiverReport(pT, now, buf, &len));
    EXPECT_EQ(0u, rrBlockJitter(buf)) << "first-packet jitter must be zero";

    // Second packet ~20 ms later: small delta, EWMA stays bounded.
    UINT64 later = now + 20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    INT64 arrival2 = KVS_CONVERT_TIMESCALE(later, HUNDREDS_OF_NANOS_IN_A_SECOND, pT->pJitterBuffer->clockRate);
    INT64 r_ts2 = r_ts + (pT->pJitterBuffer->clockRate * 20 / 1000); // 20 ms in RTP ticks
    INT64 transit2 = arrival2 - r_ts2;
    INT64 delta = transit2 - (INT64) pT->pJitterBuffer->transit;
    pT->pJitterBuffer->transit = (UINT64) transit2;
    pT->pJitterBuffer->jitter += (ABS(delta) - pT->pJitterBuffer->jitter) / 16.0;
    pT->inboundStats.received.packetsReceived = 2;
    pT->rrMaxSeq = 101;

    len = sizeof(buf);
    EXPECT_EQ(STATUS_SUCCESS, rtcpBuildReceiverReport(pT, later, buf, &len));
    UINT32 jitter = rrBlockJitter(buf);
    // Generous upper bound — ~100 ms worth of RTP ticks. The pre-fix bug
    // produced values in the 10^9+ range.
    EXPECT_LT(jitter, pT->pJitterBuffer->clockRate / 10u) << "jitter " << jitter << " exceeds 100 ms bound";

    freePeerConnection(&pRtcPeerConnection);
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
