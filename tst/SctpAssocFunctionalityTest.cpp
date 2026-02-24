#include "SctpTestHelpers.h"

#ifdef ENABLE_DATA_CHANNEL
namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class SctpAssocFunctionalityTest : public WebRtcClientTestBase {
  protected:
    TwoAssocHarness h;

    void SetUp() override
    {
        WebRtcClientTestBase::SetUp();
        h.init();
    }

    void TearDown() override
    {
        h.cleanup();
        WebRtcClientTestBase::TearDown();
    }
};

/******************************************************************************
 * Handshake
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, handshake_fullFourWay)
{
    h.completeHandshake();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_ESTABLISHED);
}

TEST_F(SctpAssocFunctionalityTest, handshake_bothEstablished)
{
    h.completeHandshake();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_ESTABLISHED);
}

TEST_F(SctpAssocFunctionalityTest, handshake_tagsConsistent)
{
    h.completeHandshake();
    EXPECT_EQ(h.assocA.peerVerificationTag, h.assocB.myVerificationTag);
    EXPECT_EQ(h.assocB.peerVerificationTag, h.assocA.myVerificationTag);
}

TEST_F(SctpAssocFunctionalityTest, handshake_simultaneousOpen)
{
    // Both call connect, then exchange
    h.capA.reset();
    h.capB.reset();
    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    sctpAssocConnect(&h.assocB, mockOutboundCapture, (UINT64) &h.capB);

    // Exchange INITs → both send INIT-ACKs
    h.exchangePackets();
    // Exchange INIT-ACKs → both send COOKIE-ECHOs
    h.exchangePackets();
    // Exchange COOKIE-ECHOs → both send COOKIE-ACKs
    h.exchangePackets();
    // Exchange COOKIE-ACKs → both ESTABLISHED
    h.exchangePackets();

    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_ESTABLISHED);
}

TEST_F(SctpAssocFunctionalityTest, handshake_tagsAreRandom)
{
    UINT32 tagA1 = h.assocA.myVerificationTag;

    // Init a new association
    SctpAssociation assocC;
    sctpAssocInit(&assocC, 5000, 5000, 1188);
    UINT32 tagC = assocC.myVerificationTag;

    // Tags should be different (with overwhelming probability)
    EXPECT_NE(tagA1, tagC);
}

TEST_F(SctpAssocFunctionalityTest, handshake_differentPorts)
{
    h.cleanup();
    h.init(5000, 6000);

    h.completeHandshake();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_ESTABLISHED);
}

/******************************************************************************
 * Basic Data Transfer
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, data_singleMessage_AtoB)
{
    h.completeHandshake();

    BYTE payload[] = "hello from A";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 12, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets(); // DATA → B, B sends SACK

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, (UINT32) 12);
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, payload, 12));
}

TEST_F(SctpAssocFunctionalityTest, data_singleMessage_BtoA)
{
    h.completeHandshake();

    BYTE payload[] = "hello from B";
    sctpAssocSend(&h.assocB, 0, SCTP_PPID_STRING, FALSE, payload, 12, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capB);

    h.exchangePackets();

    ASSERT_GE(h.msgA.count, (UINT32) 1);
    EXPECT_EQ(h.msgA.messages[0].payloadLen, (UINT32) 12);
    EXPECT_EQ(0, MEMCMP(h.msgA.messages[0].payload, payload, 12));
}

TEST_F(SctpAssocFunctionalityTest, data_bidirectional)
{
    h.completeHandshake();

    BYTE payloadA[] = "from A";
    BYTE payloadB[] = "from B";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payloadA, 6, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    sctpAssocSend(&h.assocB, 0, SCTP_PPID_STRING, FALSE, payloadB, 6, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capB);

    h.exchangePackets();

    EXPECT_GE(h.msgA.count, (UINT32) 1);
    EXPECT_GE(h.msgB.count, (UINT32) 1);
}

TEST_F(SctpAssocFunctionalityTest, data_multipleMessages)
{
    h.completeHandshake();

    for (int i = 0; i < 10; i++) {
        BYTE payload[4];
        payload[0] = (BYTE) i;
        payload[1] = (BYTE)(i + 1);
        payload[2] = (BYTE)(i + 2);
        payload[3] = (BYTE)(i + 3);
        sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 4, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    }

    // Multiple exchanges to deliver all DATA and SACKs
    h.exchangePackets();
    h.exchangePackets();

    EXPECT_EQ(h.msgB.count, (UINT32) 10);
}

TEST_F(SctpAssocFunctionalityTest, data_emptyPayload)
{
    h.completeHandshake();

    BYTE dummy = 0;
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, &dummy, 0, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, (UINT32) 0);
}

TEST_F(SctpAssocFunctionalityTest, data_binaryPpid)
{
    h.completeHandshake();

    BYTE payload[] = {0x01, 0x02};
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload, 2, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].ppid, (UINT32) SCTP_PPID_BINARY);
}

TEST_F(SctpAssocFunctionalityTest, data_stringPpid)
{
    h.completeHandshake();

    BYTE payload[] = "text";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 4, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].ppid, (UINT32) SCTP_PPID_STRING);
}

TEST_F(SctpAssocFunctionalityTest, data_dcepPpid)
{
    h.completeHandshake();

    BYTE payload[] = {0x03, 0x00};
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_DCEP, FALSE, payload, 2, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].ppid, (UINT32) SCTP_PPID_DCEP);
}

TEST_F(SctpAssocFunctionalityTest, data_payloadIntegrity)
{
    h.completeHandshake();

    BYTE payload[1000];
    for (UINT32 i = 0; i < 1000; i++) {
        payload[i] = (BYTE)(i & 0xFF);
    }

    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload, 1000, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, (UINT32) 1000);
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, payload, 1000));
}

TEST_F(SctpAssocFunctionalityTest, data_streamId0)
{
    h.completeHandshake();

    BYTE payload[] = "a";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].streamId, (UINT32) 0);
}

TEST_F(SctpAssocFunctionalityTest, data_streamId255)
{
    h.completeHandshake();

    BYTE payload[] = "a";
    sctpAssocSend(&h.assocA, 255, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].streamId, (UINT32) 255);
}

/******************************************************************************
 * Ordered vs Unordered
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, ordered_deliveredInOrder)
{
    h.completeHandshake();

    for (int i = 0; i < 5; i++) {
        BYTE payload[1] = {(BYTE) i};
        sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload, 1, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    }

    h.exchangePackets();
    h.exchangePackets();

    EXPECT_EQ(h.msgB.count, (UINT32) 5);
    for (UINT32 i = 0; i < h.msgB.count; i++) {
        EXPECT_EQ(h.msgB.messages[i].payload[0], (BYTE) i);
    }
}

TEST_F(SctpAssocFunctionalityTest, unordered_delivered)
{
    h.completeHandshake();

    BYTE payload[] = "unordered";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, TRUE, payload, 9, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, (UINT32) 9);
}

TEST_F(SctpAssocFunctionalityTest, multiStream_orderedPerStream)
{
    h.completeHandshake();

    BYTE p1[] = "s0";
    BYTE p2[] = "s1";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, p1, 2, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    sctpAssocSend(&h.assocA, 1, SCTP_PPID_STRING, FALSE, p2, 2, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();

    EXPECT_EQ(h.msgB.count, (UINT32) 2);
}

/******************************************************************************
 * SACK End-to-End
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, sack_outstandingFreed)
{
    h.completeHandshake();

    BYTE payload[] = "hello";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 1);

    h.exchangePackets(); // DATA→B → SACK
    h.exchangePackets(); // SACK→A

    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 0);
}

TEST_F(SctpAssocFunctionalityTest, sack_flightSizeZeroAfterAck)
{
    h.completeHandshake();

    BYTE payload[] = "hello";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    EXPECT_EQ(h.assocA.flightSize, (UINT32) 5);

    h.exchangePackets();
    h.exchangePackets();

    EXPECT_EQ(h.assocA.flightSize, (UINT32) 0);
}

/******************************************************************************
 * Fragment Reassembly
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, fragment_oversizedMessage)
{
    h.completeHandshake();

    // Send a message larger than MTU — will be fragmented
    UINT32 maxDataPayload = h.assocA.mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;
    UINT32 bigLen = maxDataPayload * 2 + 10; // ~2.x fragments
    BYTE* bigPayload = (BYTE*) MEMALLOC(bigLen);
    ASSERT_TRUE(bigPayload != NULL);
    for (UINT32 i = 0; i < bigLen; i++) {
        bigPayload[i] = (BYTE)(i & 0xFF);
    }

    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, bigPayload, bigLen, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Multiple fragments sent
    EXPECT_GE(h.capA.count, (UINT32) 2);

    h.exchangePackets(); // fragments → B, B sends SACKs
    h.exchangePackets(); // SACKs → A

    // B should have reassembled the full message
    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, bigLen);
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, bigPayload, bigLen));

    MEMFREE(bigPayload);
}

TEST_F(SctpAssocFunctionalityTest, fragment_exactMtuBoundary)
{
    h.completeHandshake();

    UINT32 maxDataPayload = h.assocA.mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;

    BYTE* payload = (BYTE*) MEMALLOC(maxDataPayload);
    ASSERT_TRUE(payload != NULL);
    MEMSET(payload, 0x42, maxDataPayload);

    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload, maxDataPayload, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Should be single fragment (not fragmented)
    EXPECT_EQ(h.capA.count, (UINT32) 1);

    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, maxDataPayload);

    MEMFREE(payload);
}

TEST_F(SctpAssocFunctionalityTest, fragment_mtuPlus1)
{
    h.completeHandshake();

    UINT32 maxDataPayload = h.assocA.mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;
    UINT32 len = maxDataPayload + 1;

    BYTE* payload = (BYTE*) MEMALLOC(len);
    ASSERT_TRUE(payload != NULL);
    for (UINT32 i = 0; i < len; i++) {
        payload[i] = (BYTE)(i & 0xFF);
    }

    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload, len, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Should be 2 fragments
    EXPECT_EQ(h.capA.count, (UINT32) 2);

    h.exchangePackets();
    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, len);
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, payload, len));

    MEMFREE(payload);
}

TEST_F(SctpAssocFunctionalityTest, fragment_4000ByteMessage)
{
    h.completeHandshake();

    UINT32 len = 4000;
    BYTE* payload = (BYTE*) MEMALLOC(len);
    ASSERT_TRUE(payload != NULL);
    for (UINT32 i = 0; i < len; i++) {
        payload[i] = (BYTE)(i & 0xFF);
    }

    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload, len, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();
    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, len);
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, payload, len));

    MEMFREE(payload);
}

TEST_F(SctpAssocFunctionalityTest, fragment_multipleFragmentedMessages)
{
    h.completeHandshake();

    UINT32 maxDataPayload = h.assocA.mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;
    UINT32 len = maxDataPayload * 2;

    BYTE* payload1 = (BYTE*) MEMALLOC(len);
    BYTE* payload2 = (BYTE*) MEMALLOC(len);
    ASSERT_TRUE(payload1 != NULL && payload2 != NULL);
    MEMSET(payload1, 0xAA, len);
    MEMSET(payload2, 0xBB, len);

    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload1, len, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Exchange first message's fragments
    h.exchangePackets();
    h.exchangePackets();

    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload2, len, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();
    h.exchangePackets();

    EXPECT_GE(h.msgB.count, (UINT32) 2);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, len);
    EXPECT_EQ(h.msgB.messages[0].payload[0], (BYTE) 0xAA);
    EXPECT_EQ(h.msgB.messages[1].payloadLen, len);
    EXPECT_EQ(h.msgB.messages[1].payload[0], (BYTE) 0xBB);

    MEMFREE(payload1);
    MEMFREE(payload2);
}

/******************************************************************************
 * Retransmission
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, retransmit_lostDataRetransmitted)
{
    h.completeHandshake();

    BYTE payload[] = "important";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 9, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // "Lose" the DATA by clearing capA without delivering to B
    h.capA.reset();

    // Trigger T3 timer on A
    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);

    // Now deliver the retransmitted DATA
    h.exchangePackets();

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, (UINT32) 9);
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, payload, 9));
}

TEST_F(SctpAssocFunctionalityTest, retransmit_cwndResetOnT3Expiry)
{
    h.completeHandshake();

    BYTE payload[] = "hello";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundNoop, 0);

    EXPECT_EQ(h.assocA.cwnd, (UINT32) h.assocA.mtu);
}

TEST_F(SctpAssocFunctionalityTest, retransmit_recoveryAfterRetransmit)
{
    h.completeHandshake();

    BYTE payload[] = "hello";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Lose first DATA
    h.capA.reset();

    // Trigger retransmit
    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);

    // Deliver retransmitted DATA → get SACK
    h.exchangePackets();
    h.exchangePackets();

    // A should have 0 outstanding
    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 0);

    // Normal sending should work
    BYTE payload2[] = "after recovery";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload2, 14, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    h.exchangePackets();
    h.exchangePackets();

    EXPECT_GE(h.msgB.count, (UINT32) 2);
}

/******************************************************************************
 * PR-SCTP End-to-End
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, prsctp_forwardTsn_receiverAdvances)
{
    h.completeHandshake();
    UINT32 basePeerCumTsn = h.assocB.peerCumulativeTsn;

    BYTE payload[] = "ephemeral";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 9, 0, 0, mockOutboundCapture, (UINT64) &h.capA); // maxRetransmits=0

    // Lose the DATA
    h.capA.reset();

    // Trigger T3 → abandon → FORWARD-TSN
    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);

    // Deliver FORWARD-TSN to B
    h.exchangePackets();

    EXPECT_GT(h.assocB.peerCumulativeTsn, basePeerCumTsn);
}

TEST_F(SctpAssocFunctionalityTest, prsctp_subsequentDataDelivered)
{
    h.completeHandshake();

    // Send a message that will be abandoned
    BYTE payload1[] = "abandon me";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload1, 10, 0, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Lose it
    h.capA.reset();

    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);

    // Deliver FORWARD-TSN
    h.exchangePackets();
    h.exchangePackets();

    // Now send a normal message
    BYTE payload2[] = "keep me";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload2, 7, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets();
    h.exchangePackets();

    // Check that the second message was delivered
    BOOL found = FALSE;
    for (UINT32 i = 0; i < h.msgB.count; i++) {
        if (h.msgB.messages[i].payloadLen == 7 && MEMCMP(h.msgB.messages[i].payload, payload2, 7) == 0) {
            found = TRUE;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SctpAssocFunctionalityTest, prsctp_unlimitedRetransmits)
{
    h.completeHandshake();

    BYTE payload[] = "durable";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 7, 0xFFFF, 0, mockOutboundNoop, 0);

    // Multiple T3 timeouts should NOT abandon (maxRetransmits=0xFFFF)
    for (int i = 0; i < 5; i++) {
        UINT64 time = h.assocA.t3RtxExpiry + 1;
        sctpAssocCheckTimers(&h.assocA, time, mockOutboundNoop, 0);
    }

    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 1); // still outstanding
}

/******************************************************************************
 * Congestion Control
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, congestion_initialCwnd)
{
    h.completeHandshake();
    EXPECT_EQ(h.assocA.cwnd, (UINT32)(SCTP_INITIAL_CWND_MTUS * h.assocA.mtu));
}

TEST_F(SctpAssocFunctionalityTest, congestion_lossReducesCwnd)
{
    h.completeHandshake();

    BYTE payload[] = "hello";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundNoop, 0);

    EXPECT_EQ(h.assocA.cwnd, (UINT32) h.assocA.mtu);
}

/******************************************************************************
 * Shutdown Sequence
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, shutdown_gracefulSequence)
{
    h.completeHandshake();

    // A initiates shutdown
    sctpAssocShutdown(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_SHUTDOWN_SENT);

    // B receives SHUTDOWN → sends SHUTDOWN-ACK
    h.exchangePackets();
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_SHUTDOWN_ACK_SENT);

    // A receives SHUTDOWN-ACK → sends SHUTDOWN-COMPLETE
    h.exchangePackets();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_CLOSED);

    // B receives SHUTDOWN-COMPLETE → CLOSED
    h.exchangePackets();
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_CLOSED);
}

TEST_F(SctpAssocFunctionalityTest, shutdown_duringDataTransfer)
{
    h.completeHandshake();

    BYTE payload[] = "before shutdown";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 15, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    h.exchangePackets(); // DATA→B
    h.exchangePackets(); // SACK→A

    // Now shutdown
    sctpAssocShutdown(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    h.exchangePackets();
    h.exchangePackets();
    h.exchangePackets();

    // Both should be closed
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_CLOSED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_CLOSED);
}

TEST_F(SctpAssocFunctionalityTest, shutdown_bothSidesClean)
{
    h.completeHandshake();

    sctpAssocShutdown(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    h.exchangePackets();
    h.exchangePackets();
    h.exchangePackets();

    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_CLOSED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_CLOSED);
}

/******************************************************************************
 * Heartbeat
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, heartbeat_echoesData)
{
    h.completeHandshake();

    // Build a HEARTBEAT packet
    BYTE hbPacket[128];
    UINT32 off = sctpWriteCommonHeader(hbPacket, 5000, 5000, h.assocA.myVerificationTag);
    BYTE hbData[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    hbPacket[off] = SCTP_CHUNK_HEARTBEAT;
    hbPacket[off + 1] = 0;
    putUnalignedInt16BigEndian(hbPacket + off + 2, (INT16)(SCTP_CHUNK_HEADER_SIZE + 8));
    MEMCPY(hbPacket + off + SCTP_CHUNK_HEADER_SIZE, hbData, 8);
    off += SCTP_PAD4(SCTP_CHUNK_HEADER_SIZE + 8);
    sctpFinalizePacket(hbPacket, off);

    h.capA.reset();
    sctpAssocHandlePacket(&h.assocA, hbPacket, off, mockOutboundCapture, (UINT64) &h.capA, mockMessageNoop, 0);

    // Should respond with HEARTBEAT-ACK
    ASSERT_GE(h.capA.count, (UINT32) 1);
    EXPECT_EQ(h.capA.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_HEARTBEAT_ACK);

    // Echoed data should match
    PBYTE pHbAckValue = h.capA.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE;
    EXPECT_EQ(0, MEMCMP(pHbAckValue, hbData, 8));
}

/******************************************************************************
 * Handshake Edge Cases
 * Matches Rust: resend_init_and_establish_connection,
 *   resend_cookie_echo_and_establish_connection,
 *   establish_connection_while_sending_data,
 *   resending_init_too_many_times_aborts,
 *   resending_cookie_echo_too_many_times_aborts
 *****************************************************************************/

