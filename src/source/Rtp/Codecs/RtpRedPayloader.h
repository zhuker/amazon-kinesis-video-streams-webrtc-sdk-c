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

/**
 * Split a RED-wrapped RTP packet into synthetic per-Opus-frame RTP packets.
 * Each produced packet owns a freshly-allocated pRawPacket buffer; the caller
 * must dispose of them with freeRtpPacket.
 *
 * Synthetic packets share the outer packet's SSRC. The primary keeps the outer
 * packet's sequence number and timestamp; each redundant block gets a synthetic
 * sequence number of (outer.seq - (distance from primary)) and timestamp
 * (outer.ts - offset). Redundant packets have isSynthetic=TRUE; the primary is
 * marked isSynthetic=FALSE.
 *
 * @param pRedPacket          the inbound RED-wrapped packet (not modified; not freed)
 * @param negotiatedOpusPt    Opus PT carried inside RED (matched against each F-bit block PT)
 * @param ppSyntheticPackets  caller-allocated array of PRtpPacket slots; filled oldest-first
 *                            with redundant blocks, primary last
 * @param maxCount            capacity of ppSyntheticPackets; RED_MAX_BLOCKS is a safe upper bound
 * @param pProducedCount      out: number of synthetic packets written
 * @param pFecBytes           out (optional): sum of redundant-only payload lengths
 *
 * Returns STATUS_RTP_INVALID_RED_PACKET on malformed input (oversized F-bit chain,
 * truncated body, etc.) with 0 packets produced. Blocks whose inner PT does not match
 * negotiatedOpusPt are skipped silently (with a log warning).
 */
STATUS splitRedRtpPacket(PRtpPacket pRedPacket, UINT8 negotiatedOpusPt, PRtpPacket* ppSyntheticPackets, UINT32 maxCount, PUINT32 pProducedCount,
                         PUINT32 pFecBytes);

#ifdef __cplusplus
}
#endif
#endif //__KINESIS_VIDEO_WEBRTC_CLIENT_RTPREDPAYLOADER_H
