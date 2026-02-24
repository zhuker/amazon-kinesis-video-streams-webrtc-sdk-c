#include "SctpTestHelpers.h"

#ifdef ENABLE_DATA_CHANNEL
namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

class SctpAssocApiTest : public WebRtcClientTestBase {
  protected:
    SctpAssociation assoc;
    PacketCapture cap;
    MessageCapture msg;

    void SetUp() override
    {
        WebRtcClientTestBase::SetUp();
        cap.reset();
        msg.reset();
        sctpAssocInit(&assoc, 5000, 5000, 1188);
    }

    void TearDown() override
    {
        sctpAssocCleanup(&assoc);
        WebRtcClientTestBase::TearDown();
    }

    // Helper: drive assoc through the full handshake with a simulated peer
    void driveToEstablished()
    {
        cap.reset();
        // Connect sends INIT
        sctpAssocConnect(&assoc, mockOutboundCapture, (UINT64) &cap);
        ASSERT_GE(cap.count, (UINT32) 1);

        // Simulate peer sending INIT-ACK with cookie
        SctpCookie cookie;
        MEMSET(&cookie, 0, sizeof(cookie));
        cookie.magic1 = SCTP_COOKIE_MAGIC1;
        cookie.magic2 = SCTP_COOKIE_MAGIC2;
        cookie.peerTag = assoc.myVerificationTag;
        cookie.myTag = 0xBBBBBBBB;
        cookie.peerInitialTsn = assoc.nextTsn;
        cookie.myInitialTsn = 0xBBBBBBBB;
        cookie.peerArwnd = SCTP_DEFAULT_ARWND;

        BYTE peerPacket[256];
        UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
        off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, SCTP_DEFAULT_ARWND, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie,
                                     SCTP_COOKIE_SIZE);
        sctpFinalizePacket(peerPacket, off);

        cap.reset();
        sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);
        // Should have sent COOKIE-ECHO
        ASSERT_EQ(assoc.state, SCTP_ASSOC_COOKIE_ECHOED);

        // Simulate COOKIE-ACK from peer
        BYTE cookieAckPacket[64];
        off = sctpWriteCommonHeader(cookieAckPacket, 5000, 5000, assoc.myVerificationTag);
        off += sctpWriteCookieAckChunk(cookieAckPacket + off);
        sctpFinalizePacket(cookieAckPacket, off);

        cap.reset();
        sctpAssocHandlePacket(&assoc, cookieAckPacket, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);
        ASSERT_EQ(assoc.state, SCTP_ASSOC_ESTABLISHED);
        cap.reset();
    }

    // Simulate sending a SACK from peer that acks up to cumTsn
    void sendPeerSack(UINT32 cumTsn)
    {
        BYTE packet[64];
        UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, assoc.myVerificationTag);
        off += sctpWriteSackChunk(packet + off, cumTsn, SCTP_DEFAULT_ARWND, NULL, NULL, 0);
        sctpFinalizePacket(packet, off);
        sctpAssocHandlePacket(&assoc, packet, off, mockOutboundCapture, (UINT64) &cap, mockMessageCapture, (UINT64) &msg);
    }

    // Simulate sending a SACK with gap blocks from peer
    void sendPeerSackWithGaps(UINT32 cumTsn, PUINT16 gapStarts, PUINT16 gapEnds, UINT16 numGaps)
    {
        BYTE packet[256];
        UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, assoc.myVerificationTag);
        off += sctpWriteSackChunk(packet + off, cumTsn, SCTP_DEFAULT_ARWND, gapStarts, gapEnds, numGaps);
        sctpFinalizePacket(packet, off);
        sctpAssocHandlePacket(&assoc, packet, off, mockOutboundCapture, (UINT64) &cap, mockMessageCapture, (UINT64) &msg);
    }

    // Inject a DATA packet from the simulated peer
    void sendPeerData(UINT32 tsn, UINT16 sid = 0, UINT16 ssn = 0, BOOL unordered = TRUE)
    {
        BYTE payload[] = "x";
        BYTE packet[256];
        UINT32 len = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, sid, ssn, SCTP_PPID_BINARY, unordered, payload, 1);
        sctpAssocHandlePacket(&assoc, packet, len, mockOutboundCapture, (UINT64) &cap, mockMessageCapture, (UINT64) &msg);
    }

    // Inject a FORWARD-TSN packet from the simulated peer
    void sendPeerForwardTsn(UINT32 newCumTsn)
    {
        BYTE packet[64];
        UINT32 len = buildForwardTsnPacket(packet, 5000, 5000, assoc.myVerificationTag, newCumTsn);
        sctpAssocHandlePacket(&assoc, packet, len, mockOutboundCapture, (UINT64) &cap, mockMessageCapture, (UINT64) &msg);
    }

    // Parse SACK gap blocks from a captured outbound packet
    struct SackInfo {
        UINT32 cumTsn;
        UINT32 arwnd;
        UINT16 numGaps;
        UINT16 gapStarts[128];
        UINT16 gapEnds[128];
    };

    bool parseSack(const PacketCapture& capture, SackInfo& info)
    {
        INT32 idx = ((PacketCapture&) capture).findChunkType(SCTP_CHUNK_SACK);
        if (idx < 0) {
            return false;
        }
        PBYTE data = (PBYTE) capture.packets[idx].data + SCTP_COMMON_HEADER_SIZE;
        // SACK chunk layout: type(1) flags(1) length(2) cumTsn(4) arwnd(4) numGaps(2) numDups(2) gapBlocks...
        info.cumTsn = (UINT32) getUnalignedInt32BigEndian((PINT32)(data + 4));
        info.arwnd = (UINT32) getUnalignedInt32BigEndian((PINT32)(data + 8));
        info.numGaps = (UINT16) getUnalignedInt16BigEndian((PINT16)(data + 12));
        for (UINT16 i = 0; i < info.numGaps && i < 128; i++) {
            info.gapStarts[i] = (UINT16) getUnalignedInt16BigEndian((PINT16)(data + 16 + i * 4));
            info.gapEnds[i] = (UINT16) getUnalignedInt16BigEndian((PINT16)(data + 18 + i * 4));
        }
        return true;
    }

    // Find the LAST SACK in capture (when multiple packets are captured)
    bool parseLastSack(const PacketCapture& capture, SackInfo& info)
    {
        INT32 lastIdx = -1;
        for (UINT32 i = 0; i < capture.count; i++) {
            if (capture.packets[i].len > SCTP_COMMON_HEADER_SIZE && capture.packets[i].data[SCTP_COMMON_HEADER_SIZE] == SCTP_CHUNK_SACK) {
                lastIdx = (INT32) i;
            }
        }
        if (lastIdx < 0) {
            return false;
        }
        PBYTE data = (PBYTE) capture.packets[lastIdx].data + SCTP_COMMON_HEADER_SIZE;
        info.cumTsn = (UINT32) getUnalignedInt32BigEndian((PINT32)(data + 4));
        info.arwnd = (UINT32) getUnalignedInt32BigEndian((PINT32)(data + 8));
        info.numGaps = (UINT16) getUnalignedInt16BigEndian((PINT16)(data + 12));
        for (UINT16 i = 0; i < info.numGaps && i < 128; i++) {
            info.gapStarts[i] = (UINT16) getUnalignedInt16BigEndian((PINT16)(data + 16 + i * 4));
            info.gapEnds[i] = (UINT16) getUnalignedInt16BigEndian((PINT16)(data + 18 + i * 4));
        }
        return true;
    }
};

/******************************************************************************
 * Initialization
 *****************************************************************************/

TEST_F(SctpAssocApiTest, init_stateIsClosed)
{
    EXPECT_EQ(assoc.state, SCTP_ASSOC_CLOSED);
}

TEST_F(SctpAssocApiTest, init_portsSet)
{
    EXPECT_EQ(assoc.localPort, 5000);
    EXPECT_EQ(assoc.remotePort, 5000);
}

TEST_F(SctpAssocApiTest, init_mtuSet)
{
    EXPECT_EQ(assoc.mtu, 1188);
}

TEST_F(SctpAssocApiTest, init_tagNonZero)
{
    EXPECT_NE(assoc.myVerificationTag, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, init_initialTsnMatchesTag)
{
    EXPECT_EQ(assoc.nextTsn, assoc.myVerificationTag);
}

TEST_F(SctpAssocApiTest, init_cwndIsInitialCwnd)
{
    EXPECT_EQ(assoc.cwnd, (UINT32)(SCTP_INITIAL_CWND_MTUS * 1188));
}

TEST_F(SctpAssocApiTest, init_ssthreshIsDefaultArwnd)
{
    EXPECT_EQ(assoc.ssthresh, (UINT32) SCTP_DEFAULT_ARWND);
}

TEST_F(SctpAssocApiTest, init_outstandingCountZero)
{
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
}

/******************************************************************************
 * Connect
 *****************************************************************************/

TEST_F(SctpAssocApiTest, connect_sendsInitPacket)
{
    cap.reset();
    sctpAssocConnect(&assoc, mockOutboundCapture, (UINT64) &cap);
    EXPECT_EQ(cap.count, (UINT32) 1);
}

TEST_F(SctpAssocApiTest, connect_initPacketHasVtagZero)
{
    cap.reset();
    sctpAssocConnect(&assoc, mockOutboundCapture, (UINT64) &cap);
    ASSERT_GE(cap.count, (UINT32) 1);

    UINT32 vtag = (UINT32) getUnalignedInt32BigEndian((PINT32)(cap.packets[0].data + 4));
    EXPECT_EQ(vtag, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, connect_stateCookieWait)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_COOKIE_WAIT);
}