// INIT retransmit after drop → full connection established
TEST_F(SctpAssocFunctionalityTest, handshake_initRetransmitEstablishes)
{
    h.capA.reset();
    h.capB.reset();

    // A sends INIT
    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    ASSERT_GE(h.capA.count, (UINT32) 1);

    // "Lose" A's INIT by clearing without delivering to B
    h.capA.reset();

    // Trigger T1 retransmit on A
    UINT64 time = h.assocA.t1InitExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);
    ASSERT_GE(h.capA.count, (UINT32) 1);
    EXPECT_EQ(h.capA.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_INIT);

    // Deliver retransmitted INIT to B → B sends INIT-ACK
    h.exchangePackets();

    // A sends COOKIE-ECHO
    h.exchangePackets();

    // B sends COOKIE-ACK
    h.exchangePackets();

    // A becomes ESTABLISHED
    h.exchangePackets();

    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_ESTABLISHED);
}

// COOKIE-ECHO retransmit after drop → connection established
TEST_F(SctpAssocFunctionalityTest, handshake_cookieEchoRetransmitEstablishes)
{
    h.capA.reset();
    h.capB.reset();

    // A sends INIT
    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);

    // B also sends INIT (simultaneous open for simplicity)
    sctpAssocConnect(&h.assocB, mockOutboundCapture, (UINT64) &h.capB);

    // Exchange INITs → both send INIT-ACKs
    h.exchangePackets();

    // Exchange INIT-ACKs → both send COOKIE-ECHOs, state = COOKIE_ECHOED
    h.exchangePackets();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_COOKIE_ECHOED);

    // "Lose" both COOKIE-ECHOs
    h.capA.reset();
    h.capB.reset();

    // Trigger T1 retransmit on A
    UINT64 timeA = h.assocA.t1InitExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, timeA, mockOutboundCapture, (UINT64) &h.capA);
    ASSERT_GE(h.capA.count, (UINT32) 1);

    // Trigger T1 retransmit on B
    UINT64 timeB = h.assocB.t1InitExpiry + 1;
    sctpAssocCheckTimers(&h.assocB, timeB, mockOutboundCapture, (UINT64) &h.capB);

    // Exchange retransmitted COOKIE-ECHOs → COOKIE-ACKs
    h.exchangePackets();

    // Exchange COOKIE-ACKs → ESTABLISHED
    h.exchangePackets();

    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_ESTABLISHED);
}

