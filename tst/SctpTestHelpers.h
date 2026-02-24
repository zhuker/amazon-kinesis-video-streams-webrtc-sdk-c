#ifndef __SCTP_TEST_HELPERS_H__
#define __SCTP_TEST_HELPERS_H__

#include "WebRTCClientTestFixture.h"

#ifdef ENABLE_DATA_CHANNEL

#define SCTP_TEST_MAX_PACKETS  128
#define SCTP_TEST_MAX_MESSAGES 128
#define SCTP_TEST_MAX_PKT_LEN  SCTP_MAX_PACKET_SIZE

struct CapturedPacket {
    BYTE data[SCTP_TEST_MAX_PKT_LEN];
    UINT32 len;
};

struct CapturedMessage {
    UINT32 streamId;
    UINT32 ppid;
    BYTE payload[SCTP_MAX_REASSEMBLY_SIZE];
    UINT32 payloadLen;
};

struct PacketCapture {
    CapturedPacket packets[SCTP_TEST_MAX_PACKETS];
    UINT32 count;

    void reset()
    {
        count = 0;
    }

    // Find first packet that contains a chunk of the given type (chunk type is at byte 12 after common header)
    INT32 findChunkType(UINT8 chunkType)
    {
        for (UINT32 i = 0; i < count; i++) {
            if (packets[i].len > SCTP_COMMON_HEADER_SIZE && packets[i].data[SCTP_COMMON_HEADER_SIZE] == chunkType) {
                return (INT32) i;
            }
        }
        return -1;
    }
};

struct MessageCapture {
    CapturedMessage messages[SCTP_TEST_MAX_MESSAGES];
    UINT32 count;

    void reset()
    {
        count = 0;
    }
};

// C-linkage callbacks for SctpAssocOutboundPacketFn / SctpAssocMessageFn
extern "C" {
static VOID mockOutboundCapture(UINT64 customData, PBYTE pPacket, UINT32 packetLen)
{
    PacketCapture* cap = (PacketCapture*) customData;
    if (cap != NULL && cap->count < SCTP_TEST_MAX_PACKETS && packetLen <= SCTP_TEST_MAX_PKT_LEN) {
        MEMCPY(cap->packets[cap->count].data, pPacket, packetLen);
        cap->packets[cap->count].len = packetLen;
        cap->count++;
    }
}

static VOID mockMessageCapture(UINT64 customData, UINT32 streamId, UINT32 ppid, PBYTE pPayload, UINT32 payloadLen)
{
    MessageCapture* cap = (MessageCapture*) customData;
    if (cap != NULL && cap->count < SCTP_TEST_MAX_MESSAGES) {
        cap->messages[cap->count].streamId = streamId;
        cap->messages[cap->count].ppid = ppid;
        UINT32 copyLen = payloadLen;
        if (copyLen > SCTP_MAX_REASSEMBLY_SIZE) {
            copyLen = SCTP_MAX_REASSEMBLY_SIZE;
        }
        if (copyLen > 0) {
            MEMCPY(cap->messages[cap->count].payload, pPayload, copyLen);
        }
        cap->messages[cap->count].payloadLen = payloadLen;
        cap->count++;
    }
}

static VOID mockOutboundNoop(UINT64 customData, PBYTE pPacket, UINT32 packetLen)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(pPacket);
    UNUSED_PARAM(packetLen);
}

static VOID mockMessageNoop(UINT64 customData, UINT32 streamId, UINT32 ppid, PBYTE pPayload, UINT32 payloadLen)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(streamId);
    UNUSED_PARAM(ppid);
    UNUSED_PARAM(pPayload);
    UNUSED_PARAM(payloadLen);
}
}

// Two-association test harness
struct TwoAssocHarness {
    SctpAssociation assocA;
    SctpAssociation assocB;
    PacketCapture capA; // packets sent by A
    PacketCapture capB; // packets sent by B
    MessageCapture msgA; // messages delivered to A
    MessageCapture msgB; // messages delivered to B

    void init(UINT16 portA = 5000, UINT16 portB = 5000, UINT16 mtu = 1188)
    {
        capA.reset();
        capB.reset();
        msgA.reset();
        msgB.reset();
        sctpAssocInit(&assocA, portA, portB, mtu);
        sctpAssocInit(&assocB, portB, portA, mtu);
    }

    void cleanup()
    {
        sctpAssocCleanup(&assocA);
        sctpAssocCleanup(&assocB);
    }

