/*******************************************
PCAP dump internal include file
Captures unencrypted RTP/RTCP packets to
standard pcap format for offline analysis.
Wraps each packet in Ethernet/IPv4/UDP so
Wireshark auto-detects RTP/RTCP dissectors.
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_PCAP_DUMP__
#define __KINESIS_VIDEO_WEBRTC_CLIENT_PCAP_DUMP__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// PCAP file format constants
#define PCAP_LINKTYPE_ETHERNET 1
#define PCAP_SNAPLEN           65535

// Ethernet + IPv4 + UDP overhead
#define PCAP_ETHERNET_HEADER_SIZE 14
#define PCAP_IPV4_HEADER_SIZE     20
#define PCAP_UDP_HEADER_SIZE      8
#define PCAP_TRANSPORT_OVERHEAD   (PCAP_ETHERNET_HEADER_SIZE + PCAP_IPV4_HEADER_SIZE + PCAP_UDP_HEADER_SIZE)

// Fake UDP ports — Wireshark applies RTP/RTCP dissectors on these
#define PCAP_FAKE_RTP_PORT  5004
#define PCAP_FAKE_RTCP_PORT 5005

// Ethernet + IPv4 overhead (no UDP — used for SCTP with IP proto 132)
#define PCAP_IP_TRANSPORT_OVERHEAD (PCAP_ETHERNET_HEADER_SIZE + PCAP_IPV4_HEADER_SIZE)

typedef enum {
    PCAP_PACKET_DIRECTION_SEND = 0,
    PCAP_PACKET_DIRECTION_RECV = 1,
} PCAP_PACKET_DIRECTION;

typedef struct __PcapDumpContext {
    FILE* pFile;
    MUTEX lock;
    BOOL initialized;
} PcapDumpContext, *PPcapDumpContext;

STATUS pcapDumpCreate(PCHAR filePath, PPcapDumpContext* ppCtx);
STATUS pcapDumpWritePacket(PPcapDumpContext pCtx, PBYTE pBuffer, UINT32 bufferLen, BOOL isRtcp, PCAP_PACKET_DIRECTION direction);
STATUS pcapDumpWriteSctpPacket(PPcapDumpContext pCtx, PBYTE pBuffer, UINT32 bufferLen, PCAP_PACKET_DIRECTION direction);
STATUS pcapDumpFree(PPcapDumpContext* ppCtx);

#ifdef __cplusplus
}
#endif
#endif /* __KINESIS_VIDEO_WEBRTC_CLIENT_PCAP_DUMP__ */