TEST_F(SctpAssocApiTest, connect_initChunkPresent)
{
    cap.reset();
    sctpAssocConnect(&assoc, mockOutboundCapture, (UINT64) &cap);
    ASSERT_GE(cap.count, (UINT32) 1);

    EXPECT_EQ(cap.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_INIT);
}

TEST_F(SctpAssocApiTest, connect_initTagMatchesMyTag)
{
    cap.reset();
    sctpAssocConnect(&assoc, mockOutboundCapture, (UINT64) &cap);
    ASSERT_GE(cap.count, (UINT32) 1);

    // INIT chunk: initiate tag at offset 12+4=16 (common header + chunk header)
    UINT32 initTag = (UINT32) getUnalignedInt32BigEndian((PINT32)(cap.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE));
    EXPECT_EQ(initTag, assoc.myVerificationTag);
}

TEST_F(SctpAssocApiTest, connect_t1TimerStarted)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);
    EXPECT_GT(assoc.t1InitExpiry, (UINT64) 0);
}

/******************************************************************************
 * Handle INIT
 *****************************************************************************/

TEST_F(SctpAssocApiTest, handleInit_sendsInitAck)
{
    // Build an INIT packet from a peer
    BYTE peerInit[256];
    UINT32 off = sctpWriteCommonHeader(peerInit, 5000, 5000, 0); // vtag=0 for INIT
    off += sctpWriteInitChunk(peerInit + off, 0xAAAAAAAA, 131072, 300, 300, 0xAAAAAAAA);
    sctpFinalizePacket(peerInit, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerInit, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    // Should have sent INIT-ACK
    ASSERT_GE(cap.count, (UINT32) 1);
    EXPECT_EQ(cap.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_INIT_ACK);
}

TEST_F(SctpAssocApiTest, handleInit_initAckContainsCookie)
{
    BYTE peerInit[256];
    UINT32 off = sctpWriteCommonHeader(peerInit, 5000, 5000, 0);
    off += sctpWriteInitChunk(peerInit + off, 0xAAAAAAAA, 131072, 300, 300, 0xAAAAAAAA);
    sctpFinalizePacket(peerInit, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerInit, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    // Parse the INIT-ACK to find the cookie
    ASSERT_GE(cap.count, (UINT32) 1);
    // INIT-ACK value starts at offset 12 (common header) + 4 (chunk header)
    PBYTE pInitAckValue = cap.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE;
    // State Cookie parameter at offset 16 into the value (after initTag/arwnd/os/is/tsn)
    UINT16 paramType = (UINT16) getUnalignedInt16BigEndian((PINT16)(pInitAckValue + 16));
    EXPECT_EQ(paramType, (UINT16) SCTP_PARAM_STATE_COOKIE);
}

TEST_F(SctpAssocApiTest, handleInit_cookieMagicValid)
{
    BYTE peerInit[256];
    UINT32 off = sctpWriteCommonHeader(peerInit, 5000, 5000, 0);
    off += sctpWriteInitChunk(peerInit + off, 0xAAAAAAAA, 131072, 300, 300, 0xAAAAAAAA);
    sctpFinalizePacket(peerInit, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerInit, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    ASSERT_GE(cap.count, (UINT32) 1);
    // Cookie data at offset: common_header(12) + chunk_header(4) + init_body(16) + param_header(4)
    PBYTE pCookie = cap.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE + 16 + 4;
    SctpCookie* cookie = (SctpCookie*) pCookie;
    EXPECT_EQ(cookie->magic1, (UINT32) SCTP_COOKIE_MAGIC1);
    EXPECT_EQ(cookie->magic2, (UINT32) SCTP_COOKIE_MAGIC2);
}

TEST_F(SctpAssocApiTest, handleInit_cookieContainsPeerTag)
{
    BYTE peerInit[256];
    UINT32 off = sctpWriteCommonHeader(peerInit, 5000, 5000, 0);
    off += sctpWriteInitChunk(peerInit + off, 0xAAAAAAAA, 131072, 300, 300, 0xAAAAAAAA);
    sctpFinalizePacket(peerInit, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerInit, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    ASSERT_GE(cap.count, (UINT32) 1);
    PBYTE pCookie = cap.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE + 16 + 4;
    SctpCookie* cookie = (SctpCookie*) pCookie;
    EXPECT_EQ(cookie->peerTag, (UINT32) 0xAAAAAAAA);
}

TEST_F(SctpAssocApiTest, handleInit_initAckVtagIsPeerTag)
{
    BYTE peerInit[256];
    UINT32 off = sctpWriteCommonHeader(peerInit, 5000, 5000, 0);
    off += sctpWriteInitChunk(peerInit + off, 0xAAAAAAAA, 131072, 300, 300, 0xAAAAAAAA);
    sctpFinalizePacket(peerInit, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerInit, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    ASSERT_GE(cap.count, (UINT32) 1);
    UINT32 vtag = (UINT32) getUnalignedInt32BigEndian((PINT32)(cap.packets[0].data + 4));
    EXPECT_EQ(vtag, (UINT32) 0xAAAAAAAA);
}

TEST_F(SctpAssocApiTest, handleInit_malformedIgnored)
{
    BYTE peerInit[64];
    UINT32 off = sctpWriteCommonHeader(peerInit, 5000, 5000, 0);
    // Write a truncated INIT chunk with too-short value
    peerInit[off] = SCTP_CHUNK_INIT;
    peerInit[off + 1] = 0;
    putUnalignedInt16BigEndian(peerInit + off + 2, (INT16) 8); // only 4 bytes of value (need 16)
    off += 8;
    sctpFinalizePacket(peerInit, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerInit, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);
    // No INIT-ACK should be sent
    EXPECT_EQ(cap.count, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, handleInit_stateRemainsUnchanged)
{
    BYTE peerInit[256];
    UINT32 off = sctpWriteCommonHeader(peerInit, 5000, 5000, 0);
    off += sctpWriteInitChunk(peerInit + off, 0xAAAAAAAA, 131072, 300, 300, 0xAAAAAAAA);
    sctpFinalizePacket(peerInit, off);

    SctpAssocState stateBefore = assoc.state;
    sctpAssocHandlePacket(&assoc, peerInit, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    // State should not change (stateless INIT response)
    EXPECT_EQ(assoc.state, stateBefore);
}

/******************************************************************************
 * Handle INIT-ACK
 *****************************************************************************/

TEST_F(SctpAssocApiTest, handleInitAck_sendsCookieEcho)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = assoc.myVerificationTag;
    cookie.myTag = 0xBBBBBBBB;
    cookie.peerInitialTsn = 0xBBBBBBBB;
    cookie.myInitialTsn = assoc.nextTsn;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, 131072, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    ASSERT_GE(cap.count, (UINT32) 1);
    EXPECT_EQ(cap.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_COOKIE_ECHO);
}

TEST_F(SctpAssocApiTest, handleInitAck_stateCookieEchoed)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, 131072, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_COOKIE_ECHOED);
}

TEST_F(SctpAssocApiTest, handleInitAck_peerTagStored)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xCCCCCCCC, 131072, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerVerificationTag, (UINT32) 0xCCCCCCCC);
}

TEST_F(SctpAssocApiTest, handleInitAck_peerArwndStored)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, 65536, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerArwnd, (UINT32) 65536);
}

TEST_F(SctpAssocApiTest, handleInitAck_peerCumTsnSet)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, 131072, 300, 300, 0x00001000, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerCumulativeTsn, (UINT32) 0x00001000 - 1);
    EXPECT_TRUE(assoc.peerCumulativeTsnValid);
}

TEST_F(SctpAssocApiTest, handleInitAck_cookieEchoDataStored)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = 0x42424242;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, 131072, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_TRUE(assoc.cookieEchoValid);

    SctpCookie* storedCookie = (SctpCookie*) assoc.cookieEchoData;
    EXPECT_EQ(storedCookie->peerTag, (UINT32) 0x42424242);
}

TEST_F(SctpAssocApiTest, handleInitAck_ignoredInWrongState)
{
    // Don't call connect — assoc is in CLOSED state, but code allows CLOSED too
    // Put in ESTABLISHED state
    driveToEstablished();

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, 131072, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);
    // Should not send COOKIE-ECHO (already established)
    EXPECT_EQ(cap.findChunkType(SCTP_CHUNK_COOKIE_ECHO), -1);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_ESTABLISHED);
}

/******************************************************************************
 * Handle COOKIE-ECHO
 *****************************************************************************/

TEST_F(SctpAssocApiTest, handleCookieEcho_sendsCookieAck)
{
    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = 0xAAAAAAAA;
    cookie.myTag = assoc.myVerificationTag;
    cookie.peerInitialTsn = 0xAAAAAAAA;
    cookie.myInitialTsn = assoc.nextTsn;
    cookie.peerArwnd = SCTP_DEFAULT_ARWND;

    BYTE peerPacket[128];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieEchoChunk(peerPacket + off, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_COOKIE_ACK), -1);
}

