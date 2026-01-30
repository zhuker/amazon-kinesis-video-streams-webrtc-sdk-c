/*******************************************
H264 RTP Payloader include file
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_RTPH264PAYLOADER_H
#define __KINESIS_VIDEO_WEBRTC_CLIENT_RTPH264PAYLOADER_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FU_A_HEADER_SIZE     2
#define FU_B_HEADER_SIZE     4
#define STAP_A_HEADER_SIZE   1
#define STAP_B_HEADER_SIZE   3
#define SINGLE_U_HEADER_SIZE 1
#define FU_A_INDICATOR       28
#define FU_B_INDICATOR       29
#define STAP_A_INDICATOR     24
#define STAP_B_INDICATOR     25
#define NAL_TYPE_MASK        31

// H264 NAL unit types
#define H264_NAL_TYPE_NON_IDR_SLICE  1   // Non-IDR slice (P/B frame)
#define H264_NAL_TYPE_IDR_SLICE      5   // IDR slice (keyframe)
#define H264_NAL_TYPE_SEI            6   // Supplemental Enhancement Information
#define H264_NAL_TYPE_SPS            7   // Sequence Parameter Set
#define H264_NAL_TYPE_PPS            8   // Picture Parameter Set

// H264 SPS/PPS tracker state (like libwebrtc H264SpsPpsTracker)
typedef struct {
    BOOL hasReceivedSps;        // TRUE if at least one SPS has been received
    BOOL hasReceivedPps;        // TRUE if at least one PPS has been received
    BOOL hasReceivedIdr;        // TRUE if at least one IDR has been received
    UINT64 lastSpsTime;         // Time of last SPS received (100ns units)
    UINT64 lastPpsTime;         // Time of last PPS received (100ns units)
    UINT64 lastIdrTime;         // Time of last IDR received (100ns units)
} H264SpsPpsTracker, *PH264SpsPpsTracker;

// Result of SPS/PPS check
typedef enum {
    H264_SPS_PPS_OK,                // SPS/PPS available, frame can be decoded
    H264_SPS_PPS_MISSING_SPS,       // Missing SPS, need keyframe
    H264_SPS_PPS_MISSING_PPS,       // Missing PPS, need keyframe
    H264_SPS_PPS_MISSING_BOTH       // Missing both SPS and PPS, need keyframe
} H264SpsPpsStatus;

/*
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  | FU indicator  |   FU header   |                               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
 *  |                                                               |
 *  |                         FU payload                            |
 *  |                                                               |
 *  |                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                               :...OPTIONAL RTP padding        |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

STATUS createPayloadForH264(UINT32, PBYTE, UINT32, PBYTE, PUINT32, PUINT32, PUINT32);
STATUS getNextNaluLength(PBYTE, UINT32, PUINT32, PUINT32);
STATUS createPayloadFromNalu(UINT32, PBYTE, UINT32, PPayloadArray, PUINT32, PUINT32);
STATUS depayH264FromRtpPayload(PBYTE, UINT32, PBYTE, PUINT32, PBOOL);

// H264 SPS/PPS tracker functions (like libwebrtc)
VOID h264SpsPpsTrackerInit(PH264SpsPpsTracker pTracker);
VOID h264SpsPpsTrackerReset(PH264SpsPpsTracker pTracker);
H264SpsPpsStatus h264SpsPpsTrackerProcessNalu(PH264SpsPpsTracker pTracker, PBYTE pNalu, UINT32 naluLength);
H264SpsPpsStatus h264SpsPpsTrackerCheckStatus(PH264SpsPpsTracker pTracker);
BOOL h264SpsPpsTrackerNeedsKeyframe(PH264SpsPpsTracker pTracker);

#ifdef __cplusplus
}
#endif
#endif //__KINESIS_VIDEO_WEBRTC_CLIENT_RTPH264PAYLOADER_H