// Data queued during handshake is delivered after ESTABLISHED
TEST_F(SctpAssocFunctionalityTest, handshake_dataDuringSetupDelivered)
{
    h.capA.reset();
    h.capB.reset();

    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    sctpAssocConnect(&h.assocB, mockOutboundCapture, (UINT64) &h.capB);

    // Queue data on A while in COOKIE_WAIT state
    BYTE payload[] = "queued during handshake";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 22, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_GE(h.assocA.sendQueueCount, (UINT32) 1);

    // Complete handshake
    h.exchangePackets(); // INITs → INIT-ACKs
    h.exchangePackets(); // INIT-ACKs → COOKIE-ECHOs
    h.exchangePackets(); // COOKIE-ECHOs → COOKIE-ACKs (queued data flushed)
    h.exchangePackets(); // COOKIE-ACKs + DATA → delivered + SACKs

    // More exchanges to ensure delivery
    h.exchangePackets();
    h.exchangePackets();

    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_ESTABLISHED);

    // Data should have been delivered to B
    BOOL found = FALSE;
    for (UINT32 i = 0; i < h.msgB.count; i++) {
        if (h.msgB.messages[i].payloadLen == 22 && MEMCMP(h.msgB.messages[i].payload, payload, 22) == 0) {
            found = TRUE;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// No DATA chunk sent before COOKIE-ACK → all queued
TEST_F(SctpAssocFunctionalityTest, handshake_noDataBeforeEstablished)
{
    h.capA.reset();
    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);

    // Queue data while in COOKIE_WAIT
    BYTE payload[] = "too early";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 9, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Verify no DATA chunk was sent
    EXPECT_EQ(h.capA.findChunkType(SCTP_CHUNK_DATA), -1);
    EXPECT_GE(h.assocA.sendQueueCount, (UINT32) 1);
}

// Max INIT retransmits → CLOSED
TEST_F(SctpAssocFunctionalityTest, handshake_initMaxRetransmitsCloses)
{
    h.capA.reset();
    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);

    // Drop all INITs, keep firing T1
    for (UINT32 i = 0; i <= SCTP_MAX_INIT_RETRANS + 1; i++) {
        h.capA.reset();
        if (h.assocA.t1InitExpiry == 0 || h.assocA.state == SCTP_ASSOC_CLOSED) {
            break;
        }
        UINT64 time = h.assocA.t1InitExpiry + 1;
        sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);
    }

    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_CLOSED);
}