TEST_F(SctpAssocApiTest, handleCookieEcho_stateEstablished)
{
    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = 0xAAAAAAAA;
    cookie.peerArwnd = SCTP_DEFAULT_ARWND;

    BYTE peerPacket[128];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieEchoChunk(peerPacket + off, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_ESTABLISHED);
}

TEST_F(SctpAssocApiTest, handleCookieEcho_peerTagFromCookie)
{
    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = 0xDDDDDDDD;
    cookie.peerArwnd = SCTP_DEFAULT_ARWND;

    BYTE peerPacket[128];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieEchoChunk(peerPacket + off, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerVerificationTag, (UINT32) 0xDDDDDDDD);
}

TEST_F(SctpAssocApiTest, handleCookieEcho_t1TimerStopped)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);
    EXPECT_GT(assoc.t1InitExpiry, (UINT64) 0);

    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = 0xAAAAAAAA;
    cookie.peerArwnd = SCTP_DEFAULT_ARWND;

    BYTE peerPacket[128];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieEchoChunk(peerPacket + off, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.t1InitExpiry, (UINT64) 0);
}

TEST_F(SctpAssocApiTest, handleCookieEcho_invalidMagicIgnored)
{
    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = 0xBADBAD;
    cookie.magic2 = 0xBADBAD;

    BYTE peerPacket[128];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieEchoChunk(peerPacket + off, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);
    EXPECT_NE(assoc.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(cap.findChunkType(SCTP_CHUNK_COOKIE_ACK), -1);
}

TEST_F(SctpAssocApiTest, handleCookieEcho_tooSmallIgnored)
{
    BYTE smallCookie[8] = {0};

    BYTE peerPacket[64];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieEchoChunk(peerPacket + off, smallCookie, 8);
    sctpFinalizePacket(peerPacket, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);
    EXPECT_NE(assoc.state, SCTP_ASSOC_ESTABLISHED);
}

/******************************************************************************
 * Handle COOKIE-ACK
 *****************************************************************************/

TEST_F(SctpAssocApiTest, handleCookieAck_stateEstablished)
{
    driveToEstablished();
    EXPECT_EQ(assoc.state, SCTP_ASSOC_ESTABLISHED);
}

TEST_F(SctpAssocApiTest, handleCookieAck_t1TimerStopped)
{
    driveToEstablished();
    EXPECT_EQ(assoc.t1InitExpiry, (UINT64) 0);
}

TEST_F(SctpAssocApiTest, handleCookieAck_ignoredInWrongState)
{
    // In CLOSED state, COOKIE-ACK should be ignored
    BYTE peerPacket[64];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieAckChunk(peerPacket + off);
    sctpFinalizePacket(peerPacket, off);

    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_CLOSED);
}

TEST_F(SctpAssocApiTest, handleCookieAck_flushesQueuedMessages)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    // Queue a message before ESTABLISHED
    BYTE payload[] = "queued";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 6, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_GE(assoc.sendQueueCount, (UINT32) 1);

    // Complete handshake — COOKIE-ACK should flush
    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = assoc.myVerificationTag;
    cookie.myTag = 0xBBBBBBBB;
    cookie.peerInitialTsn = 0xBBBBBBBB;
    cookie.myInitialTsn = assoc.nextTsn;
    cookie.peerArwnd = SCTP_DEFAULT_ARWND;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, SCTP_DEFAULT_ARWND, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);

    BYTE cookieAck[64];
    off = sctpWriteCommonHeader(cookieAck, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieAckChunk(cookieAck + off);
    sctpFinalizePacket(cookieAck, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, cookieAck, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    // Queued message should have been flushed as DATA
    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_DATA), -1);
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) 0);
}

/******************************************************************************
 * Sending DATA
 *****************************************************************************/

TEST_F(SctpAssocApiTest, send_inEstablished_sendsDataPacket)
{
    driveToEstablished();
    BYTE payload[] = "hello";

    cap.reset();
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);

    ASSERT_GE(cap.count, (UINT32) 1);
    EXPECT_EQ(cap.packets[0].data[SCTP_COMMON_HEADER_SIZE], SCTP_CHUNK_DATA);
}

TEST_F(SctpAssocApiTest, send_tsnIncrementsPerSend)
{
    driveToEstablished();
    UINT32 tsnBefore = assoc.nextTsn;

    BYTE payload[] = "a";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.nextTsn, tsnBefore + 1);

    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.nextTsn, tsnBefore + 2);
}

TEST_F(SctpAssocApiTest, send_orderedSsnIncrements)
{
    driveToEstablished();
    BYTE payload[] = "a";

    EXPECT_EQ(assoc.nextSsn[0], (UINT16) 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.nextSsn[0], (UINT16) 1);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.nextSsn[0], (UINT16) 2);
}

TEST_F(SctpAssocApiTest, send_unorderedSsnZero)
{
    driveToEstablished();
    BYTE payload[] = "a";

    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, TRUE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    // Unordered should not increment SSN
    EXPECT_EQ(assoc.nextSsn[0], (UINT16) 0);
}

TEST_F(SctpAssocApiTest, send_payloadInDataChunk)
{
    driveToEstablished();
    BYTE payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

    cap.reset();
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 4, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);

    ASSERT_GE(cap.count, (UINT32) 1);
    // DATA chunk payload at: common_header(12) + DATA_HEADER(16) = offset 28
    EXPECT_EQ(0, MEMCMP(cap.packets[0].data + SCTP_COMMON_HEADER_SIZE + SCTP_DATA_HEADER_SIZE, payload, 4));
}

TEST_F(SctpAssocApiTest, send_ppidCorrect)
{
    driveToEstablished();
    BYTE payload[] = "a";

    cap.reset();
    sctpAssocSend(&assoc, 0, SCTP_PPID_BINARY, FALSE, payload, 1, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);

    ASSERT_GE(cap.count, (UINT32) 1);
    // PPID at offset 12 + 12 = 24 (common_header + DATA_chunk_ppid_offset)
    UINT32 ppid = (UINT32) getUnalignedInt32BigEndian((PINT32)(cap.packets[0].data + SCTP_COMMON_HEADER_SIZE + 12));
    EXPECT_EQ(ppid, (UINT32) SCTP_PPID_BINARY);
}

TEST_F(SctpAssocApiTest, send_outstandingEntryCreated)
{
    driveToEstablished();
    BYTE payload[] = "hello";

    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);
}

TEST_F(SctpAssocApiTest, send_flightSizeUpdated)
{
    driveToEstablished();
    BYTE payload[] = "hello";

    EXPECT_EQ(assoc.flightSize, (UINT32) 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.flightSize, (UINT32) 5);
}

TEST_F(SctpAssocApiTest, send_t3RtxTimerStarted)
{
    driveToEstablished();
    EXPECT_EQ(assoc.t3RtxExpiry, (UINT64) 0);

    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_GT(assoc.t3RtxExpiry, (UINT64) 0);
}

TEST_F(SctpAssocApiTest, send_notEstablished_queuesMessage)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) 0);

    BYTE payload[] = "queued";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 6, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) 1);
}

TEST_F(SctpAssocApiTest, send_queueFull_dropsGracefully)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    BYTE payload[] = "x";
    for (UINT32 i = 0; i < SCTP_MAX_QUEUED_MESSAGES; i++) {
        sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    }
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) SCTP_MAX_QUEUED_MESSAGES);

    // One more should not crash
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) SCTP_MAX_QUEUED_MESSAGES);
}

TEST_F(SctpAssocApiTest, send_multipleSendsMultipleOutstanding)
{
    driveToEstablished();
    BYTE payload[] = "a";

    for (int i = 0; i < 5; i++) {
        sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    }
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 5);
}

TEST_F(SctpAssocApiTest, send_streamIdPreserved)
{
    driveToEstablished();
    BYTE payload[] = "a";

    sctpAssocSend(&assoc, 42, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);

    // Find the outstanding entry
    BOOL found = FALSE;
    for (UINT32 i = 0; i < SCTP_MAX_OUTSTANDING; i++) {
        if (assoc.outstanding[i].inUse) {
            EXPECT_EQ(assoc.outstanding[i].streamId, (UINT16) 42);
            found = TRUE;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SctpAssocApiTest, send_differentStreamsIndependentSsn)
{
    driveToEstablished();
    BYTE payload[] = "a";

    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 1, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.nextSsn[0], (UINT16) 1);
    EXPECT_EQ(assoc.nextSsn[1], (UINT16) 1);
}

/******************************************************************************
 * Receiving DATA
 *****************************************************************************/

TEST_F(SctpAssocApiTest, handleData_inOrderDelivered)
{
    driveToEstablished();

    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 tsn = assoc.peerCumulativeTsn + 1;
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, 0, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);

    ASSERT_EQ(msg.count, (UINT32) 1);
    EXPECT_EQ(msg.messages[0].payloadLen, (UINT32) 5);
    EXPECT_EQ(0, MEMCMP(msg.messages[0].payload, payload, 5));
}

TEST_F(SctpAssocApiTest, handleData_sendsSackImmediately)
{
    driveToEstablished();

    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 tsn = assoc.peerCumulativeTsn + 1;
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, 0, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    cap.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_SACK), -1);
}

TEST_F(SctpAssocApiTest, handleData_peerCumTsnAdvances)
{
    driveToEstablished();

    UINT32 before = assoc.peerCumulativeTsn;

    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, before + 1, 0, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerCumulativeTsn, before + 1);
}

