/*******************************************
RFC 2198 RED Payloader (Opus) include file
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_RTPREDPAYLOADER_H
#define __KINESIS_VIDEO_WEBRTC_CLIENT_RTPREDPAYLOADER_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// RFC 2198 wire-format limits
#define RED_HEADER_LEN_NON_LAST 4
#define RED_HEADER_LEN_LAST     1
// 10-bit block length field; max value is 1023 bytes per redundant block
#define RED_MAX_BLOCK_LEN 1023
// 14-bit timestamp offset
#define RED_MAX_TS_DELTA (1 << 14)
// Defensive cap on F-bit chain length when parsing
#define RED_MAX_BLOCKS 32
// Upper bound for the configurable redundancy level
#define RED_MAX_REDUNDANCY 9
// Chrome/libwebrtc default redundancy level
#define RED_DEFAULT_REDUNDANCY 1

typedef struct {
    UINT32 rtpTimestamp;
    UINT32 payloadLen; // 0 means slot is unused
    BYTE payload[RED_MAX_BLOCK_LEN];
} RedSenderSlot, *PRedSenderSlot;

typedef struct {
    UINT8 redundancyLevel; // N in [1..RED_MAX_REDUNDANCY]
    UINT8 opusPayloadType; // PT emitted inside each RED header
    UINT32 nextSlot;       // ring write index
    RedSenderSlot slots[RED_MAX_REDUNDANCY];
} RedSenderState, *PRedSenderState;

/**
 * Allocate a RED sender-side state struct. redundancyLevel is clamped into [1..RED_MAX_REDUNDANCY];
 * 0 maps to RED_DEFAULT_REDUNDANCY.
 */
STATUS createRedSenderState(UINT8 redundancyLevel, UINT8 opusPayloadType, PRedSenderState* ppState);

/**
 * Free a RED sender-side state struct.
 */
STATUS freeRedSenderState(PRedSenderState* ppState);

/**
 * Pack one Opus frame into an RFC 2198 RED body (primary + cached redundancy), or
 * emit the bare Opus bytes when the primary is too large for the 10-bit block-length
 * field. State is only mutated when payloadBuffer is non-NULL (real emit).
 *
 * @param mtu                      outer packet MTU (same value passed to other createPayloadFor* funcs)
 * @param opusFrame, opusFrameLength  current Opus payload
 * @param rtpTimestamp             RTP timestamp of the current frame (48kHz)
 * @param pState                   ring state (caller-owned). Mutated only when payloadBuffer != NULL.
 * @param payloadBuffer            NULL for size-only; otherwise destination buffer
 * @param pPayloadLength           in: buffer capacity (when payloadBuffer != NULL). out: total body length.
 * @param pPayloadSubLength        out (non-NULL): pPayloadSubLength[0] = total body length
 * @param pPayloadSubLenSize       in: capacity of pPayloadSubLength. out: always 1.
 * @param pIsFallbackToPlainOpus   out: TRUE if the body is bare Opus (primary >= 1024 bytes);
 *                                      caller must emit with the Opus PT rather than the RED PT in that case.
 */
STATUS createPayloadForOpusRed(UINT32 mtu, PBYTE opusFrame, UINT32 opusFrameLength, UINT32 rtpTimestamp, PRedSenderState pState, PBYTE payloadBuffer,
                               PUINT32 pPayloadLength, PUINT32 pPayloadSubLength, PUINT32 pPayloadSubLenSize, PBOOL pIsFallbackToPlainOpus);

#ifdef __cplusplus
}
#endif
#endif //__KINESIS_VIDEO_WEBRTC_CLIENT_RTPREDPAYLOADER_H