// Max COOKIE-ECHO retransmits → CLOSED
TEST_F(SctpAssocFunctionalityTest, handshake_cookieEchoMaxRetransmitsCloses)
{
    h.capA.reset();
    h.capB.reset();

    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    sctpAssocConnect(&h.assocB, mockOutboundCapture, (UINT64) &h.capB);

    // Exchange INITs → INIT-ACKs
    h.exchangePackets();

    // Exchange INIT-ACKs → COOKIE-ECHOs
    h.exchangePackets();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_COOKIE_ECHOED);

    // Drop all COOKIE-ECHOs, keep firing T1 on A
    h.capA.reset();
    h.capB.reset();

    for (UINT32 i = 0; i <= SCTP_MAX_INIT_RETRANS + 1; i++) {
        h.capA.reset();
        if (h.assocA.t1InitExpiry == 0 || h.assocA.state == SCTP_ASSOC_CLOSED) {
            break;
        }
        UINT64 time = h.assocA.t1InitExpiry + 1;
        sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);
    }

    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_CLOSED);
}

// Retransmitted INIT has same verification tag and TSN
TEST_F(SctpAssocFunctionalityTest, initAck_retransmittedInitHasSameTag)
{
    h.capA.reset();
    sctpAssocConnect(&h.assocA, mockOutboundCapture, (UINT64) &h.capA);
    ASSERT_GE(h.capA.count, (UINT32) 1);

    // Parse first INIT: tag at common_header(12) + chunk_header(4) = offset 16
    UINT32 tag1 = (UINT32) getUnalignedInt32BigEndian((PINT32)(h.capA.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE));

    // Trigger T1 retransmit
    h.capA.reset();
    UINT64 time = h.assocA.t1InitExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);
    ASSERT_GE(h.capA.count, (UINT32) 1);

    UINT32 tag2 = (UINT32) getUnalignedInt32BigEndian((PINT32)(h.capA.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE));

    EXPECT_EQ(tag1, tag2);
}