TEST_F(SctpAssocApiTest, handleData_duplicateTsnIgnored)
{
    driveToEstablished();

    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 tsn = assoc.peerCumulativeTsn + 1;
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, 0, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 1);

    // Send same TSN again
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    // Should not deliver again
    EXPECT_EQ(msg.count, (UINT32) 1);
}

TEST_F(SctpAssocApiTest, handleData_outOfOrderStored)
{
    driveToEstablished();

    BYTE payload[] = "hello";
    BYTE packet[256];
    // Send TSN+2 (skip TSN+1)
    UINT32 tsn = assoc.peerCumulativeTsn + 2;
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, 0, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_GE(assoc.receivedTsnCount, (UINT32) 1);
}

TEST_F(SctpAssocApiTest, handleData_gapFill_advancesCumTsn)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn;

    BYTE payload[] = "a";
    BYTE packet[256];

    // Send TSN+2 (gap)
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, base + 2, 0, 0, SCTP_PPID_STRING, FALSE, payload, 1);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerCumulativeTsn, base); // not advanced

    // Send TSN+1 (fill gap)
    off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, base + 1, 0, 0, SCTP_PPID_STRING, FALSE, payload, 1);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerCumulativeTsn, base + 2); // both now consecutive
}

TEST_F(SctpAssocApiTest, handleData_notEstablished_ignored)
{
    // Don't drive to established
    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, 1, 0, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, handleData_ppidForwarded)
{
    driveToEstablished();

    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 tsn = assoc.peerCumulativeTsn + 1;
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, 0, 0, SCTP_PPID_BINARY, FALSE, payload, 5);

    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);

    ASSERT_EQ(msg.count, (UINT32) 1);
    EXPECT_EQ(msg.messages[0].ppid, (UINT32) SCTP_PPID_BINARY);
}

TEST_F(SctpAssocApiTest, handleData_streamIdForwarded)
{
    driveToEstablished();

    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 tsn = assoc.peerCumulativeTsn + 1;
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, 42, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);

    ASSERT_EQ(msg.count, (UINT32) 1);
    EXPECT_EQ(msg.messages[0].streamId, (UINT32) 42);
}

TEST_F(SctpAssocApiTest, handleData_sackHasCorrectCumTsn)
{
    driveToEstablished();

    BYTE payload[] = "hello";
    BYTE packet[256];
    UINT32 tsn = assoc.peerCumulativeTsn + 1;
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, tsn, 0, 0, SCTP_PPID_STRING, FALSE, payload, 5);

    cap.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    INT32 sackIdx = cap.findChunkType(SCTP_CHUNK_SACK);
    ASSERT_GE(sackIdx, 0);

    // SACK cumTsn at: common_header(12) + chunk_header(4) + cumTsn(4 at offset 0)
    UINT32 sackCumTsn =
        (UINT32) getUnalignedInt32BigEndian((PINT32)(cap.packets[sackIdx].data + SCTP_COMMON_HEADER_SIZE + SCTP_CHUNK_HEADER_SIZE));
    EXPECT_EQ(sackCumTsn, tsn);
}

/******************************************************************************
 * SACK Processing
 *****************************************************************************/

TEST_F(SctpAssocApiTest, handleSack_cumTsnAdvances)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    UINT32 sentTsn = assoc.nextTsn - 1;

    cap.reset();
    sendPeerSack(sentTsn);
    EXPECT_EQ(assoc.cumulativeAckTsn, sentTsn);
}

TEST_F(SctpAssocApiTest, handleSack_outstandingFreed)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);

    UINT32 sentTsn = assoc.nextTsn - 1;
    sendPeerSack(sentTsn);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, handleSack_flightSizeReduced)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.flightSize, (UINT32) 5);

    UINT32 sentTsn = assoc.nextTsn - 1;
    sendPeerSack(sentTsn);
    EXPECT_EQ(assoc.flightSize, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, handleSack_peerArwndUpdated)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT32 sentTsn = assoc.nextTsn - 1;

    BYTE packet[64];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteSackChunk(packet + off, sentTsn, 77777, NULL, NULL, 0);
    sctpFinalizePacket(packet, off);

    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerArwnd, (UINT32) 77777);
}

TEST_F(SctpAssocApiTest, handleSack_t3TimerStoppedWhenEmpty)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_GT(assoc.t3RtxExpiry, (UINT64) 0);

    UINT32 sentTsn = assoc.nextTsn - 1;
    sendPeerSack(sentTsn);
    EXPECT_EQ(assoc.t3RtxExpiry, (UINT64) 0);
}

TEST_F(SctpAssocApiTest, handleSack_t3TimerRestartedWhenStillOutstanding)
{
    driveToEstablished();
    BYTE payload[] = "a";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 2);

    // Ack only the first
    UINT32 firstTsn = assoc.nextTsn - 2;
    sendPeerSack(firstTsn);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);
    EXPECT_GT(assoc.t3RtxExpiry, (UINT64) 0);
}

TEST_F(SctpAssocApiTest, handleSack_cwndGrowsInSlowStart)
{
    driveToEstablished();
    UINT32 cwndBefore = assoc.cwnd;
    // Ensure we're in slow start (cwnd <= ssthresh)
    EXPECT_LE(assoc.cwnd, assoc.ssthresh);

    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    UINT32 sentTsn = assoc.nextTsn - 1;
    sendPeerSack(sentTsn);

    EXPECT_GT(assoc.cwnd, cwndBefore);
}

/******************************************************************************
 * FORWARD-TSN Handling
 *****************************************************************************/

TEST_F(SctpAssocApiTest, handleForwardTsn_advancesPeerCumTsn)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn;

    BYTE packet[64];
    UINT32 off = buildForwardTsnPacket(packet, 5000, 5000, assoc.myVerificationTag, base + 5);

    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerCumulativeTsn, base + 5);
}

TEST_F(SctpAssocApiTest, handleForwardTsn_removesOldReceivedTsns)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn;

    // First receive an out-of-order TSN
    BYTE payload[] = "a";
    BYTE dataPacket[256];
    UINT32 off = buildDataPacket(dataPacket, 5000, 5000, assoc.myVerificationTag, base + 3, 0, 0, SCTP_PPID_STRING, FALSE, payload, 1);
    sctpAssocHandlePacket(&assoc, dataPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_GE(assoc.receivedTsnCount, (UINT32) 1);

    // Send FORWARD-TSN past it
    BYTE fwdPacket[64];
    off = buildForwardTsnPacket(fwdPacket, 5000, 5000, assoc.myVerificationTag, base + 5);
    sctpAssocHandlePacket(&assoc, fwdPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.receivedTsnCount, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, handleForwardTsn_sendsSack)
{
    driveToEstablished();

    BYTE packet[64];
    UINT32 off = buildForwardTsnPacket(packet, 5000, 5000, assoc.myVerificationTag, assoc.peerCumulativeTsn + 5);

    cap.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);
    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_SACK), -1);
}

TEST_F(SctpAssocApiTest, handleForwardTsn_backwardIgnored)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn;

    // Advance first
    BYTE packet[64];
    UINT32 off = buildForwardTsnPacket(packet, 5000, 5000, assoc.myVerificationTag, base + 5);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerCumulativeTsn, base + 5);

    // Try to go backward
    off = buildForwardTsnPacket(packet, 5000, 5000, assoc.myVerificationTag, base + 2);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.peerCumulativeTsn, base + 5); // unchanged
}

/******************************************************************************
 * Timer Management
 *****************************************************************************/

TEST_F(SctpAssocApiTest, t1Timer_retransmitsInit)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_COOKIE_WAIT);

    UINT64 farFuture = assoc.t1InitExpiry + 1000;

    cap.reset();
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundCapture, (UINT64) &cap);

    // Should have retransmitted INIT
    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_INIT), -1);
}

TEST_F(SctpAssocApiTest, t1Timer_retransmitsCookieEcho)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    // Move to COOKIE_ECHOED by receiving INIT-ACK
    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, 131072, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_COOKIE_ECHOED);

    UINT64 farFuture = assoc.t1InitExpiry + 1000;
    cap.reset();
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundCapture, (UINT64) &cap);

    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_COOKIE_ECHO), -1);
}

TEST_F(SctpAssocApiTest, t1Timer_rtoBackedOff)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);
    UINT32 rtoBefore = assoc.rtoMs;

    UINT64 farFuture = assoc.t1InitExpiry + 1000;
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.rtoMs, rtoBefore * 2);
}

TEST_F(SctpAssocApiTest, t1Timer_rtoCappedAtMax)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);
    assoc.rtoMs = SCTP_RTO_MAX_MS; // already at max

    UINT64 farFuture = assoc.t1InitExpiry + 1000;
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.rtoMs, (UINT32) SCTP_RTO_MAX_MS);
}

TEST_F(SctpAssocApiTest, t1Timer_maxRetransmitsClosed)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    // Fire T1 enough times to exceed max retransmissions
    for (UINT32 i = 0; i <= SCTP_MAX_INIT_RETRANS + 1; i++) {
        UINT64 farFuture = assoc.t1InitExpiry + 1000;
        if (assoc.t1InitExpiry == 0 || assoc.state == SCTP_ASSOC_CLOSED) {
            break;
        }
        sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);
    }

    EXPECT_EQ(assoc.state, SCTP_ASSOC_CLOSED);
}

