#ifndef __KINESIS_VIDEO_WEBRTC_SCTP_INT__
#define __KINESIS_VIDEO_WEBRTC_SCTP_INT__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Pad length to 4-byte boundary
#define SCTP_PAD4(x) (((x) + 3) & ~3u)

/******************************************************************************
 * SCTP chunk types (RFC 9260 + extensions)
 *****************************************************************************/
#define SCTP_CHUNK_DATA             0
#define SCTP_CHUNK_INIT             1
#define SCTP_CHUNK_INIT_ACK         2
#define SCTP_CHUNK_SACK             3
#define SCTP_CHUNK_HEARTBEAT        4
#define SCTP_CHUNK_HEARTBEAT_ACK    5
#define SCTP_CHUNK_ABORT            6
#define SCTP_CHUNK_SHUTDOWN         7
#define SCTP_CHUNK_SHUTDOWN_ACK     8
#define SCTP_CHUNK_ERROR            9
#define SCTP_CHUNK_COOKIE_ECHO      10
#define SCTP_CHUNK_COOKIE_ACK       11
#define SCTP_CHUNK_SHUTDOWN_COMPLETE 14
#define SCTP_CHUNK_FORWARD_TSN      192 // RFC 3758

/******************************************************************************
 * DATA chunk flags (RFC 9260 §3.3.1)
 *****************************************************************************/
#define SCTP_DATA_FLAG_END       0x01 // E bit
#define SCTP_DATA_FLAG_BEGIN     0x02 // B bit
#define SCTP_DATA_FLAG_UNORDERED 0x04 // U bit

/******************************************************************************
 * SCTP INIT/INIT-ACK optional parameter types
 *****************************************************************************/
#define SCTP_PARAM_STATE_COOKIE            0x0007
#define SCTP_PARAM_SUPPORTED_EXTENSIONS    0x8008
#define SCTP_PARAM_FORWARD_TSN_SUPPORTED   0xC000 // RFC 3758

/******************************************************************************
 * Sizes
 *****************************************************************************/
#define SCTP_COMMON_HEADER_SIZE  12
#define SCTP_CHUNK_HEADER_SIZE   4
#define SCTP_DATA_HEADER_SIZE    16 // chunk hdr(4) + TSN(4) + SID(2) + SSN(2) + PPID(4)
#define SCTP_INIT_HEADER_SIZE    20 // chunk hdr(4) + tag(4) + arwnd(4) + os(2) + is(2) + tsn(4)
#define SCTP_SACK_HEADER_SIZE    16 // chunk hdr(4) + cumTsn(4) + arwnd(4) + nGaps(2) + nDups(2)
#define SCTP_COOKIE_ECHO_HEADER_SIZE 4
#define SCTP_COOKIE_ACK_SIZE     4
#define SCTP_SHUTDOWN_SIZE       8  // chunk hdr(4) + cumTsn(4)
#define SCTP_SHUTDOWN_ACK_SIZE   4
#define SCTP_SHUTDOWN_COMPLETE_SIZE 4
#define SCTP_FORWARD_TSN_HEADER_SIZE 8 // chunk hdr(4) + newCumTsn(4)

#define SCTP_MAX_PACKET_SIZE    1200
#define SCTP_DEFAULT_ARWND      131072
#define SCTP_INITIAL_CWND_MTUS  3
#define SCTP_MAX_STREAMS        300

/******************************************************************************
 * Outstanding / receive buffer limits
 *****************************************************************************/
#define SCTP_MAX_OUTSTANDING    2048
#define SCTP_MAX_RECEIVED       2048
#define SCTP_MAX_GAP_BLOCKS     128
#define SCTP_MAX_REASSEMBLY_SIZE 65536 // 64KB max reassembled message

/******************************************************************************
 * Cookie
 *****************************************************************************/
#define SCTP_COOKIE_SIZE        44
#define SCTP_COOKIE_MAGIC1      0x4B565343 // "KVSC"
#define SCTP_COOKIE_MAGIC2      0x44415441 // "DATA"

/******************************************************************************
 * Timer defaults (milliseconds)
 *****************************************************************************/
#define SCTP_RTO_INITIAL_MS     1000
#define SCTP_RTO_MIN_MS         1000
#define SCTP_RTO_MAX_MS         60000
#define SCTP_MAX_INIT_RETRANS   8
#define SCTP_MAX_DATA_RETRANS   10

/******************************************************************************
 * Association states (RFC 9260 §4)
 *****************************************************************************/
typedef enum {
    SCTP_ASSOC_CLOSED = 0,
    SCTP_ASSOC_COOKIE_WAIT,
    SCTP_ASSOC_COOKIE_ECHOED,
    SCTP_ASSOC_ESTABLISHED,
    SCTP_ASSOC_SHUTDOWN_PENDING,
    SCTP_ASSOC_SHUTDOWN_SENT,
    SCTP_ASSOC_SHUTDOWN_ACK_SENT,
} SctpAssocState;

/******************************************************************************
 * Cookie format (44 bytes) — sent in INIT-ACK, echoed in COOKIE-ECHO.
 * No HMAC needed since DTLS provides authentication.
 *****************************************************************************/