/******************************************************************************
 * Retransmission Integration
 * Matches Rust: recover_on_last_retransmission, send_a_lot_of_bytes_missed_second_packet
 *****************************************************************************/

// Multiple T3 retransmissions before success
TEST_F(SctpAssocFunctionalityTest, retransmit_multipleRetriesBeforeSuccess)
{
    h.completeHandshake();

    BYTE payload[] = "persistent";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 10, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

    // Drop first 3 attempts
    for (int i = 0; i < 3; i++) {
        h.capA.reset();
        UINT64 time = h.assocA.t3RtxExpiry + 1;
        sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);
    }
    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 1);

    // Deliver the retransmitted DATA
    h.exchangePackets(); // DATA→B, B sends SACK
    h.exchangePackets(); // SACK→A

    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 0);
    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.msgB.messages[0].payloadLen, (UINT32) 10);
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, payload, 10));
}

// Fragmented message abandoned by PR-SCTP lifetime
TEST_F(SctpAssocFunctionalityTest, retransmit_fragmentedAbandonedByLifetime)
{
    h.completeHandshake();

    UINT32 maxDataPayload = h.assocA.mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;
    UINT32 bigLen = maxDataPayload * 2 + 10;
    BYTE* bigPayload = (BYTE*) MEMALLOC(bigLen);
    ASSERT_TRUE(bigPayload != NULL);
    MEMSET(bigPayload, 0xCC, bigLen);

    // Send with lifetime=100ms
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, bigPayload, bigLen, 0xFFFF, 100, mockOutboundNoop, 0);
    EXPECT_GE(h.assocA.outstandingCount, (UINT32) 3);

    // Drop all fragments, trigger T3 well past lifetime
    UINT64 time = h.assocA.t3RtxExpiry + 200;
    h.capA.reset();
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);

    // All fragments abandoned
    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 0);

    // FORWARD-TSN should have been sent
    EXPECT_NE(h.capA.findChunkType(SCTP_CHUNK_FORWARD_TSN), -1);

    MEMFREE(bigPayload);
}

/******************************************************************************
 * PR-SCTP: Multiple Messages, One Abandoned
 * Matches Rust: send_message_with_limited_rtx — msg1 + msg3 delivered, msg2 expired
 *****************************************************************************/

TEST_F(SctpAssocFunctionalityTest, prsctp_multipleMessagesOneAbandoned)
{
    h.completeHandshake();

    // Message 1: normal (delivered)
    BYTE payload1[] = "msg1";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload1, 4, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    h.exchangePackets(); // DATA→B, SACK
    h.exchangePackets(); // SACK→A

    ASSERT_GE(h.msgB.count, (UINT32) 1);
    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 0);

    // Message 2: limited retransmit (will be abandoned)
    BYTE payload2[] = "msg2-abandon";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload2, 12, 0, 0, mockOutboundCapture, (UINT64) &h.capA); // maxRetransmits=0

    // Drop msg2
    h.capA.reset();

    // Trigger T3 → abandon msg2 → FORWARD-TSN
    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);
    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 0);

    // Deliver FORWARD-TSN to B
    h.exchangePackets();
    h.exchangePackets();

    // Message 3: normal (should still be delivered)
    BYTE payload3[] = "msg3";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload3, 4, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    h.exchangePackets();
    h.exchangePackets();

    // msg1 and msg3 delivered, msg2 abandoned
    BOOL foundMsg1 = FALSE, foundMsg3 = FALSE;
    for (UINT32 i = 0; i < h.msgB.count; i++) {
        if (h.msgB.messages[i].payloadLen == 4 && MEMCMP(h.msgB.messages[i].payload, payload1, 4) == 0) {
            foundMsg1 = TRUE;
        }
        if (h.msgB.messages[i].payloadLen == 4 && MEMCMP(h.msgB.messages[i].payload, payload3, 4) == 0) {
            foundMsg3 = TRUE;
        }
    }
    EXPECT_TRUE(foundMsg1);
    EXPECT_TRUE(foundMsg3);
}

