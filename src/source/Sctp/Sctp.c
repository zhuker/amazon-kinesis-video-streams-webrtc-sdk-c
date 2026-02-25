#define LOG_CLASS "SCTP"
#include "../Include_i.h"

// Forward declaration
static STATUS handleDcepPacket(PSctpSession pSctpSession, UINT32 streamId, PBYTE data, UINT32 length);

// Internal bridge callback: forward SCTP association outbound packets to the session's callback
static VOID sctpOutboundBridge(UINT64 customData, PBYTE pPacket, UINT32 packetLen)
{
    PSctpSession pSctpSession = (PSctpSession) customData;
    if (pSctpSession == NULL || ATOMIC_LOAD(&pSctpSession->shutdownStatus) != SCTP_SESSION_ACTIVE) {
        return;
    }
    if (pSctpSession->sctpSessionCallbacks.outboundPacketFunc != NULL) {
        pSctpSession->sctpSessionCallbacks.outboundPacketFunc(pSctpSession->sctpSessionCallbacks.customData, pPacket, packetLen);
    }
}

// Internal bridge callback: deliver reassembled messages / DCEP to the session's callbacks
static VOID sctpMessageBridge(UINT64 customData, UINT32 streamId, UINT32 ppid, PBYTE pPayload, UINT32 payloadLen)
{
    PSctpSession pSctpSession = (PSctpSession) customData;
    BOOL isBinary = FALSE;

    if (pSctpSession == NULL || ATOMIC_LOAD(&pSctpSession->shutdownStatus) != SCTP_SESSION_ACTIVE) {
        return;
    }

    switch (ppid) {
        case SCTP_PPID_DCEP:
            handleDcepPacket(pSctpSession, streamId, pPayload, payloadLen);
            break;
        case SCTP_PPID_BINARY:
        case SCTP_PPID_BINARY_EMPTY:
            isBinary = TRUE;
            // fallthrough
        case SCTP_PPID_STRING:
        case SCTP_PPID_STRING_EMPTY:
            if (pSctpSession->sctpSessionCallbacks.dataChannelMessageFunc != NULL) {
                pSctpSession->sctpSessionCallbacks.dataChannelMessageFunc(pSctpSession->sctpSessionCallbacks.customData, streamId, isBinary,
                                                                          pPayload, payloadLen);
            }
            break;
        default:
            DLOGI("Unhandled PPID on incoming SCTP message %u", ppid);
            break;
    }
}

STATUS initSctpSession()
{
    // No global state needed — all state is per-association
    return STATUS_SUCCESS;
}

VOID deinitSctpSession()
{
    // No global teardown needed
}

STATUS createSctpSession(PSctpSessionCallbacks pSctpSessionCallbacks, PSctpSession* ppSctpSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSctpSession pSctpSession = NULL;

    CHK(ppSctpSession != NULL && pSctpSessionCallbacks != NULL, STATUS_NULL_ARG);

    pSctpSession = (PSctpSession) MEMCALLOC(1, SIZEOF(SctpSession));
    CHK(pSctpSession != NULL, STATUS_NOT_ENOUGH_MEMORY);

    ATOMIC_STORE(&pSctpSession->shutdownStatus, SCTP_SESSION_ACTIVE);
    pSctpSession->lock = MUTEX_CREATE(TRUE);
    pSctpSession->sctpSessionCallbacks = *pSctpSessionCallbacks;

    // Initialize the SCTP association
    sctpAssocInit(&pSctpSession->assoc, SCTP_ASSOCIATION_DEFAULT_PORT, SCTP_ASSOCIATION_DEFAULT_PORT, SCTP_MTU);

    // Start the handshake (sends INIT)
    CHK_STATUS(sctpAssocConnect(&pSctpSession->assoc, sctpOutboundBridge, (UINT64) pSctpSession));

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        freeSctpSession(&pSctpSession);
    }

    *ppSctpSession = pSctpSession;

    LEAVES();
    return retStatus;
}