TEST_F(SctpAssocApiTest, t1Timer_noExpiryWhenNotSet)
{
    // Timer not set
    EXPECT_EQ(assoc.t1InitExpiry, (UINT64) 0);

    cap.reset();
    sctpAssocCheckTimers(&assoc, 999999, mockOutboundCapture, (UINT64) &cap);
    EXPECT_EQ(cap.count, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, t3Timer_retransmitsData)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT64 farFuture = assoc.t3RtxExpiry + 1000;
    cap.reset();
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundCapture, (UINT64) &cap);

    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_DATA), -1);
}

TEST_F(SctpAssocApiTest, t3Timer_ssthreshHalved)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT32 cwndBefore = assoc.cwnd;
    UINT64 farFuture = assoc.t3RtxExpiry + 1000;
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);

    UINT32 expectedSsthresh = (cwndBefore / 2 > 2 * assoc.mtu) ? cwndBefore / 2 : 2 * assoc.mtu;
    EXPECT_EQ(assoc.ssthresh, expectedSsthresh);
}

TEST_F(SctpAssocApiTest, t3Timer_cwndResetToMtu)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT64 farFuture = assoc.t3RtxExpiry + 1000;
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.cwnd, (UINT32) assoc.mtu);
}

TEST_F(SctpAssocApiTest, t3Timer_rtoBackedOff)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT32 rtoBefore = assoc.rtoMs;
    UINT64 farFuture = assoc.t3RtxExpiry + 1000;
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.rtoMs, rtoBefore * 2);
}

TEST_F(SctpAssocApiTest, t3Timer_rtoCappedAtMax)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    assoc.rtoMs = SCTP_RTO_MAX_MS;
    UINT64 farFuture = assoc.t3RtxExpiry + 1000;
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.rtoMs, (UINT32) SCTP_RTO_MAX_MS);
}

TEST_F(SctpAssocApiTest, t3Timer_restartedWhenStillOutstanding)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT64 farFuture = assoc.t3RtxExpiry + 1000;
    sctpAssocCheckTimers(&assoc, farFuture, mockOutboundNoop, 0);

    // Should still have timer running
    EXPECT_GT(assoc.t3RtxExpiry, (UINT64) 0);
}

TEST_F(SctpAssocApiTest, t3Timer_noExpiryWhenNotSet)
{
    driveToEstablished();
    EXPECT_EQ(assoc.t3RtxExpiry, (UINT64) 0);

    cap.reset();
    sctpAssocCheckTimers(&assoc, 999999, mockOutboundCapture, (UINT64) &cap);
    EXPECT_EQ(cap.count, (UINT32) 0);
}

// Rust: stopped_timer_does_not_expire — start T3, stop it via SACK, then advance time
TEST_F(SctpAssocApiTest, t3Timer_stoppedDoesNotExpire)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    // T3 should be running
    EXPECT_GT(assoc.t3RtxExpiry, (UINT64) 0);
    UINT64 expiryBefore = assoc.t3RtxExpiry;

    // SACK acks the DATA → outstanding goes to 0 → T3 should stop
    sendPeerSack(assoc.nextTsn - 1);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
    EXPECT_EQ(assoc.t3RtxExpiry, (UINT64) 0);

    // Advance time past the original expiry — no retransmit should happen
    cap.reset();
    sctpAssocCheckTimers(&assoc, expiryBefore + 10000, mockOutboundCapture, (UINT64) &cap);
    EXPECT_EQ(cap.count, (UINT32) 0);
}

// Rust: timer_restart_does_not_drift — verify expiry is relative to current time, not accumulated
TEST_F(SctpAssocApiTest, t3Timer_noDrift)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT64 firstExpiry = assoc.t3RtxExpiry;
    UINT32 rto1 = assoc.rtoMs;

    // Fire T3 at exactly first expiry + 1
    sctpAssocCheckTimers(&assoc, firstExpiry + 1, mockOutboundNoop, 0);

    // RTO doubled
    UINT32 rto2 = assoc.rtoMs;
    EXPECT_EQ(rto2, rto1 * 2);

    // New expiry should be based on the current time (firstExpiry+1) + new RTO,
    // NOT on firstExpiry + rto1 + rto2 (which would cause drift)
    UINT64 secondExpiry = assoc.t3RtxExpiry;
    EXPECT_GE(secondExpiry, firstExpiry + 1);

    // The difference between new expiry and the fire time should be the new RTO
    UINT64 fireTime = firstExpiry + 1;
    EXPECT_EQ(secondExpiry - fireTime, (UINT64) rto2);
}

/******************************************************************************
 * PR-SCTP / Abandonment
 *****************************************************************************/

TEST_F(SctpAssocApiTest, prsctp_maxRetransmitsExceeded_abandoned)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 1, 0, mockOutboundNoop, 0); // maxRetransmits=1

    // First T3 timeout: retransmitCount goes to 1
    UINT64 time = assoc.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&assoc, time, mockOutboundNoop, 0);

    // Second T3 timeout: retransmitCount goes to 2, > maxRetransmits(1) -> abandoned
    time = assoc.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&assoc, time, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, prsctp_lifetimeExpired_abandoned)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 100, mockOutboundNoop, 0); // lifetimeMs=100

    // Trigger T3 at a time well past lifetime
    UINT64 time = assoc.t3RtxExpiry + 200;
    sctpAssocCheckTimers(&assoc, time, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, prsctp_forwardTsnSent)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0, 0, mockOutboundNoop, 0); // maxRetransmits=0

    // First T3 timeout should abandon (retransmitCount=1 > maxRetransmits=0)
    cap.reset();
    UINT64 time = assoc.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&assoc, time, mockOutboundCapture, (UINT64) &cap);

    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_FORWARD_TSN), -1);
}

TEST_F(SctpAssocApiTest, prsctp_flightSizeReducedOnAbandon)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.flightSize, (UINT32) 5);

    UINT64 time = assoc.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&assoc, time, mockOutboundNoop, 0);

    EXPECT_EQ(assoc.flightSize, (UINT32) 0);
}

/******************************************************************************
 * Shutdown
 *****************************************************************************/

TEST_F(SctpAssocApiTest, shutdown_sendsShutdownChunk)
{
    driveToEstablished();

    cap.reset();
    sctpAssocShutdown(&assoc, mockOutboundCapture, (UINT64) &cap);

    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_SHUTDOWN), -1);
}

TEST_F(SctpAssocApiTest, shutdown_stateShutdownSent)
{
    driveToEstablished();
    sctpAssocShutdown(&assoc, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_SHUTDOWN_SENT);
}

TEST_F(SctpAssocApiTest, shutdown_notEstablished_noop)
{
    // In CLOSED state, shutdown should be no-op
    cap.reset();
    sctpAssocShutdown(&assoc, mockOutboundCapture, (UINT64) &cap);
    EXPECT_EQ(cap.count, (UINT32) 0);
}

TEST_F(SctpAssocApiTest, handleShutdownAck_sendsShutdownComplete)
{
    driveToEstablished();
    sctpAssocShutdown(&assoc, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_SHUTDOWN_SENT);

    // Peer sends SHUTDOWN-ACK
    BYTE packet[64];
    UINT32 off = sctpWriteCommonHeader(packet, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteShutdownAckChunk(packet + off);
    sctpFinalizePacket(packet, off);

    cap.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundCapture, (UINT64) &cap, mockMessageNoop, 0);

    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_SHUTDOWN_COMPLETE), -1);
    EXPECT_EQ(assoc.state, SCTP_ASSOC_CLOSED);
}

/******************************************************************************
 * Cleanup
 *****************************************************************************/

TEST_F(SctpAssocApiTest, cleanup_freesOutstandingPayloads)
{
    driveToEstablished();
    BYTE payload[] = "hello";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);

    sctpAssocCleanup(&assoc);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
    EXPECT_EQ(assoc.flightSize, (UINT32) 0);
}

/******************************************************************************
 * Gap Block SACK Generation (matches Rust dcsctp chunk_validators.rs + data_tracker.rs)
 *
 * Tests verify that when we receive out-of-order DATA, our outbound SACK
 * contains correctly sorted and merged gap ack blocks.
 *****************************************************************************/

// All gap block tests use base TSN relative to peer's initial TSN from handshake.
// After driveToEstablished(), peerCumulativeTsn = peerInitialTsn - 1 = 0xBBBBBBBA.
// The first in-order DATA from peer should use TSN = peerCumulativeTsn + 1.

// No gaps: in-order DATA → SACK with cumTsn=base, no gap blocks
TEST_F(SctpAssocApiTest, gapBlocks_noGapsValid)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    cap.reset();
    sendPeerData(base);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// Single gap block: receive base, then base+2 (skip base+1) → gap [2,2]
TEST_F(SctpAssocApiTest, gapBlocks_singleBlock)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    cap.reset();
    sendPeerData(base + 2);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 2);
}

// Two gap blocks: receive base, base+2, base+4 → gaps [2,2] [4,4]
TEST_F(SctpAssocApiTest, gapBlocks_twoBlocks)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    cap.reset();
    sendPeerData(base + 4);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[1], (UINT16) 4);
    EXPECT_EQ(sack.gapEnds[1], (UINT16) 4);
}