#pragma pack(push, 1)
typedef struct {
    UINT32 magic1;
    UINT32 magic2;
    UINT32 peerTag;
    UINT32 myTag;
    UINT32 peerInitialTsn;
    UINT32 myInitialTsn;
    UINT32 peerArwnd;
    UINT64 tieTag;
    UINT16 numInStreams;
    UINT16 numOutStreams;
    UINT32 reserved;
} SctpCookie;
#pragma pack(pop)

/******************************************************************************
 * Outstanding DATA chunk (send queue entry)
 *****************************************************************************/
typedef struct {
    UINT32 tsn;
    UINT16 streamId;
    UINT16 ssn;
    UINT32 ppid;
    PBYTE payload;
    UINT32 payloadLen;
    BOOL unordered;
    UINT64 sentTime;          // for RTT measurement (ms since epoch)
    UINT32 retransmitCount;
    BOOL acked;
    BOOL abandoned;           // PR-SCTP: message expired
    UINT16 maxRetransmits;    // 0xFFFF = unlimited
    UINT64 lifetimeMs;        // 0 = unlimited
    UINT64 creationTime;      // for lifetime check (ms since epoch)
    BOOL inUse;               // slot is occupied
} SctpOutstandingData;

/******************************************************************************
 * Received DATA chunk (for reassembly)
 *****************************************************************************/
typedef struct {
    UINT32 tsn;
    UINT16 streamId;
    UINT16 ssn;
    UINT32 ppid;
    PBYTE payload;
    UINT32 payloadLen;
    BOOL isBegin;
    BOOL isEnd;
    BOOL isUnordered;
    BOOL inUse;
} SctpReceivedData;

/******************************************************************************
 * Callback types for association outbound packets and inbound messages
 *****************************************************************************/
typedef VOID (*SctpAssocOutboundPacketFn)(UINT64 customData, PBYTE pPacket, UINT32 packetLen);
typedef VOID (*SctpAssocMessageFn)(UINT64 customData, UINT32 streamId, UINT32 ppid, PBYTE pPayload, UINT32 payloadLen);

/******************************************************************************
 * SCTP Association state
 *****************************************************************************/
typedef struct {
    SctpAssocState state;
    UINT16 localPort;
    UINT16 remotePort;
    UINT32 myVerificationTag;
    UINT32 peerVerificationTag;

    // TSN tracking (send side)
    UINT32 nextTsn;
    UINT32 cumulativeAckTsn;  // last TSN acked by peer (initially nextTsn - 1)
    SctpOutstandingData outstanding[SCTP_MAX_OUTSTANDING];
    UINT32 outstandingCount;

    // SSN tracking per stream (send side)
    UINT16 nextSsn[SCTP_MAX_STREAMS];

    // Receive tracking
    UINT32 peerCumulativeTsn;       // highest in-order TSN received from peer
    BOOL   peerCumulativeTsnValid;  // have we received any DATA yet?
    UINT32 receivedTsns[SCTP_MAX_RECEIVED]; // out-of-order TSNs
    UINT32 receivedTsnCount;
    BOOL   needSack;

    // Congestion control
    UINT32 cwnd;
    UINT32 ssthresh;
    UINT32 peerArwnd;
    UINT32 flightSize;  // bytes in flight
    UINT16 mtu;

    // Timers (absolute time in ms)
    UINT64 t1InitExpiry;
    UINT64 t3RtxExpiry;
    UINT32 initRetransmitCount;
    UINT32 rtoMs;
    UINT32 srttMs;

    // Outbound packet buffer
    BYTE outPacket[SCTP_MAX_PACKET_SIZE];
    UINT32 outPacketLen;

    // Cookie state (for handshake)
    SctpCookie cookie;
    BYTE cookieEchoData[SCTP_COOKIE_SIZE]; // raw cookie bytes for retransmit
    BOOL cookieEchoValid;

    // Tie tag for simultaneous open
    UINT64 tieTag;

    // Advanced.Peer.Ack.Point for PR-SCTP (FORWARD-TSN)
    UINT32 advancedPeerAckPoint;

    // Fragment reassembly (receive side)
    PBYTE reassemblyBuf;
    UINT32 reassemblyLen;
    UINT16 reassemblyStreamId;
    UINT16 reassemblySsn;
    UINT32 reassemblyPpid;
    BOOL reassemblyInProgress;

    // Queued messages (sent before ESTABLISHED)
#define SCTP_MAX_QUEUED_MESSAGES 32
#define SCTP_MAX_QUEUED_PAYLOAD  1200
    struct {
        UINT16 streamId;
        UINT32 ppid;
        BOOL unordered;
        UINT16 maxRetransmits;
        UINT64 lifetimeMs;
        BYTE payload[SCTP_MAX_QUEUED_PAYLOAD];
        UINT32 payloadLen;
        BOOL inUse;
    } sendQueue[SCTP_MAX_QUEUED_MESSAGES];
    UINT32 sendQueueCount;
} SctpAssociation, *PSctpAssociation;

#ifdef __cplusplus
}
#endif

#endif /* __KINESIS_VIDEO_WEBRTC_SCTP_INT__ */
