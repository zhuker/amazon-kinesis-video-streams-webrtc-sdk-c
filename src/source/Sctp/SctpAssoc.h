#ifndef __KINESIS_VIDEO_WEBRTC_SCTP_ASSOC__
#define __KINESIS_VIDEO_WEBRTC_SCTP_ASSOC__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initialize an SCTP association.
VOID sctpAssocInit(PSctpAssociation pAssoc, UINT16 localPort, UINT16 remotePort, UINT16 mtu);

// Start the handshake by sending INIT. Transitions to COOKIE_WAIT.
STATUS sctpAssocConnect(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData);

// Handle an inbound SCTP packet. Dispatches chunks to appropriate handlers.
STATUS sctpAssocHandlePacket(PSctpAssociation pAssoc, PBYTE pBuf, UINT32 bufLen, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData,
                             SctpAssocMessageFn messageFn, UINT64 messageCustomData);

// Send a data message on the association.
STATUS sctpAssocSend(PSctpAssociation pAssoc, UINT16 streamId, UINT32 ppid, BOOL unordered, PBYTE pPayload, UINT32 payloadLen, UINT16 maxRetransmits,
                     UINT64 lifetimeMs, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData);

// Check and process timer expiries (T1-init, T3-rtx).
STATUS sctpAssocCheckTimers(PSctpAssociation pAssoc, UINT64 nowMs, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData);

// Initiate graceful shutdown.
STATUS sctpAssocShutdown(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData);

// Free any dynamically allocated payloads in outstanding queue.
VOID sctpAssocCleanup(PSctpAssociation pAssoc);

#ifdef __cplusplus
}
#endif

#endif /* __KINESIS_VIDEO_WEBRTC_SCTP_ASSOC__ */