// Sorted: receive TSNs out of order (base+4 before base+2) → SACK has sorted gaps
TEST_F(SctpAssocApiTest, gapBlocks_sorted)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 4);
    cap.reset();
    sendPeerData(base + 2);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[1], (UINT16) 4);
    EXPECT_EQ(sack.gapEnds[1], (UINT16) 4);
}

// Adjacent merge: receive base+2 then base+3 → single gap [2,3]
TEST_F(SctpAssocApiTest, gapBlocks_adjacentMerged)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    cap.reset();
    sendPeerData(base + 3);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 3);
}

// Expand right: base+2 then base+3 → gap grows [2,2] → [2,3]
TEST_F(SctpAssocApiTest, gapBlock_expandRight)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    cap.reset();
    sendPeerData(base + 3);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 3);
}

// Expand right with another block: base+2, base+10, base+20, then base+11
TEST_F(SctpAssocApiTest, gapBlock_expandRightWithOther)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 10);
    sendPeerData(base + 20);
    cap.reset();
    sendPeerData(base + 11);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 3);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[1], (UINT16) 10);
    EXPECT_EQ(sack.gapEnds[1], (UINT16) 11);
    EXPECT_EQ(sack.gapStarts[2], (UINT16) 20);
    EXPECT_EQ(sack.gapEnds[2], (UINT16) 20);
}

// Expand left: receive base+3 then base+2 → gap [3,3] → [2,3]
TEST_F(SctpAssocApiTest, gapBlock_expandLeft)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 3);
    cap.reset();
    sendPeerData(base + 2);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 3);
}

// Expand left with another block: base+2, base+11, base+20, then base+10
TEST_F(SctpAssocApiTest, gapBlock_expandLeftWithOther)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 11);
    sendPeerData(base + 20);
    cap.reset();
    sendPeerData(base + 10);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 3);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[1], (UINT16) 10);
    EXPECT_EQ(sack.gapEnds[1], (UINT16) 11);
    EXPECT_EQ(sack.gapStarts[2], (UINT16) 20);
    EXPECT_EQ(sack.gapEnds[2], (UINT16) 20);
}

// Expand and merge: two blocks [2,2] [4,4], then fill base+3 → single block [2,4]
TEST_F(SctpAssocApiTest, gapBlock_expandAndMerge)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 4);
    cap.reset();
    sendPeerData(base + 3);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 4);
}

// Merge many into one: [2,2] [4,4] [6,6] then base+3, base+5 → [2,6]
TEST_F(SctpAssocApiTest, gapBlock_mergeManyIntoOne)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 4);
    sendPeerData(base + 6);
    sendPeerData(base + 3);
    cap.reset();
    sendPeerData(base + 5);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 6);
}

// Duplicate in gap: receive base+2 twice → same gap block
TEST_F(SctpAssocApiTest, gapBlock_addDuplicate)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    cap.reset();
    sendPeerData(base + 2); // duplicate

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 2);
}

// Fill gap → cumTsn advances past gap blocks
TEST_F(SctpAssocApiTest, gapBlock_fillGapAdvancesCumTsn)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 3);
    cap.reset();
    sendPeerData(base + 1); // fill the gap

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 3);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// Fill gap across three blocks
TEST_F(SctpAssocApiTest, gapBlock_fillGapThreeBlocks)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 4);
    sendPeerData(base + 6);

    sendPeerData(base + 1);
    sendPeerData(base + 3);
    cap.reset();
    sendPeerData(base + 5);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 6);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// Single-TSN gap block preserved
TEST_F(SctpAssocApiTest, gapBlock_singleTsnPreserved)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    cap.reset();
    sendPeerData(base + 2);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], sack.gapEnds[0]);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
}

// FORWARD-TSN before first gap block → gaps preserved with new offsets
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnBeforeFirstBlock)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 10);

    cap.reset();
    sendPeerForwardTsn(base + 1);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 1);
    EXPECT_EQ(sack.numGaps, (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 4);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 4);
    EXPECT_EQ(sack.gapStarts[1], (UINT16) 9);
    EXPECT_EQ(sack.gapEnds[1], (UINT16) 9);
}

// FORWARD-TSN at beginning of block → cumTsn advances through consecutive
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnAtBlockStart)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 6);
    sendPeerData(base + 10);

    cap.reset();
    sendPeerForwardTsn(base + 5);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 6);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 4);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 4);
}

// FORWARD-TSN far after all blocks → all gone
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnFarAfterAll)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 10);

    cap.reset();
    sendPeerForwardTsn(base + 100);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 100);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// FORWARD-TSN between blocks → first removed, second preserved
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnBetweenBlocks)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 10);

    cap.reset();
    sendPeerForwardTsn(base + 7);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 7);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 3);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 3);
}

// FORWARD-TSN merges on fill
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnMergesOnFill)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 7);

    sendPeerForwardTsn(base + 4); // cumTsn→base+4, then base+5 consecutive → base+5
    cap.reset();
    sendPeerData(base + 6); // fills gap → cumTsn=base+7

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 7);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// Many in-order advances: 11 sequential TSNs
TEST_F(SctpAssocApiTest, gapBlock_manyInOrderAdvances)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    for (UINT32 i = 0; i < 10; i++) {
        sendPeerData(base + i);
    }
    cap.reset();
    sendPeerData(base + 10);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 10);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// FORWARD-TSN backward (≤ current cumAck) → no change
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnBackwardNoop)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    EXPECT_EQ(assoc.peerCumulativeTsn, base);

    cap.reset();
    sendPeerForwardTsn(base - 1); // backward

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
}

// Contiguous duplicate ignored
TEST_F(SctpAssocApiTest, gapBlock_contiguousDuplicateIgnored)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    EXPECT_EQ(assoc.peerCumulativeTsn, base);

    cap.reset();
    sendPeerData(base); // duplicate

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// Three blocks, fill middle → two blocks merge
TEST_F(SctpAssocApiTest, gapBlock_fillMiddleMerges)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 4);
    sendPeerData(base + 6);

    cap.reset();
    sendPeerData(base + 3); // fill between first and second → [2,4] [6,6]

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 2);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 2);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 4);
    EXPECT_EQ(sack.gapStarts[1], (UINT16) 6);
    EXPECT_EQ(sack.gapEnds[1], (UINT16) 6);
}

// Reversed order: base+5..base+1 → all merge, cumTsn=base+5
TEST_F(SctpAssocApiTest, gapBlock_reversedOrderAllMerge)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 4);
    sendPeerData(base + 3);
    sendPeerData(base + 2);
    cap.reset();
    sendPeerData(base + 1);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 5);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// Wide gap: base, then base+100 → single gap block at offset 100
TEST_F(SctpAssocApiTest, gapBlock_wideGap)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    cap.reset();
    sendPeerData(base + 100);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 100);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 100);
}

// FORWARD-TSN at end of block: block consumed, cumTsn advances through
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnAtBlockEnd)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 6);
    sendPeerData(base + 10);

    cap.reset();
    sendPeerForwardTsn(base + 6);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 6);
    EXPECT_EQ(sack.numGaps, (UINT16) 1);
    EXPECT_EQ(sack.gapStarts[0], (UINT16) 4);
    EXPECT_EQ(sack.gapEnds[0], (UINT16) 4);
}

// FORWARD-TSN at middle of block: partial block consumed
TEST_F(SctpAssocApiTest, gapBlock_forwardTsnAtBlockMiddle)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 5);
    sendPeerData(base + 6);
    sendPeerData(base + 7);
    sendPeerData(base + 8);

    cap.reset();
    sendPeerForwardTsn(base + 6);

    SackInfo sack;
    ASSERT_TRUE(parseSack(cap, sack));
    EXPECT_EQ(sack.cumTsn, base + 8);
    EXPECT_EQ(sack.numGaps, (UINT16) 0);
}

// CumTsn valid at rest after in-order DATA
TEST_F(SctpAssocApiTest, gapBlock_cumTsnValidAtRest)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    EXPECT_EQ(assoc.peerCumulativeTsn, base);
    EXPECT_EQ(assoc.receivedTsnCount, (UINT32) 0);
}

// CumTsn valid with gap TSNs: verify internal state
TEST_F(SctpAssocApiTest, gapBlock_cumTsnValidWithGaps)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    sendPeerData(base);
    sendPeerData(base + 2);
    sendPeerData(base + 4);

    EXPECT_EQ(assoc.peerCumulativeTsn, base);
    EXPECT_EQ(assoc.receivedTsnCount, (UINT32) 2);
}

/******************************************************************************
 * Send / Fragment Flags
 * Matches Rust: carve_out_beginning_middle_and_end, add_and_get_single_chunk,
 *   defaults_to_ordered_send, empty_buffer
 *****************************************************************************/

// Single-chunk message has both BEGIN and END flags set
TEST_F(SctpAssocApiTest, send_singleChunk_bothBEFlags)
{
    driveToEstablished();
    BYTE payload[] = "hello";

    cap.reset();
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);

    ASSERT_GE(cap.count, (UINT32) 1);
    UINT8 flags = cap.packets[0].data[SCTP_COMMON_HEADER_SIZE + 1];
    EXPECT_TRUE((flags & SCTP_DATA_FLAG_BEGIN) != 0);
    EXPECT_TRUE((flags & SCTP_DATA_FLAG_END) != 0);
}