/******************************************************************************
 * Advanced Integration
 * Matches Rust: send_many_messages, send_many_api_method
 *****************************************************************************/

// 100 messages all delivered with correct payload
TEST_F(SctpAssocFunctionalityTest, advanced_100MessagesIntegrity)
{
    h.completeHandshake();

    for (int i = 0; i < 100; i++) {
        BYTE payload[8];
        payload[0] = (BYTE)(i >> 24);
        payload[1] = (BYTE)(i >> 16);
        payload[2] = (BYTE)(i >> 8);
        payload[3] = (BYTE)(i);
        payload[4] = (BYTE) ~payload[0];
        payload[5] = (BYTE) ~payload[1];
        payload[6] = (BYTE) ~payload[2];
        payload[7] = (BYTE) ~payload[3];

        sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, payload, 8, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

        // Exchange every 10 messages to prevent buffer overflow
        if ((i + 1) % 10 == 0) {
            h.exchangePackets();
            h.exchangePackets();
        }
    }

    // Final exchanges
    h.exchangePackets();
    h.exchangePackets();
    h.exchangePackets();

    EXPECT_EQ(h.msgB.count, (UINT32) 100);

    // Verify first and last message integrity
    BYTE first[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[0].payload, first, 8));

    BYTE last[8] = {0, 0, 0, 99, 0xFF, 0xFF, 0xFF, 0x9C};
    EXPECT_EQ(0, MEMCMP(h.msgB.messages[99].payload, last, 8));
}

// Multiple large fragmented messages all reassembled correctly
TEST_F(SctpAssocFunctionalityTest, advanced_fragmentedMultipleMessages)
{
    h.completeHandshake();

    UINT32 maxDataPayload = h.assocA.mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;
    UINT32 bigLen = maxDataPayload + 50; // 2 fragments each

    for (int i = 0; i < 5; i++) {
        BYTE* bigPayload = (BYTE*) MEMALLOC(bigLen);
        ASSERT_TRUE(bigPayload != NULL);
        MEMSET(bigPayload, (BYTE)(0x10 + i), bigLen);

        sctpAssocSend(&h.assocA, 0, SCTP_PPID_BINARY, FALSE, bigPayload, bigLen, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);

        // Exchange after each message to ensure delivery
        h.exchangePackets();
        h.exchangePackets();
        h.exchangePackets();

        MEMFREE(bigPayload);
    }

    ASSERT_EQ(h.msgB.count, (UINT32) 5);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(h.msgB.messages[i].payloadLen, bigLen);
        EXPECT_EQ(h.msgB.messages[i].payload[0], (BYTE)(0x10 + i));
        EXPECT_EQ(h.msgB.messages[i].payload[bigLen - 1], (BYTE)(0x10 + i));
    }
}

// Advancing time with no outstanding → no spurious packets
TEST_F(SctpAssocFunctionalityTest, advanced_idleTimeAdvanceNoPackets)
{
    h.completeHandshake();

    // Send and receive one message to verify everything works
    BYTE payload[] = "warmup";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 6, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    h.exchangePackets();
    h.exchangePackets();
    EXPECT_EQ(h.assocA.outstandingCount, (UINT32) 0);

    // Advance time significantly — should not generate any packets
    h.capA.reset();
    h.capB.reset();
    sctpAssocCheckTimers(&h.assocA, 999999999, mockOutboundCapture, (UINT64) &h.capA);
    sctpAssocCheckTimers(&h.assocB, 999999999, mockOutboundCapture, (UINT64) &h.capB);

    EXPECT_EQ(h.capA.count, (UINT32) 0);
    EXPECT_EQ(h.capB.count, (UINT32) 0);
}

// Bidirectional heartbeat: both sides respond
TEST_F(SctpAssocFunctionalityTest, heartbeat_bothSidesRespond)
{
    h.completeHandshake();

    // Send HEARTBEAT to A
    BYTE hbPacketA[128];
    UINT32 off = sctpWriteCommonHeader(hbPacketA, 5000, 5000, h.assocA.myVerificationTag);
    BYTE hbDataA[] = {0x01, 0x02, 0x03, 0x04};
    hbPacketA[off] = SCTP_CHUNK_HEARTBEAT;
    hbPacketA[off + 1] = 0;
    putUnalignedInt16BigEndian(hbPacketA + off + 2, (INT16)(SCTP_CHUNK_HEADER_SIZE + 4));
    MEMCPY(hbPacketA + off + SCTP_CHUNK_HEADER_SIZE, hbDataA, 4);
    off += SCTP_PAD4(SCTP_CHUNK_HEADER_SIZE + 4);
    sctpFinalizePacket(hbPacketA, off);

    h.capA.reset();
    sctpAssocHandlePacket(&h.assocA, hbPacketA, off, mockOutboundCapture, (UINT64) &h.capA, mockMessageNoop, 0);
    ASSERT_GE(h.capA.count, (UINT32) 1);
    EXPECT_EQ(h.capA.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_HEARTBEAT_ACK);

    // Send HEARTBEAT to B
    BYTE hbPacketB[128];
    off = sctpWriteCommonHeader(hbPacketB, 5000, 5000, h.assocB.myVerificationTag);
    BYTE hbDataB[] = {0x05, 0x06, 0x07, 0x08};
    hbPacketB[off] = SCTP_CHUNK_HEARTBEAT;
    hbPacketB[off + 1] = 0;
    putUnalignedInt16BigEndian(hbPacketB + off + 2, (INT16)(SCTP_CHUNK_HEADER_SIZE + 4));
    MEMCPY(hbPacketB + off + SCTP_CHUNK_HEADER_SIZE, hbDataB, 4);
    off += SCTP_PAD4(SCTP_CHUNK_HEADER_SIZE + 4);
    sctpFinalizePacket(hbPacketB, off);

    h.capB.reset();
    sctpAssocHandlePacket(&h.assocB, hbPacketB, off, mockOutboundCapture, (UINT64) &h.capB, mockMessageNoop, 0);
    ASSERT_GE(h.capB.count, (UINT32) 1);
    EXPECT_EQ(h.capB.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_HEARTBEAT_ACK);

    // Verify echoed data matches
    PBYTE pHbAckA = h.capA.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE;
    EXPECT_EQ(0, MEMCMP(pHbAckA, hbDataA, 4));
    PBYTE pHbAckB = h.capB.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE;
    EXPECT_EQ(0, MEMCMP(pHbAckB, hbDataB, 4));
}

