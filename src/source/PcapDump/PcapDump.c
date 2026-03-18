/**
 * PCAP file dump for unencrypted RTP/RTCP packets.
 *
 * Each packet is wrapped in a fake Ethernet/IPv4/UDP frame so that
 * Wireshark recognises the file immediately and applies the correct
 * RTP/RTCP dissectors based on the UDP port numbers.
 *
 * Send packets use 10.0.0.1 -> 10.0.0.2, receive packets use
 * 10.0.0.2 -> 10.0.0.1, making it easy to set display filters.
 */
#define LOG_CLASS "PcapDump"
#include "../Include_i.h"

// Fake MAC addresses (locally-administered)
static const BYTE PCAP_SRC_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const BYTE PCAP_DST_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

// Fake IPs in network byte order: 10.0.0.1 and 10.0.0.2
#define PCAP_IP_LOCAL  0x0100000AU
#define PCAP_IP_REMOTE 0x0200000AU

static UINT16 ipChecksum(const BYTE* pData, UINT32 len)
{
    UINT32 sum = 0;
    UINT32 i;
    for (i = 0; i + 1 < len; i += 2) {
        sum += (UINT32) pData[i] << 8 | pData[i + 1];
    }
    if (i < len) {
        sum += (UINT32) pData[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (UINT16) ~sum;
}

STATUS pcapDumpCreate(PCHAR filePath, PPcapDumpContext* ppCtx)
{
    STATUS retStatus = STATUS_SUCCESS;
    PPcapDumpContext pCtx = NULL;
    BYTE globalHeader[24];

    CHK(filePath != NULL && ppCtx != NULL, STATUS_NULL_ARG);

    pCtx = (PPcapDumpContext) MEMCALLOC(1, SIZEOF(PcapDumpContext));
    CHK(pCtx != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pCtx->lock = MUTEX_CREATE(TRUE);

    pCtx->pFile = fopen(filePath, "wb");
    CHK(pCtx->pFile != NULL, STATUS_OPEN_FILE_FAILED);

    // PCAP global header (24 bytes, little-endian)
    // magic_number = 0xA1B2C3D4
    globalHeader[0] = 0xD4;
    globalHeader[1] = 0xC3;
    globalHeader[2] = 0xB2;
    globalHeader[3] = 0xA1;
    // version_major = 2
    globalHeader[4] = 0x02;
    globalHeader[5] = 0x00;
    // version_minor = 4
    globalHeader[6] = 0x04;
    globalHeader[7] = 0x00;
    // thiszone = 0
    MEMSET(globalHeader + 8, 0, 4);
    // sigfigs = 0
    MEMSET(globalHeader + 12, 0, 4);
    // snaplen = 65535
    globalHeader[16] = 0xFF;
    globalHeader[17] = 0xFF;
    globalHeader[18] = 0x00;
    globalHeader[19] = 0x00;
    // network = 1 (LINKTYPE_ETHERNET)
    globalHeader[20] = 0x01;
    globalHeader[21] = 0x00;
    globalHeader[22] = 0x00;
    globalHeader[23] = 0x00;

    CHK(fwrite(globalHeader, 1, SIZEOF(globalHeader), pCtx->pFile) == SIZEOF(globalHeader), STATUS_WRITE_TO_FILE_FAILED);
    fflush(pCtx->pFile);

    pCtx->initialized = TRUE;
    *ppCtx = pCtx;
    pCtx = NULL;

CleanUp:
    if (pCtx != NULL) {
        if (pCtx->pFile != NULL) {
            fclose(pCtx->pFile);
        }
        if (IS_VALID_MUTEX_VALUE(pCtx->lock)) {
            MUTEX_FREE(pCtx->lock);
        }
        MEMFREE(pCtx);
    }
    return retStatus;
}

static VOID buildEthIpUdpHeader(BYTE* pHeader, UINT32 payloadLen, UINT16 srcPort, UINT16 dstPort, UINT32 srcIp, UINT32 dstIp)
{
    UINT16 ipTotalLen = (UINT16) (PCAP_IPV4_HEADER_SIZE + PCAP_UDP_HEADER_SIZE + payloadLen);
    UINT16 udpLen = (UINT16) (PCAP_UDP_HEADER_SIZE + payloadLen);
    UINT16 cksum;
    BYTE* ip;
    BYTE* udp;

    // Ethernet header (14 bytes)
    MEMCPY(pHeader, PCAP_DST_MAC, 6);
    MEMCPY(pHeader + 6, PCAP_SRC_MAC, 6);
    pHeader[12] = 0x08; // EtherType = 0x0800 (IPv4)
    pHeader[13] = 0x00;

    // IPv4 header (20 bytes)
    ip = pHeader + PCAP_ETHERNET_HEADER_SIZE;
    ip[0] = 0x45;                     // Version=4, IHL=5
    ip[1] = 0x00;                     // DSCP/ECN
    ip[2] = (BYTE) (ipTotalLen >> 8); // Total length (big-endian)
    ip[3] = (BYTE) (ipTotalLen & 0xFF);
    ip[4] = 0x00; // Identification
    ip[5] = 0x00;
    ip[6] = 0x40; // Flags: Don't Fragment
    ip[7] = 0x00;
    ip[8] = 0x40;  // TTL = 64
    ip[9] = 0x11;  // Protocol = UDP
    ip[10] = 0x00; // Checksum placeholder
    ip[11] = 0x00;
    MEMCPY(ip + 12, &srcIp, 4);
    MEMCPY(ip + 16, &dstIp, 4);
    cksum = ipChecksum(ip, PCAP_IPV4_HEADER_SIZE);
    ip[10] = (BYTE) (cksum >> 8);
    ip[11] = (BYTE) (cksum & 0xFF);

    // UDP header (8 bytes)
    udp = ip + PCAP_IPV4_HEADER_SIZE;
    udp[0] = (BYTE) (srcPort >> 8);
    udp[1] = (BYTE) (srcPort & 0xFF);
    udp[2] = (BYTE) (dstPort >> 8);
    udp[3] = (BYTE) (dstPort & 0xFF);
    udp[4] = (BYTE) (udpLen >> 8);
    udp[5] = (BYTE) (udpLen & 0xFF);
    udp[6] = 0x00; // Checksum = 0 (optional in IPv4)
    udp[7] = 0x00;
}

static VOID buildEthIpHeader(BYTE* pHeader, UINT32 payloadLen, UINT8 protocol, UINT32 srcIp, UINT32 dstIp)
{
    UINT16 ipTotalLen = (UINT16) (PCAP_IPV4_HEADER_SIZE + payloadLen);
    UINT16 cksum;
    BYTE* ip;

    // Ethernet header (14 bytes)
    MEMCPY(pHeader, PCAP_DST_MAC, 6);
    MEMCPY(pHeader + 6, PCAP_SRC_MAC, 6);
    pHeader[12] = 0x08;
    pHeader[13] = 0x00;

    // IPv4 header (20 bytes)
    ip = pHeader + PCAP_ETHERNET_HEADER_SIZE;
    ip[0] = 0x45;
    ip[1] = 0x00;
    ip[2] = (BYTE) (ipTotalLen >> 8);
    ip[3] = (BYTE) (ipTotalLen & 0xFF);
    ip[4] = 0x00;
    ip[5] = 0x00;
    ip[6] = 0x40;
    ip[7] = 0x00;
    ip[8] = 0x40;
    ip[9] = protocol;
    ip[10] = 0x00;
    ip[11] = 0x00;
    MEMCPY(ip + 12, &srcIp, 4);
    MEMCPY(ip + 16, &dstIp, 4);
    cksum = ipChecksum(ip, PCAP_IPV4_HEADER_SIZE);
    ip[10] = (BYTE) (cksum >> 8);
    ip[11] = (BYTE) (cksum & 0xFF);
}

static STATUS pcapDumpWriteRaw(PPcapDumpContext pCtx, BYTE* pTransportHeader, UINT32 transportHeaderLen, PBYTE pBuffer, UINT32 bufferLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    BYTE recordHeader[16];
    UINT64 now, sec, usec;
    UINT32 totalLen;

    totalLen = transportHeaderLen + bufferLen;

    now = GETTIME();
    sec = now / (10 * 1000 * 1000);
    usec = (now / 10) % 1000000;

    recordHeader[0] = (BYTE) (sec & 0xFF);
    recordHeader[1] = (BYTE) ((sec >> 8) & 0xFF);
    recordHeader[2] = (BYTE) ((sec >> 16) & 0xFF);
    recordHeader[3] = (BYTE) ((sec >> 24) & 0xFF);
    recordHeader[4] = (BYTE) (usec & 0xFF);
    recordHeader[5] = (BYTE) ((usec >> 8) & 0xFF);
    recordHeader[6] = (BYTE) ((usec >> 16) & 0xFF);
    recordHeader[7] = (BYTE) ((usec >> 24) & 0xFF);
    recordHeader[8] = (BYTE) (totalLen & 0xFF);
    recordHeader[9] = (BYTE) ((totalLen >> 8) & 0xFF);
    recordHeader[10] = (BYTE) ((totalLen >> 16) & 0xFF);
    recordHeader[11] = (BYTE) ((totalLen >> 24) & 0xFF);
    recordHeader[12] = recordHeader[8];
    recordHeader[13] = recordHeader[9];
    recordHeader[14] = recordHeader[10];
    recordHeader[15] = recordHeader[11];

    MUTEX_LOCK(pCtx->lock);
    if (fwrite(recordHeader, 1, SIZEOF(recordHeader), pCtx->pFile) != SIZEOF(recordHeader) ||
        fwrite(pTransportHeader, 1, transportHeaderLen, pCtx->pFile) != transportHeaderLen ||
        fwrite(pBuffer, 1, bufferLen, pCtx->pFile) != bufferLen) {
        retStatus = STATUS_WRITE_TO_FILE_FAILED;
    }
    MUTEX_UNLOCK(pCtx->lock);

    return retStatus;
}

STATUS pcapDumpWritePacket(PPcapDumpContext pCtx, PBYTE pBuffer, UINT32 bufferLen, BOOL isRtcp, PCAP_PACKET_DIRECTION direction)
{
    STATUS retStatus = STATUS_SUCCESS;
    BYTE transportHeader[PCAP_TRANSPORT_OVERHEAD];
    UINT16 srcPort, dstPort;
    UINT32 srcIp, dstIp;

    CHK(pCtx != NULL && pCtx->initialized && pBuffer != NULL && bufferLen > 0, STATUS_NULL_ARG);

    srcPort = isRtcp ? PCAP_FAKE_RTCP_PORT : PCAP_FAKE_RTP_PORT;
    dstPort = srcPort;

    if (direction == PCAP_PACKET_DIRECTION_SEND) {
        srcIp = PCAP_IP_LOCAL;
        dstIp = PCAP_IP_REMOTE;
    } else {
        srcIp = PCAP_IP_REMOTE;
        dstIp = PCAP_IP_LOCAL;
    }

    buildEthIpUdpHeader(transportHeader, bufferLen, srcPort, dstPort, srcIp, dstIp);
    retStatus = pcapDumpWriteRaw(pCtx, transportHeader, SIZEOF(transportHeader), pBuffer, bufferLen);

CleanUp:
    return retStatus;
}

STATUS pcapDumpWriteSctpPacket(PPcapDumpContext pCtx, PBYTE pBuffer, UINT32 bufferLen, PCAP_PACKET_DIRECTION direction)
{
    STATUS retStatus = STATUS_SUCCESS;
    BYTE transportHeader[PCAP_IP_TRANSPORT_OVERHEAD];
    UINT32 srcIp, dstIp;

    CHK(pCtx != NULL && pCtx->initialized && pBuffer != NULL && bufferLen > 0, STATUS_NULL_ARG);

    if (direction == PCAP_PACKET_DIRECTION_SEND) {
        srcIp = PCAP_IP_LOCAL;
        dstIp = PCAP_IP_REMOTE;
    } else {
        srcIp = PCAP_IP_REMOTE;
        dstIp = PCAP_IP_LOCAL;
    }

    buildEthIpHeader(transportHeader, bufferLen, 132, srcIp, dstIp); // IP protocol 132 = SCTP
    retStatus = pcapDumpWriteRaw(pCtx, transportHeader, SIZEOF(transportHeader), pBuffer, bufferLen);

CleanUp:
    return retStatus;
}

STATUS pcapDumpFree(PPcapDumpContext* ppCtx)
{
    STATUS retStatus = STATUS_SUCCESS;
    PPcapDumpContext pCtx;

    CHK(ppCtx != NULL, STATUS_NULL_ARG);
    pCtx = *ppCtx;
    CHK(pCtx != NULL, retStatus);

    if (pCtx->pFile != NULL) {
        fflush(pCtx->pFile);
        fclose(pCtx->pFile);
        pCtx->pFile = NULL;
    }
    if (IS_VALID_MUTEX_VALUE(pCtx->lock)) {
        MUTEX_FREE(pCtx->lock);
    }
    MEMFREE(pCtx);
    *ppCtx = NULL;

CleanUp:
    return retStatus;
}