// 3-fragment message: first has B only, middle has neither, last has E only
TEST_F(SctpAssocApiTest, send_threeFragments_correctFlags)
{
    driveToEstablished();
    UINT32 maxDataPayload = assoc.mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;
    UINT32 bigLen = maxDataPayload * 2 + 10; // ~2.x fragments → 3 chunks

    BYTE* bigPayload = (BYTE*) MEMALLOC(bigLen);
    ASSERT_TRUE(bigPayload != NULL);
    MEMSET(bigPayload, 0xAA, bigLen);

    cap.reset();
    sctpAssocSend(&assoc, 0, SCTP_PPID_BINARY, FALSE, bigPayload, bigLen, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);

    ASSERT_EQ(cap.count, (UINT32) 3);

    // First fragment: BEGIN set, END clear
    UINT8 f0 = cap.packets[0].data[SCTP_COMMON_HEADER_SIZE + 1];
    EXPECT_TRUE((f0 & SCTP_DATA_FLAG_BEGIN) != 0);
    EXPECT_TRUE((f0 & SCTP_DATA_FLAG_END) == 0);

    // Middle fragment: neither BEGIN nor END
    UINT8 f1 = cap.packets[1].data[SCTP_COMMON_HEADER_SIZE + 1];
    EXPECT_TRUE((f1 & SCTP_DATA_FLAG_BEGIN) == 0);
    EXPECT_TRUE((f1 & SCTP_DATA_FLAG_END) == 0);

    // Last fragment: END set, BEGIN clear
    UINT8 f2 = cap.packets[2].data[SCTP_COMMON_HEADER_SIZE + 1];
    EXPECT_TRUE((f2 & SCTP_DATA_FLAG_BEGIN) == 0);
    EXPECT_TRUE((f2 & SCTP_DATA_FLAG_END) != 0);

    MEMFREE(bigPayload);
}

// Two messages from different streams have correct stream IDs in the DATA chunk
TEST_F(SctpAssocApiTest, send_twoStreams_correctStreamIdInChunk)
{
    driveToEstablished();
    BYTE payload[] = "a";

    cap.reset();
    sctpAssocSend(&assoc, 10, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);
    sctpAssocSend(&assoc, 20, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);

    ASSERT_EQ(cap.count, (UINT32) 2);

    // Stream ID at offset: common_header(12) + chunk_data_offset_8 = 20
    UINT16 sid0 = (UINT16) getUnalignedInt16BigEndian((PINT16)(cap.packets[0].data + SCTP_COMMON_HEADER_SIZE + 8));
    UINT16 sid1 = (UINT16) getUnalignedInt16BigEndian((PINT16)(cap.packets[1].data + SCTP_COMMON_HEADER_SIZE + 8));
    EXPECT_EQ(sid0, (UINT16) 10);
    EXPECT_EQ(sid1, (UINT16) 20);
}

// Send queue starts empty
TEST_F(SctpAssocApiTest, send_queueStartsEmpty)
{
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) 0);
}

// Queue drains to 0 after COOKIE-ACK transitions to ESTABLISHED
TEST_F(SctpAssocApiTest, send_queueDrainsOnEstablished)
{
    sctpAssocConnect(&assoc, mockOutboundNoop, 0);

    BYTE payload[] = "queued1";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 7, 0xFFFF, 0, mockOutboundNoop, 0);
    BYTE payload2[] = "queued2";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload2, 7, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) 2);

    // Complete handshake
    SctpCookie cookie;
    MEMSET(&cookie, 0, sizeof(cookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = assoc.myVerificationTag;
    cookie.myTag = 0xBBBBBBBB;
    cookie.peerInitialTsn = 0xBBBBBBBB;
    cookie.myInitialTsn = assoc.nextTsn;
    cookie.peerArwnd = SCTP_DEFAULT_ARWND;

    BYTE peerPacket[256];
    UINT32 off = sctpWriteCommonHeader(peerPacket, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteInitAckChunk(peerPacket + off, 0xBBBBBBBB, SCTP_DEFAULT_ARWND, 300, 300, 0xBBBBBBBB, (PBYTE) &cookie, SCTP_COOKIE_SIZE);
    sctpFinalizePacket(peerPacket, off);
    sctpAssocHandlePacket(&assoc, peerPacket, off, mockOutboundNoop, 0, mockMessageNoop, 0);

    BYTE cookieAck[64];
    off = sctpWriteCommonHeader(cookieAck, 5000, 5000, assoc.myVerificationTag);
    off += sctpWriteCookieAckChunk(cookieAck + off);
    sctpFinalizePacket(cookieAck, off);
    sctpAssocHandlePacket(&assoc, cookieAck, off, mockOutboundNoop, 0, mockMessageNoop, 0);

    EXPECT_EQ(assoc.state, SCTP_ASSOC_ESTABLISHED);
    EXPECT_EQ(assoc.sendQueueCount, (UINT32) 0);
}

/******************************************************************************
 * SACK with Gap Blocks — Send Side
 * Matches Rust: acks_and_nacks_with_gap_ack_blocks,
 *   gap_ack_blocks_do_not_move_cum_tsn_ack, send_three_chunks_and_ack_two
 *****************************************************************************/

// Gap block in SACK acks a specific TSN, frees its outstanding entry
TEST_F(SctpAssocApiTest, handleSack_gapBlockAcksSpecificTsn)
{
    driveToEstablished();
    BYTE payload[] = "a";

    // Send 3 chunks: TSN, TSN+1, TSN+2
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 3);

    UINT32 firstTsn = assoc.nextTsn - 3;

    // Peer SACKs cumTsn=firstTsn (acks first), gap [2,2] (acks firstTsn+2 = third, skips second)
    UINT16 gapStarts[] = {2};
    UINT16 gapEnds[] = {2};
    cap.reset();
    sendPeerSackWithGaps(firstTsn, gapStarts, gapEnds, 1);

    // First and third acked → only second remains outstanding
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);

    // Verify the remaining outstanding is the second TSN
    BOOL foundSecond = FALSE;
    for (UINT32 i = 0; i < SCTP_MAX_OUTSTANDING; i++) {
        if (assoc.outstanding[i].inUse) {
            EXPECT_EQ(assoc.outstanding[i].tsn, firstTsn + 1);
            foundSecond = TRUE;
            break;
        }
    }
    EXPECT_TRUE(foundSecond);
}

// Gap-only SACK (cumTsn not advancing) does not move cumulativeAckTsn
TEST_F(SctpAssocApiTest, handleSack_gapBlockDoesNotMoveCumTsn)
{
    driveToEstablished();
    BYTE payload[] = "a";

    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT32 firstTsn = assoc.nextTsn - 3;
    UINT32 cumAckBefore = assoc.cumulativeAckTsn;

    // SACK with cumTsn before our first TSN, gap acks third
    UINT16 gapStarts[] = {3};
    UINT16 gapEnds[] = {3};
    sendPeerSackWithGaps(firstTsn - 1, gapStarts, gapEnds, 1);

    // cumulativeAckTsn should NOT have moved past firstTsn-1
    EXPECT_EQ(assoc.cumulativeAckTsn, firstTsn - 1);
}

// Send 3 chunks, SACK acks first 2 by cumTsn, third remains
TEST_F(SctpAssocApiTest, handleSack_partialAckRemainder)
{
    driveToEstablished();
    BYTE payload[] = "a";

    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 3);

    UINT32 firstTsn = assoc.nextTsn - 3;
    sendPeerSack(firstTsn + 1); // acks first two

    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);
    EXPECT_EQ(assoc.cumulativeAckTsn, firstTsn + 1);
}

// Gap block SACK reduces flight size for gap-acked chunks
TEST_F(SctpAssocApiTest, handleSack_gapBlockReducesFlightSize)
{
    driveToEstablished();
    BYTE payload[] = "hello";

    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 5, 0xFFFF, 0, mockOutboundNoop, 0);
    EXPECT_EQ(assoc.flightSize, (UINT32) 15);

    UINT32 firstTsn = assoc.nextTsn - 3;

    // Gap ack the third chunk only (cumTsn before our data)
    UINT16 gapStarts[] = {3};
    UINT16 gapEnds[] = {3};
    sendPeerSackWithGaps(firstTsn - 1, gapStarts, gapEnds, 1);

    // Third chunk (5 bytes) acked by gap → flight size reduced by 5
    EXPECT_EQ(assoc.flightSize, (UINT32) 10);
}

// Gap block SACK reduces outstanding count
TEST_F(SctpAssocApiTest, handleSack_gapBlockReducesOutstanding)
{
    driveToEstablished();
    BYTE payload[] = "a";

    for (int i = 0; i < 5; i++) {
        sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    }
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 5);

    UINT32 firstTsn = assoc.nextTsn - 5;

    // cumTsn acks first, gap acks third and fifth
    UINT16 gapStarts[] = {2, 4};
    UINT16 gapEnds[] = {2, 4};
    sendPeerSackWithGaps(firstTsn, gapStarts, gapEnds, 2);

    // 3 acked (first by cumTsn, third+fifth by gap) → 2 remain
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 2);
}

/******************************************************************************
 * RTO Behavior
 * Matches Rust: has_valid_initial_rto, can_do_exponential_backoff (multi-step),
 *   will_never_go_above_maximum_rto (stress), will_never_go_below_minimum_rto
 *****************************************************************************/