// Full SHUTDOWN sequence after data transfer
TEST_F(SctpAssocFunctionalityTest, shutdown_fullSequenceAfterData)
{
    h.completeHandshake();

    // Send some data both ways
    BYTE payA[] = "from A";
    BYTE payB[] = "from B";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payA, 6, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capA);
    sctpAssocSend(&h.assocB, 0, SCTP_PPID_STRING, FALSE, payB, 6, 0xFFFF, 0, mockOutboundCapture, (UINT64) &h.capB);
    h.exchangePackets();
    h.exchangePackets();

    EXPECT_GE(h.msgA.count, (UINT32) 1);
    EXPECT_GE(h.msgB.count, (UINT32) 1);

    // Graceful shutdown initiated by B
    sctpAssocShutdown(&h.assocB, mockOutboundCapture, (UINT64) &h.capB);
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_SHUTDOWN_SENT);

    // SHUTDOWN→A → SHUTDOWN-ACK
    h.exchangePackets();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_SHUTDOWN_ACK_SENT);

    // SHUTDOWN-ACK→B → SHUTDOWN-COMPLETE
    h.exchangePackets();
    EXPECT_EQ(h.assocB.state, SCTP_ASSOC_CLOSED);

    // SHUTDOWN-COMPLETE→A → CLOSED
    h.exchangePackets();
    EXPECT_EQ(h.assocA.state, SCTP_ASSOC_CLOSED);
}

// Congestion window grows back after loss recovery
TEST_F(SctpAssocFunctionalityTest, congestion_cwndRecoveryAfterLoss)
{
    h.completeHandshake();

    BYTE payload[] = "hello";
    sctpAssocSend(&h.assocA, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    // T3 timeout → cwnd reset to MTU
    UINT64 time = h.assocA.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&h.assocA, time, mockOutboundCapture, (UINT64) &h.capA);
    UINT32 cwndAfterLoss = h.assocA.cwnd;
    EXPECT_EQ(cwndAfterLoss, (UINT32) h.assocA.mtu);

    // Deliver retransmit → SACK → cwnd grows
    h.exchangePackets();
    h.exchangePackets();
    EXPECT_GT(h.assocA.cwnd, cwndAfterLoss);
}

/******************************************************************************
 * SctpSession-level retransmission via sctpSessionTickTimers
 *
 * The existing retransmit_* tests call sctpAssocCheckTimers() directly with
 * a fake timestamp — so they never exercise the real timer path.  In
 * production, sctpAssocCheckTimers is driven by sctpSessionTickTimers()
 * which is invoked from a periodic timer callback.  This test verifies that
 * sctpSessionTickTimers triggers retransmission when no incoming packets
 * arrive (the exact scenario that caused the Android CI flood-send failure).
 *****************************************************************************/

// Capture callbacks compatible with SctpSession callback signatures
struct SessionMessageCapture {
    UINT32 count;
    BOOL lastIsBinary;
    BYTE lastPayload[4096];
    UINT32 lastPayloadLen;

    void reset()
    {
        count = 0;
        lastIsBinary = FALSE;
        lastPayloadLen = 0;
    }
};

static PacketCapture gSessionCapA;
static PacketCapture gSessionCapB;
static SessionMessageCapture gSessionMsgA;
static SessionMessageCapture gSessionMsgB;

static VOID sessionOutboundCaptureA(UINT64 customData, PBYTE pPacket, UINT32 packetLen)
{
    UNUSED_PARAM(customData);
    if (gSessionCapA.count < SCTP_TEST_MAX_PACKETS && packetLen <= SCTP_TEST_MAX_PKT_LEN) {
        MEMCPY(gSessionCapA.packets[gSessionCapA.count].data, pPacket, packetLen);
        gSessionCapA.packets[gSessionCapA.count].len = packetLen;
        gSessionCapA.count++;
    }
}

static VOID sessionOutboundCaptureB(UINT64 customData, PBYTE pPacket, UINT32 packetLen)
{
    UNUSED_PARAM(customData);
    if (gSessionCapB.count < SCTP_TEST_MAX_PACKETS && packetLen <= SCTP_TEST_MAX_PKT_LEN) {
        MEMCPY(gSessionCapB.packets[gSessionCapB.count].data, pPacket, packetLen);
        gSessionCapB.packets[gSessionCapB.count].len = packetLen;
        gSessionCapB.count++;
    }
}

static VOID sessionMessageCaptureA(UINT64 customData, UINT32 streamId, BOOL isBinary, PBYTE pMsg, UINT32 msgLen)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(streamId);
    gSessionMsgA.lastIsBinary = isBinary;
    if (msgLen <= sizeof(gSessionMsgA.lastPayload)) {
        MEMCPY(gSessionMsgA.lastPayload, pMsg, msgLen);
    }
    gSessionMsgA.lastPayloadLen = msgLen;
    gSessionMsgA.count++;
}

