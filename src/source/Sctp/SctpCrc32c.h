#ifndef __KINESIS_VIDEO_WEBRTC_SCTP_CRC32C__
#define __KINESIS_VIDEO_WEBRTC_SCTP_CRC32C__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>

/**
 * Compute CRC32c (Castagnoli) checksum for SCTP.
 * Uses polynomial 0x1EDC6F41 (reflected: 0x82F63B78).
 * This is different from the standard CRC32 (ISO polynomial 0xEDB88320) used by kvspic.
 */
UINT32 sctpCrc32c(PBYTE pData, UINT32 length);

#ifdef __cplusplus
}
#endif

#endif /* __KINESIS_VIDEO_WEBRTC_SCTP_CRC32C__ */
