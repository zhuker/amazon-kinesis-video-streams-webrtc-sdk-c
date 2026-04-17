/*******************************************
RFC 2198 RED Depayloader (Opus) include file
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_RTPREDDEPAYLOADER_H
#define __KINESIS_VIDEO_WEBRTC_CLIENT_RTPREDDEPAYLOADER_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

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
#endif //__KINESIS_VIDEO_WEBRTC_CLIENT_RTPREDDEPAYLOADER_H