static VOID sessionMessageCaptureB(UINT64 customData, UINT32 streamId, BOOL isBinary, PBYTE pMsg, UINT32 msgLen)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(streamId);
    gSessionMsgB.lastIsBinary = isBinary;
    if (msgLen <= sizeof(gSessionMsgB.lastPayload)) {
        MEMCPY(gSessionMsgB.lastPayload, pMsg, msgLen);
    }
    gSessionMsgB.lastPayloadLen = msgLen;
    gSessionMsgB.count++;
}

static VOID sessionOpenNoop(UINT64 customData, UINT32 channelId, PBYTE pName, UINT32 nameLen)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(channelId);
    UNUSED_PARAM(pName);
    UNUSED_PARAM(nameLen);
}

// Deliver captured packets from one session to another
static UINT32 deliverSessionPackets(PacketCapture* src, PSctpSession dst)
{
    UINT32 delivered = 0;
    for (UINT32 i = 0; i < src->count; i++) {
        putSctpPacket(dst, src->packets[i].data, src->packets[i].len);
        delivered++;
    }
    src->reset();
    return delivered;
}

// Exchange all pending packets between two sessions
static UINT32 exchangeSessionPackets(PSctpSession a, PSctpSession b)
{
    // Snapshot and reset captures to avoid re-delivering packets
    // generated during this exchange
    PacketCapture tmpA, tmpB;
    MEMCPY(&tmpA, &gSessionCapA, sizeof(PacketCapture));
    MEMCPY(&tmpB, &gSessionCapB, sizeof(PacketCapture));
    gSessionCapA.reset();
    gSessionCapB.reset();

    UINT32 total = 0;
    // A's outbound → B's inbound
    for (UINT32 i = 0; i < tmpA.count; i++) {
        putSctpPacket(b, tmpA.packets[i].data, tmpA.packets[i].len);
        total++;
    }
    // B's outbound → A's inbound
    for (UINT32 i = 0; i < tmpB.count; i++) {
        putSctpPacket(a, tmpB.packets[i].data, tmpB.packets[i].len);
        total++;
    }
    return total;
}

TEST_F(SctpAssocFunctionalityTest, retransmit_sessionTickTimersDrivesRetransmission)
{
    PSctpSession sessionA = NULL;
    PSctpSession sessionB = NULL;

    gSessionCapA.reset();
    gSessionCapB.reset();
    gSessionMsgA.reset();
    gSessionMsgB.reset();

    SctpSessionCallbacks cbA;
    cbA.customData = 0;
    cbA.outboundPacketFunc = sessionOutboundCaptureA;
    cbA.dataChannelMessageFunc = sessionMessageCaptureA;
    cbA.dataChannelOpenFunc = sessionOpenNoop;

    SctpSessionCallbacks cbB;
    cbB.customData = 0;
    cbB.outboundPacketFunc = sessionOutboundCaptureB;
    cbB.dataChannelMessageFunc = sessionMessageCaptureB;
    cbB.dataChannelOpenFunc = sessionOpenNoop;

    // Create both sessions — each immediately sends INIT
    EXPECT_EQ(createSctpSession(&cbA, &sessionA), STATUS_SUCCESS);
    EXPECT_EQ(createSctpSession(&cbB, &sessionB), STATUS_SUCCESS);
    ASSERT_TRUE(sessionA != NULL);
    ASSERT_TRUE(sessionB != NULL);

    // Complete handshake by exchanging packets until both ESTABLISHED
    for (int round = 0; round < 8; round++) {
        if (sessionA->assoc.state == SCTP_ASSOC_ESTABLISHED && sessionB->assoc.state == SCTP_ASSOC_ESTABLISHED) {
            break;
        }
        exchangeSessionPackets(sessionA, sessionB);
    }
    ASSERT_EQ(sessionA->assoc.state, SCTP_ASSOC_ESTABLISHED);
    ASSERT_EQ(sessionB->assoc.state, SCTP_ASSOC_ESTABLISHED);

    // Clear captures after handshake
    gSessionCapA.reset();
    gSessionCapB.reset();

    // Shorten RTO so the test doesn't need to sleep 10+ seconds.
    // SCTP_NOW_MS() uses units of 10ms, so rtoMs=10 → 100ms real time.
    sessionA->assoc.rtoMs = 10;

    // Send a message from A — it will be captured in gSessionCapA
    BYTE payload[] = "retransmit me";
    EXPECT_EQ(sctpSessionWriteMessage(sessionA, 0, FALSE, payload, 13), STATUS_SUCCESS);
    ASSERT_GT(gSessionCapA.count, (UINT32) 0);

    // "Lose" the DATA packet — don't deliver to B
    gSessionCapA.reset();

    // Verify B has NOT received the message
    EXPECT_EQ(gSessionMsgB.count, (UINT32) 0);

    // T3-rtx is armed on A, but no packets arrive to trigger putSctpPacket.
    // Without sctpSessionTickTimers, the message is lost forever.
    // Sleep past the T3 expiry (rtoMs=10 → 100ms real time, sleep 200ms).
    THREAD_SLEEP(200 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);

    // Drive the timer via the same function the periodic callback uses
    EXPECT_EQ(sctpSessionTickTimers(sessionA), STATUS_SUCCESS);

    // A should have retransmitted the DATA
    ASSERT_GT(gSessionCapA.count, (UINT32) 0);

    // Deliver the retransmitted DATA to B
    deliverSessionPackets(&gSessionCapA, sessionB);
    // Deliver B's SACK back to A
    deliverSessionPackets(&gSessionCapB, sessionA);

    // B should have received the message
    ASSERT_EQ(gSessionMsgB.count, (UINT32) 1);
    EXPECT_EQ(gSessionMsgB.lastPayloadLen, (UINT32) 13);
    EXPECT_EQ(0, MEMCMP(gSessionMsgB.lastPayload, payload, 13));

    freeSctpSession(&sessionA);
    freeSctpSession(&sessionB);
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
#endif