    // Deliver all captured packets from A to B, and B to A. Returns total packets exchanged.
    UINT32 exchangePackets()
    {
        UINT32 total = 0;
        // Snapshot counts before exchange to avoid infinite loop if new packets are generated
        UINT32 aCount = capA.count;
        UINT32 bCount = capB.count;

        // Temporary storage for the current packets
        PacketCapture tmpA, tmpB;
        tmpA.count = 0;
        tmpB.count = 0;

        // Copy current packets to temp
        for (UINT32 i = 0; i < aCount; i++) {
            tmpA.packets[i] = capA.packets[i];
        }
        tmpA.count = aCount;

        for (UINT32 i = 0; i < bCount; i++) {
            tmpB.packets[i] = capB.packets[i];
        }
        tmpB.count = bCount;

        // Reset captures (new packets from HandlePacket will go here)
        capA.reset();
        capB.reset();

        // Feed A's output to B
        for (UINT32 i = 0; i < tmpA.count; i++) {
            sctpAssocHandlePacket(&assocB, tmpA.packets[i].data, tmpA.packets[i].len, mockOutboundCapture, (UINT64) &capB, mockMessageCapture,
                                  (UINT64) &msgB);
            total++;
        }

        // Feed B's output to A
        for (UINT32 i = 0; i < tmpB.count; i++) {
            sctpAssocHandlePacket(&assocA, tmpB.packets[i].data, tmpB.packets[i].len, mockOutboundCapture, (UINT64) &capA, mockMessageCapture,
                                  (UINT64) &msgA);
            total++;
        }

        return total;
    }

    // Run the full 4-way handshake to get both to ESTABLISHED
    void completeHandshake()
    {
        capA.reset();
        capB.reset();

        // A sends INIT
        sctpAssocConnect(&assocA, mockOutboundCapture, (UINT64) &capA);

        // B also sends INIT (simultaneous open)
        sctpAssocConnect(&assocB, mockOutboundCapture, (UINT64) &capB);

        // Exchange: A's INIT→B, B's INIT→A → each responds with INIT-ACK
        exchangePackets();

        // Exchange: INIT-ACKs → each responds with COOKIE-ECHO
        exchangePackets();

        // Exchange: COOKIE-ECHOs → each responds with COOKIE-ACK
        exchangePackets();

        // Exchange: COOKIE-ACKs → both become ESTABLISHED
        exchangePackets();

        // Clear captures for the actual test
        capA.reset();
        capB.reset();
        msgA.reset();
        msgB.reset();
    }
};

// Packet builder helpers — build a complete SCTP packet ready for injection

static UINT32 buildDataPacket(PBYTE buf, UINT16 srcPort, UINT16 dstPort, UINT32 vtag, UINT32 tsn, UINT16 sid, UINT16 ssn, UINT32 ppid,
                               BOOL unordered, PBYTE payload, UINT32 payloadLen)
{
    UINT32 off = sctpWriteCommonHeader(buf, srcPort, dstPort, vtag);
    off += sctpWriteDataChunk(buf + off, tsn, sid, ssn, ppid, unordered, payload, payloadLen);
    sctpFinalizePacket(buf, off);
    return off;
}

static UINT32 buildSackPacket(PBYTE buf, UINT16 srcPort, UINT16 dstPort, UINT32 vtag, UINT32 cumTsn, UINT32 arwnd, PUINT16 gapStarts,
                               PUINT16 gapEnds, UINT16 numGaps)
{
    UINT32 off = sctpWriteCommonHeader(buf, srcPort, dstPort, vtag);
    off += sctpWriteSackChunk(buf + off, cumTsn, arwnd, gapStarts, gapEnds, numGaps);
    sctpFinalizePacket(buf, off);
    return off;
}

// Build a DATA packet with explicit flags (for fragment testing: B, M, E flags)
static UINT32 buildDataPacketWithFlags(PBYTE buf, UINT16 srcPort, UINT16 dstPort, UINT32 vtag, UINT32 tsn, UINT16 sid, UINT16 ssn,
                                        UINT32 ppid, UINT8 flags, PBYTE payload, UINT32 payloadLen)
{
    UINT32 off = sctpWriteCommonHeader(buf, srcPort, dstPort, vtag);
    UINT32 chunkLen = SCTP_DATA_HEADER_SIZE + payloadLen;
    buf[off + 0] = SCTP_CHUNK_DATA;
    buf[off + 1] = flags;
    putUnalignedInt16BigEndian(buf + off + 2, (INT16) chunkLen);
    putUnalignedInt32BigEndian(buf + off + 4, (INT32) tsn);
    putUnalignedInt16BigEndian(buf + off + 8, (INT16) sid);
    putUnalignedInt16BigEndian(buf + off + 10, (INT16) ssn);
    putUnalignedInt32BigEndian(buf + off + 12, (INT32) ppid);
    if (payloadLen > 0) {
        MEMCPY(buf + off + SCTP_DATA_HEADER_SIZE, payload, payloadLen);
    }
    off += SCTP_PAD4(chunkLen);
    sctpFinalizePacket(buf, off);
    return off;
}

static UINT32 buildForwardTsnPacket(PBYTE buf, UINT16 srcPort, UINT16 dstPort, UINT32 vtag, UINT32 newCumTsn)
{
    UINT32 off = sctpWriteCommonHeader(buf, srcPort, dstPort, vtag);
    off += sctpWriteForwardTsnChunk(buf + off, newCumTsn);
    sctpFinalizePacket(buf, off);
    return off;
}

#endif // ENABLE_DATA_CHANNEL
#endif // __SCTP_TEST_HELPERS_H__
