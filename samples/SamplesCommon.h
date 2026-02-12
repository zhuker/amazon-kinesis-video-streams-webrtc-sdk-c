/*******************************************
Shared include file for samples that don't
depend on AWS signaling (WHEP/WHIP servers,
standalone tools, etc.)
*******************************************/
#ifndef __KINESIS_VIDEO_SAMPLE_COMMON_INCLUDE__
#define __KINESIS_VIDEO_SAMPLE_COMMON_INCLUDE__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>

#define NUMBER_OF_H264_FRAME_FILES  1500
#define NUMBER_OF_H265_FRAME_FILES  1500
#define NUMBER_OF_OPUS_FRAME_FILES  618
#define DEFAULT_FPS_VALUE           25

#define SAMPLE_AUDIO_FRAME_DURATION (20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)
#define SAMPLE_VIDEO_FRAME_DURATION (HUNDREDS_OF_NANOS_IN_A_SECOND / DEFAULT_FPS_VALUE)

STATUS readFrameFromDisk(PBYTE, PUINT32, PCHAR);

#ifdef __cplusplus
}
#endif
#endif /* __KINESIS_VIDEO_SAMPLE_COMMON_INCLUDE__ */
