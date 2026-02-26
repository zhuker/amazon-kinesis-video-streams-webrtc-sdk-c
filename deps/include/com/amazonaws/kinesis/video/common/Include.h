/**
 * Minimal stub of producer-c common/Include.h for self-contained builds (signaling OFF).
 * Provides only the definitions needed by non-signaling WebRTC code.
 * AWS-specific types and macros are guarded by ENABLE_SIGNALING.
 */
#ifndef __KINESIS_VIDEO_COMMON_INCLUDE__
#define __KINESIS_VIDEO_COMMON_INCLUDE__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

////////////////////////////////////////////////////
// Public headers
////////////////////////////////////////////////////
#include <com/amazonaws/kinesis/video/client/Include.h>
#ifndef JSMN_HEADER
#define JSMN_HEADER
#endif
#include <com/amazonaws/kinesis/video/common/jsmn.h>

////////////////////////////////////////////////////
// Status codes used by non-signaling code
////////////////////////////////////////////////////
#define STATUS_COMMON_PRODUCER_BASE                         0x15000000
#define STATUS_INVALID_API_CALL_RETURN_JSON                 STATUS_COMMON_PRODUCER_BASE + 0x0000000c
#define STATUS_HMAC_GENERATION_ERROR                        STATUS_COMMON_PRODUCER_BASE + 0x00000010

////////////////////////////////////////////////////
// Lengths used by non-signaling code
////////////////////////////////////////////////////
#define MAX_JSON_TOKEN_COUNT    100

#ifdef __cplusplus
}
#endif

#endif /* __KINESIS_VIDEO_COMMON_INCLUDE__ */