// Initial RTO matches SCTP_RTO_INITIAL_MS
TEST_F(SctpAssocApiTest, rto_initialValue)
{
    EXPECT_EQ(assoc.rtoMs, (UINT32) SCTP_RTO_INITIAL_MS);
}

// RTO doubles across 4 consecutive T3 timeouts
TEST_F(SctpAssocApiTest, rto_doublesAcrossMultipleTimeouts)
{
    driveToEstablished();
    BYTE payload[] = "a";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);

    UINT32 expected = SCTP_RTO_INITIAL_MS;
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(assoc.rtoMs, expected);
        UINT64 time = assoc.t3RtxExpiry + 1;
        sctpAssocCheckTimers(&assoc, time, mockOutboundNoop, 0);
        expected *= 2;
        if (expected > SCTP_RTO_MAX_MS) {
            expected = SCTP_RTO_MAX_MS;
        }
    }
    EXPECT_EQ(assoc.rtoMs, expected);
}

// RTO capped at SCTP_RTO_MAX_MS even after many timeouts (stress test)
TEST_F(SctpAssocApiTest, rto_cappedAfterManyTimeouts)
{
    driveToEstablished();
    BYTE payload[] = "a";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);

    for (int i = 0; i < 20; i++) {
        if (assoc.t3RtxExpiry == 0 || assoc.outstandingCount == 0) {
            break;
        }
        UINT64 time = assoc.t3RtxExpiry + 1;
        sctpAssocCheckTimers(&assoc, time, mockOutboundNoop, 0);
        EXPECT_LE(assoc.rtoMs, (UINT32) SCTP_RTO_MAX_MS);
    }
}

// RTO never goes below SCTP_RTO_MIN_MS
TEST_F(SctpAssocApiTest, rto_neverBelowMin)
{
    // At init
    EXPECT_GE(assoc.rtoMs, (UINT32) SCTP_RTO_MIN_MS);

    // After handshake
    driveToEstablished();
    EXPECT_GE(assoc.rtoMs, (UINT32) SCTP_RTO_MIN_MS);

    // After successful SACK
    BYTE payload[] = "a";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    sendPeerSack(assoc.nextTsn - 1);
    EXPECT_GE(assoc.rtoMs, (UINT32) SCTP_RTO_MIN_MS);
}

// Our simplified RTO: SACK does NOT reset rtoMs (no Jacobson SRTT)
TEST_F(SctpAssocApiTest, rto_notResetByAck)
{
    driveToEstablished();
    BYTE payload[] = "a";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);

    // Trigger T3 timeout → RTO doubles
    UINT64 time = assoc.t3RtxExpiry + 1;
    sctpAssocCheckTimers(&assoc, time, mockOutboundNoop, 0);
    UINT32 rtoAfterTimeout = assoc.rtoMs;
    EXPECT_EQ(rtoAfterTimeout, (UINT32)(SCTP_RTO_INITIAL_MS * 2));

    // Successful SACK should NOT reset rtoMs back to initial
    sendPeerSack(assoc.nextTsn - 1);
    EXPECT_EQ(assoc.rtoMs, rtoAfterTimeout);
}

/******************************************************************************
 * Fragment Reassembly — Receive Side (via packet injection)
 * Matches Rust: single_ordered_chunk_message, can_receive_large_unordered_chunk
 *****************************************************************************/

// Receive 3 in-order fragments → complete message delivered
TEST_F(SctpAssocApiTest, reassembly_threeFragmentsInOrder)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    BYTE frag1[] = "AAA";
    BYTE frag2[] = "BBB";
    BYTE frag3[] = "CCC";

    BYTE packet[256];

    // Fragment 1: BEGIN
    UINT32 off = buildDataPacketWithFlags(packet, 5000, 5000, assoc.myVerificationTag, base, 0, 0, SCTP_PPID_BINARY,
                                           SCTP_DATA_FLAG_BEGIN, frag1, 3);
    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 0); // not delivered yet

    // Fragment 2: MIDDLE (no flags)
    off = buildDataPacketWithFlags(packet, 5000, 5000, assoc.myVerificationTag, base + 1, 0, 0, SCTP_PPID_BINARY, 0, frag2, 3);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 0); // not delivered yet

    // Fragment 3: END
    off = buildDataPacketWithFlags(packet, 5000, 5000, assoc.myVerificationTag, base + 2, 0, 0, SCTP_PPID_BINARY,
                                    SCTP_DATA_FLAG_END, frag3, 3);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);

    // Complete message delivered: "AAABBBCCC"
    ASSERT_EQ(msg.count, (UINT32) 1);
    EXPECT_EQ(msg.messages[0].payloadLen, (UINT32) 9);
    BYTE expected[] = "AAABBBCCC";
    EXPECT_EQ(0, MEMCMP(msg.messages[0].payload, expected, 9));
}

// Duplicate fragment (same TSN) ignored, no double delivery
TEST_F(SctpAssocApiTest, reassembly_duplicateFragmentIgnored)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    BYTE payload[] = "hello";

    BYTE packet[256];

    // Single complete message (B+E)
    UINT32 off = buildDataPacket(packet, 5000, 5000, assoc.myVerificationTag, base, 0, 0, SCTP_PPID_BINARY, FALSE, payload, 5);

    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 1);

    // Send same TSN again — should be ignored
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 1); // no second delivery
}

// Reassembly: unordered 3-fragment message delivered correctly
TEST_F(SctpAssocApiTest, reassembly_unorderedThreeFragments)
{
    driveToEstablished();
    UINT32 base = assoc.peerCumulativeTsn + 1;
    BYTE frag1[] = "XX";
    BYTE frag2[] = "YY";
    BYTE frag3[] = "ZZ";

    BYTE packet[256];
    UINT8 uFlag = SCTP_DATA_FLAG_UNORDERED;

    // Fragment 1: BEGIN + UNORDERED
    UINT32 off = buildDataPacketWithFlags(packet, 5000, 5000, assoc.myVerificationTag, base, 0, 0, SCTP_PPID_BINARY,
                                           SCTP_DATA_FLAG_BEGIN | uFlag, frag1, 2);
    msg.reset();
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 0);

    // Fragment 2: MIDDLE + UNORDERED
    off = buildDataPacketWithFlags(packet, 5000, 5000, assoc.myVerificationTag, base + 1, 0, 0, SCTP_PPID_BINARY, uFlag, frag2, 2);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);
    EXPECT_EQ(msg.count, (UINT32) 0);

    // Fragment 3: END + UNORDERED
    off = buildDataPacketWithFlags(packet, 5000, 5000, assoc.myVerificationTag, base + 2, 0, 0, SCTP_PPID_BINARY,
                                    SCTP_DATA_FLAG_END | uFlag, frag3, 2);
    sctpAssocHandlePacket(&assoc, packet, off, mockOutboundNoop, 0, mockMessageCapture, (UINT64) &msg);

    ASSERT_EQ(msg.count, (UINT32) 1);
    EXPECT_EQ(msg.messages[0].payloadLen, (UINT32) 6);
    BYTE expected[] = "XXYYZZ";
    EXPECT_EQ(0, MEMCMP(msg.messages[0].payload, expected, 6));
}

/******************************************************************************
 * Outstanding / Retransmit
 * Matches Rust: acks_single_chunk (multiple), retransmit cumTsn tracking
 *****************************************************************************/

// 5 chunks acked by a single SACK → all outstanding freed
TEST_F(SctpAssocApiTest, outstanding_multipleAckedByOneSack)
{
    driveToEstablished();
    BYTE payload[] = "a";

    for (int i = 0; i < 5; i++) {
        sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload, 1, 0xFFFF, 0, mockOutboundNoop, 0);
    }
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 5);
    EXPECT_EQ(assoc.flightSize, (UINT32) 5);

    sendPeerSack(assoc.nextTsn - 1); // ack all 5

    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
    EXPECT_EQ(assoc.flightSize, (UINT32) 0);
    EXPECT_EQ(assoc.t3RtxExpiry, (UINT64) 0);
}

// Abandoned message FORWARD-TSN: after abandon, sending new data works with correct TSN
TEST_F(SctpAssocApiTest, outstanding_abandonThenSendNewData)
{
    driveToEstablished();

    // Send a message with maxRetransmits=0
    BYTE payload1[] = "ephemeral";
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload1, 9, 0, 0, mockOutboundNoop, 0);
    UINT32 tsnAfterFirst = assoc.nextTsn;
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);

    // T3 timeout → abandon → FORWARD-TSN
    UINT64 time = assoc.t3RtxExpiry + 1;
    cap.reset();
    sctpAssocCheckTimers(&assoc, time, mockOutboundCapture, (UINT64) &cap);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 0);
    EXPECT_NE(cap.findChunkType(SCTP_CHUNK_FORWARD_TSN), -1);

    // Send new data — should use the next TSN (no gap)
    BYTE payload2[] = "after";
    cap.reset();
    sctpAssocSend(&assoc, 0, SCTP_PPID_STRING, FALSE, payload2, 5, 0xFFFF, 0, mockOutboundCapture, (UINT64) &cap);
    EXPECT_EQ(assoc.nextTsn, tsnAfterFirst + 1);
    EXPECT_EQ(assoc.outstandingCount, (UINT32) 1);
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
#endif