STATUS freeSctpSession(PSctpSession* ppSctpSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSctpSession pSctpSession;

    CHK(ppSctpSession != NULL, STATUS_NULL_ARG);

    pSctpSession = *ppSctpSession;

    CHK(pSctpSession != NULL, retStatus);

    ATOMIC_STORE(&pSctpSession->shutdownStatus, SCTP_SESSION_SHUTDOWN_INITIATED);

    MUTEX_LOCK(pSctpSession->lock);

    // Try to send SHUTDOWN if established
    if (pSctpSession->assoc.state == SCTP_ASSOC_ESTABLISHED) {
        sctpAssocShutdown(&pSctpSession->assoc, sctpOutboundBridge, (UINT64) pSctpSession);
    }

    // Clean up outstanding queue
    sctpAssocCleanup(&pSctpSession->assoc);

    MUTEX_UNLOCK(pSctpSession->lock);

    ATOMIC_STORE(&pSctpSession->shutdownStatus, SCTP_SESSION_SHUTDOWN_COMPLETED);

    if (IS_VALID_MUTEX_VALUE(pSctpSession->lock)) {
        MUTEX_FREE(pSctpSession->lock);
    }

    SAFE_MEMFREE(*ppSctpSession);
    *ppSctpSession = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS putSctpPacket(PSctpSession pSctpSession, PBYTE buf, UINT32 bufLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 nowMs;
    BOOL locked = FALSE;

    CHK(pSctpSession != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pSctpSession->lock);
    locked = TRUE;

    // Check timers before processing
    nowMs = GETTIME() / (10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    sctpAssocCheckTimers(&pSctpSession->assoc, nowMs, sctpOutboundBridge, (UINT64) pSctpSession);

    CHK_STATUS(sctpAssocHandlePacket(&pSctpSession->assoc, buf, bufLen, sctpOutboundBridge, (UINT64) pSctpSession, sctpMessageBridge,
                                     (UINT64) pSctpSession));

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pSctpSession->lock);
    }
    LEAVES();
    return retStatus;
}

STATUS sctpSessionTickTimers(PSctpSession pSctpSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 nowMs;
    BOOL locked = FALSE;

    CHK(pSctpSession != NULL, STATUS_NULL_ARG);

    if (ATOMIC_LOAD(&pSctpSession->shutdownStatus) != SCTP_SESSION_ACTIVE) {
        CHK(FALSE, retStatus);
    }

    MUTEX_LOCK(pSctpSession->lock);
    locked = TRUE;

    nowMs = GETTIME() / (10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    sctpAssocCheckTimers(&pSctpSession->assoc, nowMs, sctpOutboundBridge, (UINT64) pSctpSession);

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pSctpSession->lock);
    }
    LEAVES();
    return retStatus;
}

STATUS sctpSessionWriteMessage(PSctpSession pSctpSession, UINT32 streamId, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL unordered = FALSE;
    UINT16 maxRetransmits = 0xFFFF; // unlimited
    UINT64 lifetimeMs = 0;         // unlimited
    UINT32 ppid;
    BOOL locked = FALSE;

    CHK(pSctpSession != NULL && pMessage != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pSctpSession->lock);
    locked = TRUE;

    // Read channel parameters from the DCEP packet buffer (same as usrsctp version)
    if ((pSctpSession->packet[1] & DCEP_DATA_CHANNEL_RELIABLE_UNORDERED) != 0) {
        unordered = TRUE;
    }
    if ((pSctpSession->packet[1] & DCEP_DATA_CHANNEL_REXMIT) != 0) {
        maxRetransmits = (UINT16) getUnalignedInt32BigEndian((PINT32)(pSctpSession->packet + SIZEOF(UINT32)));
    }
    if ((pSctpSession->packet[1] & DCEP_DATA_CHANNEL_TIMED) != 0) {
        lifetimeMs = (UINT64) getUnalignedInt32BigEndian((PINT32)(pSctpSession->packet + SIZEOF(UINT32)));
    }

    ppid = isBinary ? SCTP_PPID_BINARY : SCTP_PPID_STRING;

    CHK_STATUS(sctpAssocSend(&pSctpSession->assoc, (UINT16) streamId, ppid, unordered, pMessage, pMessageLen, maxRetransmits, lifetimeMs,
                             sctpOutboundBridge, (UINT64) pSctpSession));

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pSctpSession->lock);
    }
    LEAVES();
    return retStatus;
}

// https://tools.ietf.org/html/draft-ietf-rtcweb-data-protocol-09#section-5.1
//      0                   1                   2                   3
//      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     |  Message Type |  Channel Type |            Priority           |
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     |                    Reliability Parameter                      |
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     |         Label Length          |       Protocol Length         |
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     \                                                               /
//     |                             Label                             |
//     /                                                               /
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     \                                                               /
//     |                            Protocol                           |
//     /                                                               /
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
STATUS sctpSessionWriteDcep(PSctpSession pSctpSession, UINT32 streamId, PCHAR pChannelName, UINT32 pChannelNameLen,
                            PRtcDataChannelInit pRtcDataChannelInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    CHK(pSctpSession != NULL && pChannelName != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pSctpSession->lock);
    locked = TRUE;

    MEMSET(pSctpSession->packet, 0x00, SIZEOF(pSctpSession->packet));
    pSctpSession->packetSize = SCTP_DCEP_HEADER_LENGTH + pChannelNameLen;

    // Build DATA_CHANNEL_OPEN message in packet buffer
    pSctpSession->packet[0] = DCEP_DATA_CHANNEL_OPEN; // message type
    pSctpSession->packet[1] = DCEP_DATA_CHANNEL_RELIABLE_ORDERED;

    if (!pRtcDataChannelInit->ordered) {
        pSctpSession->packet[1] |= DCEP_DATA_CHANNEL_RELIABLE_UNORDERED;
    }
    if (pRtcDataChannelInit->maxRetransmits.value >= 0 && pRtcDataChannelInit->maxRetransmits.isNull == FALSE) {
        pSctpSession->packet[1] |= DCEP_DATA_CHANNEL_REXMIT;
        putUnalignedInt32BigEndian(pSctpSession->packet + SIZEOF(UINT32), pRtcDataChannelInit->maxRetransmits.value);
    } else if (pRtcDataChannelInit->maxPacketLifeTime.value >= 0 && pRtcDataChannelInit->maxPacketLifeTime.isNull == FALSE) {
        pSctpSession->packet[1] |= DCEP_DATA_CHANNEL_TIMED;
        putUnalignedInt32BigEndian(pSctpSession->packet + SIZEOF(UINT32), pRtcDataChannelInit->maxPacketLifeTime.value);
    }

    putUnalignedInt16BigEndian(pSctpSession->packet + SCTP_DCEP_LABEL_LEN_OFFSET, pChannelNameLen);
    MEMCPY(pSctpSession->packet + SCTP_DCEP_LABEL_OFFSET, pChannelName, pChannelNameLen);

    // Send DCEP via SCTP association
    CHK_STATUS(sctpAssocSend(&pSctpSession->assoc, (UINT16) streamId, SCTP_PPID_DCEP, FALSE, pSctpSession->packet, pSctpSession->packetSize,
                             0xFFFF, 0, sctpOutboundBridge, (UINT64) pSctpSession));

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pSctpSession->lock);
    }
    LEAVES();
    return retStatus;
}

static STATUS handleDcepPacket(PSctpSession pSctpSession, UINT32 streamId, PBYTE data, UINT32 length)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 labelLength = 0;
    UINT16 protocolLength = 0;

    // Assert that is DCEP of type DataChannelOpen
    CHK(length > SCTP_DCEP_HEADER_LENGTH && data[0] == DCEP_DATA_CHANNEL_OPEN, STATUS_SUCCESS);

    MEMCPY(&labelLength, data + 8, SIZEOF(UINT16));
    MEMCPY(&protocolLength, data + 10, SIZEOF(UINT16));
    putInt16((PINT16) &labelLength, labelLength);
    putInt16((PINT16) &protocolLength, protocolLength);

    CHK((labelLength + protocolLength + SCTP_DCEP_HEADER_LENGTH) >= length, STATUS_SCTP_INVALID_DCEP_PACKET);

    CHK(SCTP_MAX_ALLOWABLE_PACKET_LENGTH >= length, STATUS_SCTP_INVALID_DCEP_PACKET);

    pSctpSession->sctpSessionCallbacks.dataChannelOpenFunc(pSctpSession->sctpSessionCallbacks.customData, streamId, data + SCTP_DCEP_HEADER_LENGTH,
                                                           labelLength);

CleanUp:
    LEAVES();
    return retStatus;
}
