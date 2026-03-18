#define LOG_CLASS "PeerConnection"

#include "../Include_i.h"

static volatile ATOMIC_BOOL gKvsWebRtcInitialized = (SIZE_T) FALSE;

// Function to get access to the Singleton instance
PWebRtcClientContext getWebRtcClientInstance()
{
    ENTERS();
    static WebRtcClientContext w = {.pStunIpAddrCtx = NULL, .stunCtxlock = INVALID_MUTEX_VALUE, .contextRefCnt = 0, .isContextInitialized = FALSE};
    ATOMIC_INCREMENT(&w.contextRefCnt);
    LEAVES();
    return &w;
}

VOID releaseHoldOnInstance(PWebRtcClientContext pWebRtcClientContext)
{
    ENTERS();
    ATOMIC_DECREMENT(&pWebRtcClientContext->contextRefCnt);
    LEAVES();
}

STATUS createWebRtcClientInstance()
{
    ENTERS();
    PWebRtcClientContext pWebRtcClientContext = getWebRtcClientInstance();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    CHK_WARN(!ATOMIC_LOAD_BOOL(&pWebRtcClientContext->isContextInitialized), retStatus, "WebRtc client context already initialized, nothing to do");
    CHK_ERR(!IS_VALID_MUTEX_VALUE(pWebRtcClientContext->stunCtxlock), retStatus, "Mutex seems to have been created already");

    pWebRtcClientContext->stunCtxlock = MUTEX_CREATE(TRUE);
    CHK_ERR(IS_VALID_MUTEX_VALUE(pWebRtcClientContext->stunCtxlock), STATUS_NULL_ARG, "Mutex creation failed");
    MUTEX_LOCK(pWebRtcClientContext->stunCtxlock);
    locked = TRUE;
    CHK_WARN(pWebRtcClientContext->pStunIpAddrCtx == NULL, STATUS_INVALID_OPERATION, "STUN object already allocated");
    pWebRtcClientContext->pStunIpAddrCtx = (PStunIpAddrContext) MEMCALLOC(1, SIZEOF(StunIpAddrContext));
    CHK_ERR(pWebRtcClientContext->pStunIpAddrCtx != NULL, STATUS_NULL_ARG, "Memory allocation for WebRtc client object failed");
    pWebRtcClientContext->pStunIpAddrCtx->expirationDuration = 2 * HUNDREDS_OF_NANOS_IN_AN_HOUR;
    ATOMIC_STORE_BOOL(&pWebRtcClientContext->isContextInitialized, TRUE);
    DLOGI("Initialized WebRTC Client instance");
CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pWebRtcClientContext->stunCtxlock);
    }
    releaseHoldOnInstance(pWebRtcClientContext);
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS allocateSrtp(PKvsPeerConnection pKvsPeerConnection)
{
    ENTERS();
    DtlsKeyingMaterial dtlsKeyingMaterial;
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    MEMSET(&dtlsKeyingMaterial, 0, SIZEOF(DtlsKeyingMaterial));

    CHK(pKvsPeerConnection != NULL, STATUS_SUCCESS);
    CHK_STATUS(dtlsSessionVerifyRemoteCertificateFingerprint(pKvsPeerConnection->pDtlsSession, pKvsPeerConnection->remoteCertificateFingerprint));
    CHK_STATUS(dtlsSessionPopulateKeyingMaterial(pKvsPeerConnection->pDtlsSession, &dtlsKeyingMaterial));

    MUTEX_LOCK(pKvsPeerConnection->pSrtpSessionLock);
    locked = TRUE;

    CHK_STATUS(initSrtpSession(pKvsPeerConnection->dtlsIsServer ? dtlsKeyingMaterial.clientWriteKey : dtlsKeyingMaterial.serverWriteKey,
                               pKvsPeerConnection->dtlsIsServer ? dtlsKeyingMaterial.serverWriteKey : dtlsKeyingMaterial.clientWriteKey,
                               dtlsKeyingMaterial.srtpProfile, &(pKvsPeerConnection->pSrtpSession)));

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->pSrtpSessionLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

#ifdef ENABLE_DATA_CHANNEL
STATUS allocateSctpSortDataChannelsDataCallback(UINT64 customData, PHashEntry pHashEntry)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PAllocateSctpSortDataChannelsData data = (PAllocateSctpSortDataChannelsData) customData;
    PKvsDataChannel pKvsDataChannel = (PKvsDataChannel) pHashEntry->value;

    CHK(customData != 0, STATUS_NULL_ARG);

    pKvsDataChannel->channelId = data->currentDataChannelId;
    pKvsDataChannel->dataChannel.id = data->currentDataChannelId;
    pKvsDataChannel->rtcDataChannelDiagnostics.dataChannelIdentifier = data->currentDataChannelId;
    CHK_STATUS(hashTablePut(data->pKvsPeerConnection->pDataChannels, pKvsDataChannel->channelId, (UINT64) pKvsDataChannel));

    data->currentDataChannelId += 2;

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

#ifdef ENABLE_NATIVE_SCTP
// Period for the SCTP retransmission timer tick (50ms)
#define SCTP_TIMER_TICK_PERIOD (50 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)

static STATUS sctpTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    UNUSED_PARAM(currentTime);
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;

    if (pKvsPeerConnection == NULL || pKvsPeerConnection->pSctpSession == NULL) {
        return STATUS_TIMER_QUEUE_STOP_SCHEDULING;
    }

    sctpSessionTickTimers(pKvsPeerConnection->pSctpSession);
    return STATUS_SUCCESS;
}
#endif

STATUS allocateSctp(PKvsPeerConnection pKvsPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    SctpSessionCallbacks sctpSessionCallbacks;
    AllocateSctpSortDataChannelsData data;
    UINT32 currentDataChannelId = 0;
    UINT64 hashValue = 0;
    PKvsDataChannel pKvsDataChannel = NULL;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    currentDataChannelId = (pKvsPeerConnection->dtlsIsServer) ? 1 : 0;

    // Re-sort DataChannel hashmap using proper streamIds if we are offerer or answerer
    data.currentDataChannelId = currentDataChannelId;
    data.pKvsPeerConnection = pKvsPeerConnection;
    data.unkeyedDataChannels = pKvsPeerConnection->pDataChannels;
    CHK_STATUS(hashTableCreateWithParams(CODEC_HASH_TABLE_BUCKET_COUNT, CODEC_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pDataChannels));
    CHK_STATUS(hashTableIterateEntries(data.unkeyedDataChannels, (UINT64) &data, allocateSctpSortDataChannelsDataCallback));

    // Free unkeyed DataChannels
    CHK_LOG_ERR(hashTableClear(data.unkeyedDataChannels));
    CHK_LOG_ERR(hashTableFree(data.unkeyedDataChannels));

    // Create the SCTP Session
    sctpSessionCallbacks.outboundPacketFunc = onSctpSessionOutboundPacket;
    sctpSessionCallbacks.dataChannelMessageFunc = onSctpSessionDataChannelMessage;
    sctpSessionCallbacks.dataChannelOpenFunc = onSctpSessionDataChannelOpen;
    sctpSessionCallbacks.customData = (UINT64) pKvsPeerConnection;
    CHK_STATUS(createSctpSession(&sctpSessionCallbacks, &(pKvsPeerConnection->pSctpSession)));
#ifdef ENABLE_NATIVE_SCTP
    // Start periodic SCTP timer to drive retransmissions independently of incoming packets
    if (IS_VALID_TIMER_QUEUE_HANDLE(pKvsPeerConnection->timerQueueHandle)) {
        CHK_STATUS(timerQueueAddTimer(pKvsPeerConnection->timerQueueHandle, SCTP_TIMER_TICK_PERIOD, SCTP_TIMER_TICK_PERIOD, sctpTimerCallback,
                                      (UINT64) pKvsPeerConnection, &pKvsPeerConnection->sctpTimerCallbackId));
    }
#endif

    for (; currentDataChannelId < data.currentDataChannelId; currentDataChannelId += 2) {
        pKvsDataChannel = NULL;
        retStatus = hashTableGet(pKvsPeerConnection->pDataChannels, currentDataChannelId, &hashValue);
        pKvsDataChannel = (PKvsDataChannel) hashValue;
        if (retStatus == STATUS_SUCCESS || retStatus == STATUS_HASH_KEY_NOT_PRESENT) {
            retStatus = STATUS_SUCCESS;
        } else {
            CHK(FALSE, retStatus);
        }
        CHK(pKvsDataChannel != NULL, STATUS_INTERNAL_ERROR);
        CHK_STATUS(sctpSessionWriteDcep(pKvsPeerConnection->pSctpSession, currentDataChannelId, pKvsDataChannel->dataChannel.name,
                                        STRLEN(pKvsDataChannel->dataChannel.name), &pKvsDataChannel->rtcDataChannelInit));
        pKvsDataChannel->rtcDataChannelDiagnostics.state = RTC_DATA_CHANNEL_STATE_OPEN;
        if (STATUS_FAILED(hashTableUpsert(pKvsPeerConnection->pDataChannels, currentDataChannelId, (UINT64) pKvsDataChannel))) {
            DLOGW("Failed to update entry in hash table with recent changes to data channel");
        }
        if (pKvsDataChannel->onOpen != NULL) {
            pKvsDataChannel->onOpen(pKvsDataChannel->onOpenCustomData, &pKvsDataChannel->dataChannel);
        }
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}
#endif

VOID onInboundPacket(UINT64 customData, PBYTE buff, UINT32 buffLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    BOOL isDtlsConnected = FALSE;
    INT32 signedBuffLen = buffLen;

    CHK(signedBuffLen > 2 && pKvsPeerConnection != NULL, STATUS_SUCCESS);
    CHK(ATOMIC_LOAD_BOOL(&pKvsPeerConnection->receiveEnabled), STATUS_SUCCESS);

    /*
     demux each packet off of its first byte
     https://tools.ietf.org/html/rfc5764#section-5.1.2
                 +----------------+
                  | 127 < B < 192 -+--> forward to RTP
                  |                |
      packet -->  |  19 < B < 64  -+--> forward to DTLS
                  |                |
                  |       B < 2   -+--> forward to STUN
                  +----------------+
    */
    if (buff[0] > 19 && buff[0] < 64) {
        dtlsSessionProcessPacket(pKvsPeerConnection->pDtlsSession, buff, &signedBuffLen);

        CHK_STATUS(dtlsSessionIsInitFinished(pKvsPeerConnection->pDtlsSession, &isDtlsConnected));
        if (isDtlsConnected) {
            if (pKvsPeerConnection->pSrtpSession == NULL) {
                CHK_STATUS(allocateSrtp(pKvsPeerConnection));
            }

#ifdef ENABLE_DATA_CHANNEL
            if (ATOMIC_LOAD_BOOL(&pKvsPeerConnection->sctpIsEnabled)) {
                if (pKvsPeerConnection->pSctpSession == NULL) {
                    CHK_STATUS(allocateSctp(pKvsPeerConnection));
                }

                if (signedBuffLen > 0) {
                    // PCAP: capture decrypted inbound SCTP
                    if (pKvsPeerConnection->pPcapDump != NULL) {
                        pcapDumpWriteSctpPacket(pKvsPeerConnection->pPcapDump, buff, signedBuffLen, PCAP_PACKET_DIRECTION_RECV);
                    }
                    CHK_STATUS(putSctpPacket(pKvsPeerConnection->pSctpSession, buff, signedBuffLen));
                }
            }
#endif
            changePeerConnectionState(pKvsPeerConnection, RTC_PEER_CONNECTION_STATE_CONNECTED);
        }

    } else if ((buff[0] > 127 && buff[0] < 192) && (pKvsPeerConnection->pSrtpSession != NULL)) {
        if (buff[1] >= 192 && buff[1] <= 223) {
            if (STATUS_FAILED(retStatus = decryptSrtcpPacket(pKvsPeerConnection->pSrtpSession, buff, &signedBuffLen))) {
                DLOGW("decryptSrtcpPacket failed with 0x%08x", retStatus);
                CHK(FALSE, STATUS_SUCCESS);
            }

            // PCAP: capture decrypted inbound RTCP
            if (pKvsPeerConnection->pPcapDump != NULL) {
                pcapDumpWritePacket(pKvsPeerConnection->pPcapDump, buff, signedBuffLen, TRUE, PCAP_PACKET_DIRECTION_RECV);
            }

            CHK_STATUS(onRtcpPacket(pKvsPeerConnection, buff, signedBuffLen));
        } else {
            CHK_STATUS(sendPacketToRtpReceiver(pKvsPeerConnection, buff, signedBuffLen));
        }
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
}

static VOID rrInitSeq(PKvsRtpTransceiver pTransceiver, UINT16 seq)
{
    pTransceiver->rrBaseSeq = seq;
    pTransceiver->rrMaxSeq = seq;
    pTransceiver->rrBadSeq = RTP_SEQ_MOD + 1;
    pTransceiver->rrCycles = 0;
    pTransceiver->rrExpectedPrior = 0;
    pTransceiver->rrReceivedPrior = 0;
    pTransceiver->rrSeqInitialized = TRUE;
}

static VOID rrUpdateSeq(PKvsRtpTransceiver pTransceiver, UINT16 seq)
{
    // RFC 3550 Appendix A.1
    UINT16 udelta = seq - pTransceiver->rrMaxSeq;

    if (udelta < MAX_DROPOUT) {
        // in order, with permissible gap
        if (seq < pTransceiver->rrMaxSeq) {
            // sequence number wrapped
            pTransceiver->rrCycles += RTP_SEQ_MOD;
        }
        pTransceiver->rrMaxSeq = seq;
    } else if (udelta <= RTP_SEQ_MOD - MAX_MISORDER) {
        // the sequence number made a very large jump
        if ((UINT32) seq == pTransceiver->rrBadSeq) {
            // two sequential packets -- assume that the other side restarted
            rrInitSeq(pTransceiver, seq);
        } else {
            pTransceiver->rrBadSeq = ((UINT32) seq + 1) & (RTP_SEQ_MOD - 1);
        }
    }
    // else duplicate or reordered packet — ignore for max tracking
}

STATUS sendPacketToRtpReceiver(PKvsPeerConnection pKvsPeerConnection, PBYTE pBuffer, UINT32 bufferLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    PKvsRtpTransceiver pTransceiver;
    UINT64 item, now;
    UINT32 ssrc;
    PRtpPacket pRtpPacket = NULL;
    PBYTE pPayload = NULL;
    BOOL ownedByJitterBuffer = FALSE, discarded = FALSE;
    UINT64 packetsReceived = 0, packetsFailedDecryption = 0, lastPacketReceivedTimestamp = 0, headerBytesReceived = 0, bytesReceived = 0,
           packetsDiscarded = 0;
    INT64 arrival, r_ts, transit, delta;

    CHK(pKvsPeerConnection != NULL && pBuffer != NULL, STATUS_NULL_ARG);
    CHK(bufferLen >= MIN_HEADER_LENGTH, STATUS_INVALID_ARG);

    now = GETTIME();

    // IMPORTANT: Track TWCC BEFORE decryption!
    // RTP header and extensions are NOT encrypted by SRTP (only payload is)
    // This ensures we track TWCC even for packets that fail SRTP replay check
    if (pKvsPeerConnection->twccExtId != 0) {
        // Parse RTP header from raw (still encrypted) packet to get TWCC extension
        PRtpPacket pTwccPacket = NULL;
        PBYTE pTwccBuffer = (PBYTE) MEMALLOC(bufferLen);
        if (pTwccBuffer != NULL) {
            MEMCPY(pTwccBuffer, pBuffer, bufferLen);
            if (STATUS_SUCCEEDED(createRtpPacketFromBytes(pTwccBuffer, bufferLen, &pTwccPacket))) {
                pTwccPacket->receivedTime = now;
                if (pTwccPacket->header.extension && pTwccPacket->header.extensionProfile == TWCC_EXT_PROFILE) {
                    twccReceiverOnPacketReceived(pKvsPeerConnection, pTwccPacket);
                }
                freeRtpPacket(&pTwccPacket);
            } else {
                MEMFREE(pTwccBuffer);
            }
        }
    }

    // Now decrypt
    if (STATUS_FAILED(retStatus = decryptSrtpPacket(pKvsPeerConnection->pSrtpSession, pBuffer, (PINT32) &bufferLen))) {
        DLOGW("decryptSrtpPacket failed with 0x%08x", retStatus);
        packetsFailedDecryption++;
        CHK(FALSE, STATUS_SUCCESS);
    }

    // PCAP: capture decrypted inbound RTP
    if (pKvsPeerConnection->pPcapDump != NULL) {
        pcapDumpWritePacket(pKvsPeerConnection->pPcapDump, pBuffer, bufferLen, FALSE, PCAP_PACKET_DIRECTION_RECV);
    }

    CHK(NULL != (pPayload = (PBYTE) MEMALLOC(bufferLen)), STATUS_NOT_ENOUGH_MEMORY);
    MEMCPY(pPayload, pBuffer, bufferLen);
    CHK_STATUS(createRtpPacketFromBytes(pPayload, bufferLen, &pRtpPacket));
    pPayload = NULL; // pRtpPacket now owns the buffer
    pRtpPacket->receivedTime = now;

    ssrc = pRtpPacket->header.ssrc;

    // Find matching transceiver for this SSRC
    CHK_STATUS(doubleListGetHeadNode(pKvsPeerConnection->pTransceivers, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &item));
        pTransceiver = (PKvsRtpTransceiver) item;

        if (pTransceiver->jitterBufferSsrc == ssrc) {
            packetsReceived++;

            if (!pTransceiver->rrSeqInitialized) {
                rrInitSeq(pTransceiver, pRtpPacket->header.sequenceNumber);
            }
            rrUpdateSeq(pTransceiver, pRtpPacket->header.sequenceNumber);

            // https://tools.ietf.org/html/rfc3550#section-6.4.1
            // https://tools.ietf.org/html/rfc3550#appendix-A.8
            // interarrival jitter
            // arrival, the current time in the same units.
            // r_ts, the timestamp from   the incoming packet
            arrival = KVS_CONVERT_TIMESCALE(now, HUNDREDS_OF_NANOS_IN_A_SECOND, pTransceiver->pJitterBuffer->clockRate);
            r_ts = pRtpPacket->header.timestamp;
            transit = arrival - r_ts;
            delta = transit - pTransceiver->pJitterBuffer->transit;
            pTransceiver->pJitterBuffer->transit = transit;
            pTransceiver->pJitterBuffer->jitter += (1. / 16.) * ((DOUBLE) ABS(delta) - pTransceiver->pJitterBuffer->jitter);

            headerBytesReceived += RTP_HEADER_LEN(pRtpPacket);
            bytesReceived += pRtpPacket->rawPacketLength - RTP_HEADER_LEN(pRtpPacket);

            CHK_STATUS(jitterBufferPush(pTransceiver->pJitterBuffer, pRtpPacket, &discarded));
            if (discarded) {
                packetsDiscarded++;
            }
            lastPacketReceivedTimestamp = KVS_CONVERT_TIMESCALE(now, HUNDREDS_OF_NANOS_IN_A_SECOND, 1000);
            ownedByJitterBuffer = TRUE;
            CHK(FALSE, STATUS_SUCCESS);
        }
        pCurNode = pCurNode->pNext;
    }

    DLOGW("No transceiver to handle inbound ssrc %u", ssrc);

CleanUp:
    if (packetsReceived > 0) {
        MUTEX_LOCK(pTransceiver->statsLock);
        pTransceiver->inboundStats.received.packetsReceived += packetsReceived;
        pTransceiver->inboundStats.packetsFailedDecryption += packetsFailedDecryption;
        pTransceiver->inboundStats.lastPacketReceivedTimestamp = lastPacketReceivedTimestamp;
        pTransceiver->inboundStats.headerBytesReceived += headerBytesReceived;
        pTransceiver->inboundStats.bytesReceived += bytesReceived;
        pTransceiver->inboundStats.received.jitter = pTransceiver->pJitterBuffer->jitter / pTransceiver->pJitterBuffer->clockRate;
        pTransceiver->inboundStats.received.packetsDiscarded += packetsDiscarded;
        if (pTransceiver->rrSeqInitialized) {
            UINT32 extMax = pTransceiver->rrCycles + pTransceiver->rrMaxSeq;
            UINT32 expected = extMax - pTransceiver->rrBaseSeq + 1;
            pTransceiver->inboundStats.received.packetsLost = (INT64) expected - (INT64) pTransceiver->inboundStats.received.packetsReceived;
        }
        MUTEX_UNLOCK(pTransceiver->statsLock);
    }
    if (!ownedByJitterBuffer) {
        SAFE_MEMFREE(pPayload);
        freeRtpPacket(&pRtpPacket);
        CHK_LOG_ERR(retStatus);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS changePeerConnectionState(PKvsPeerConnection pKvsPeerConnection, RTC_PEER_CONNECTION_STATE newState)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    RtcOnConnectionStateChange onConnectionStateChange = NULL;
    UINT64 customData = 0;
    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;
    switch (newState) {
        case RTC_PEER_CONNECTION_STATE_CONNECTING:
            if (pKvsPeerConnection->iceConnectingStartTime == 0) {
                pKvsPeerConnection->iceConnectingStartTime = GETTIME();
            }
            break;
        case RTC_PEER_CONNECTION_STATE_CONNECTED:
            if (pKvsPeerConnection->iceConnectingStartTime != 0) {
                PROFILE_WITH_START_TIME_OBJ(pKvsPeerConnection->iceConnectingStartTime,
                                            pKvsPeerConnection->peerConnectionDiagnostics.iceHolePunchingTime, "ICE Hole Punching Time");
                pKvsPeerConnection->iceConnectingStartTime = 0;
            }
            break;
        default:
            break;
    }

    /* new and closed state are terminal*/
    CHK(pKvsPeerConnection->connectionState != newState && pKvsPeerConnection->connectionState != RTC_PEER_CONNECTION_STATE_FAILED &&
            pKvsPeerConnection->connectionState != RTC_PEER_CONNECTION_STATE_CLOSED,
        retStatus);

    pKvsPeerConnection->connectionState = newState;
    onConnectionStateChange = pKvsPeerConnection->onConnectionStateChange;
    customData = pKvsPeerConnection->onConnectionStateChangeCustomData;
    MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = FALSE;

    if (onConnectionStateChange != NULL) {
        onConnectionStateChange(customData, newState);
    }

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS onFrameReadyFunc(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pTransceiver = (PKvsRtpTransceiver) customData;
    PRtpPacket pPacket = NULL;
    Frame frame;
    UINT32 filledSize = 0, index;

    CHK(pTransceiver != NULL, STATUS_NULL_ARG);

    // TODO: handle multi-packet frames
    retStatus = jitterBufferGetPacket(pTransceiver->pJitterBuffer, startIndex, &pPacket);
    if (retStatus == STATUS_SUCCESS || retStatus == STATUS_HASH_KEY_NOT_PRESENT) {
        retStatus = STATUS_SUCCESS;
    } else {
        CHK(FALSE, retStatus);
    }
    CHK(pPacket != NULL, STATUS_NULL_ARG);
    MUTEX_LOCK(pTransceiver->statsLock);
    // https://www.w3.org/TR/webrtc-stats/#dom-rtcinboundrtpstreamstats-jitterbufferdelay
    pTransceiver->inboundStats.jitterBufferDelay += (DOUBLE) (GETTIME() - pPacket->receivedTime) / HUNDREDS_OF_NANOS_IN_A_SECOND;
    index = pTransceiver->inboundStats.jitterBufferEmittedCount;
    pTransceiver->inboundStats.jitterBufferEmittedCount++;
    if (MEDIA_STREAM_TRACK_KIND_VIDEO == pTransceiver->transceiver.receiver.track.kind) {
        pTransceiver->inboundStats.framesReceived++;
    }
    MUTEX_UNLOCK(pTransceiver->statsLock);

    if (frameSize > pTransceiver->peerFrameBufferSize) {
        MEMFREE(pTransceiver->peerFrameBuffer);
        pTransceiver->peerFrameBufferSize = (UINT32) (frameSize * PEER_FRAME_BUFFER_SIZE_INCREMENT_FACTOR);
        pTransceiver->peerFrameBuffer = (PBYTE) MEMALLOC(pTransceiver->peerFrameBufferSize);
        CHK(pTransceiver->peerFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);
    }

    CHK_STATUS(jitterBufferFillFrameData(pTransceiver->pJitterBuffer, pTransceiver->peerFrameBuffer, frameSize, &filledSize, startIndex, endIndex));
    CHK(frameSize == filledSize, STATUS_INVALID_ARG_LEN);

    frame.version = FRAME_CURRENT_VERSION;
    frame.decodingTs = KVS_CONVERT_TIMESCALE(pPacket->header.timestamp, pTransceiver->pJitterBuffer->clockRate, HUNDREDS_OF_NANOS_IN_A_SECOND);
    frame.presentationTs = frame.decodingTs;
    frame.frameData = pTransceiver->peerFrameBuffer;
    frame.size = frameSize;
    frame.duration = 0;
    frame.index = index;
    // TODO: Fill frame flag and track id and index if we need to, currently those are not used by RtcRtpTransceiver
    if (pTransceiver->onFrame != NULL) {
        pTransceiver->onFrame(pTransceiver->onFrameCustomData, &frame);
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS onFrameDroppedFunc(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 timestamp)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PRtpPacket pFirstPacket = NULL;
    PKvsRtpTransceiver pTransceiver = (PKvsRtpTransceiver) customData;
    UINT32 totalPartialSize = 0;
    UINT32 filledSize = 0;
    Frame frame;

    DLOGW("Frame with timestamp %u is dropped!", timestamp);
    CHK(pTransceiver != NULL, STATUS_NULL_ARG);

    // Get first available packet for stats and timestamp
    retStatus = jitterBufferGetPacket(pTransceiver->pJitterBuffer, startIndex, &pFirstPacket);
    if (retStatus == STATUS_SUCCESS || retStatus == STATUS_HASH_KEY_NOT_PRESENT) {
        retStatus = STATUS_SUCCESS;
    } else {
        CHK(FALSE, retStatus);
    }
    CHK(pFirstPacket != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pTransceiver->statsLock);
    // https://www.w3.org/TR/webrtc-stats/#dom-rtcinboundrtpstreamstats-jitterbufferdelay
    pTransceiver->inboundStats.jitterBufferDelay += (DOUBLE) (GETTIME() - pFirstPacket->receivedTime) / HUNDREDS_OF_NANOS_IN_A_SECOND;
    pTransceiver->inboundStats.jitterBufferEmittedCount++;
    pTransceiver->inboundStats.received.framesDropped++;
    pTransceiver->inboundStats.received.fullFramesLost++;
    MUTEX_UNLOCK(pTransceiver->statsLock);

    // If no partial frame callback registered, skip partial frame extraction
    CHK(pTransceiver->onPartialFrame != NULL, retStatus);

    // Calculate total size of available packets (size-query pass with NULL buffer)
    jitterBufferFillPartialFrameData(pTransceiver->pJitterBuffer, NULL, 0, &totalPartialSize, startIndex, endIndex);

    // If no data available, skip callback
    CHK(totalPartialSize > 0, retStatus);

    // Ensure buffer is large enough
    if (totalPartialSize > pTransceiver->peerFrameBufferSize) {
        MEMFREE(pTransceiver->peerFrameBuffer);
        pTransceiver->peerFrameBufferSize = (UINT32) (totalPartialSize * PEER_FRAME_BUFFER_SIZE_INCREMENT_FACTOR);
        pTransceiver->peerFrameBuffer = (PBYTE) MEMALLOC(pTransceiver->peerFrameBufferSize);
        CHK(pTransceiver->peerFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);
    }

    // Fill partial frame data (skipping missing packets)
    jitterBufferFillPartialFrameData(pTransceiver->pJitterBuffer, pTransceiver->peerFrameBuffer, totalPartialSize, &filledSize, startIndex, endIndex);

    // Build frame struct and invoke callback
    frame.version = FRAME_CURRENT_VERSION;
    frame.decodingTs = pFirstPacket->header.timestamp * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    frame.presentationTs = frame.decodingTs;
    frame.frameData = pTransceiver->peerFrameBuffer;
    frame.size = filledSize;
    frame.duration = 0;
    frame.index = pTransceiver->inboundStats.jitterBufferEmittedCount - 1;
    frame.flags = FRAME_FLAG_NONE;

    pTransceiver->onPartialFrame(pTransceiver->onPartialFrameCustomData, &frame);

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

PVOID dtlsSessionStartThread(PVOID args)
{
    ENTERS();
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) args;
    if (pKvsPeerConnection != NULL) {
        dtlsSessionHandshakeInThread(pKvsPeerConnection->pDtlsSession, pKvsPeerConnection->dtlsIsServer);
    } else {
        DLOGE("Peer connection object NULL, cannot start DTLS handshake");
    }

    LEAVES();
    return NULL;
}

VOID onIceConnectionStateChange(UINT64 customData, UINT64 connectionState)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    RTC_PEER_CONNECTION_STATE newConnectionState = RTC_PEER_CONNECTION_STATE_NEW;
    BOOL startDtlsSession = FALSE, dtlsConnected;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    switch (connectionState) {
        case ICE_AGENT_STATE_NEW:
            newConnectionState = RTC_PEER_CONNECTION_STATE_NEW;
            break;

        case ICE_AGENT_STATE_CHECK_CONNECTION:
            newConnectionState = RTC_PEER_CONNECTION_STATE_CONNECTING;
            break;

        case ICE_AGENT_STATE_CONNECTED:
            /* explicit fall-through */
        case ICE_AGENT_STATE_NOMINATING:
            newConnectionState = RTC_PEER_CONNECTION_STATE_CONNECTING;
            break;
        case ICE_AGENT_STATE_READY:
            /* start dtlsSession as soon as ice is connected */
            newConnectionState = RTC_PEER_CONNECTION_STATE_CONNECTING;
            startDtlsSession = TRUE;
            break;

        case ICE_AGENT_STATE_DISCONNECTED:
            DLOGD("ice agent disconnected");
            newConnectionState = RTC_PEER_CONNECTION_STATE_DISCONNECTED;
            break;

        case ICE_AGENT_STATE_FAILED:
            newConnectionState = RTC_PEER_CONNECTION_STATE_FAILED;
            break;

        default:
            DLOGW("Unknown ice agent state %" PRIu64, connectionState);
            break;
    }

    if (startDtlsSession) {
        CHK_STATUS(dtlsSessionIsInitFinished(pKvsPeerConnection->pDtlsSession, &dtlsConnected));
        if (dtlsConnected) {
            // In ICE restart scenario, DTLS handshake is not going to be reset. Therefore, we need to check
            // if the DTLS state has been connected.
            newConnectionState = RTC_PEER_CONNECTION_STATE_CONNECTED;
        } else {
            // PeerConnection's state changes to CONNECTED only when DTLS state is also connected. So, we need
            // wait until DTLS state changes to CONNECTED.
            //
            // Reference: https://w3c.github.io/webrtc-pc/#rtcpeerconnectionstate-enum
#if defined(ENABLE_KVS_THREADPOOL) && defined(KVS_USE_OPENSSL)
            CHK_STATUS(threadpoolContextPush(dtlsSessionStartThread, (PVOID) pKvsPeerConnection));
#else
            CHK_STATUS(dtlsSessionStart(pKvsPeerConnection->pDtlsSession, pKvsPeerConnection->dtlsIsServer));
#endif
        }
    }

    CHK_STATUS(changePeerConnectionState(pKvsPeerConnection, newConnectionState));

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
}

VOID onNewIceLocalCandidate(UINT64 customData, PCHAR candidateSdpStr)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    BOOL locked = FALSE;
    CHAR jsonStrBuffer[MAX_ICE_CANDIDATE_JSON_LEN];
    INT32 strCompleteLen = 0;
    PCHAR pIceCandidateStr = NULL;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    CHK(candidateSdpStr == NULL || STRLEN(candidateSdpStr) < MAX_SDP_ATTRIBUTE_VALUE_LENGTH, STATUS_INVALID_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    // Check inside the lock: peerConnectionOnIceCandidate() writes onIceCandidate under peerConnectionObjLock,
    // so this read must also be protected to avoid a data race.
    CHK(pKvsPeerConnection->onIceCandidate != NULL, retStatus); // do nothing if onIceCandidate is not implemented

    if (candidateSdpStr != NULL) {
        strCompleteLen = SNPRINTF(jsonStrBuffer, ARRAY_SIZE(jsonStrBuffer), ICE_CANDIDATE_JSON_TEMPLATE, candidateSdpStr);
        CHK(strCompleteLen > 0, STATUS_INTERNAL_ERROR);
        CHK(strCompleteLen < (INT32) ARRAY_SIZE(jsonStrBuffer), STATUS_BUFFER_TOO_SMALL);
        pIceCandidateStr = jsonStrBuffer;
    }

    pKvsPeerConnection->onIceCandidate(pKvsPeerConnection->onIceCandidateCustomData, pIceCandidateStr);

CleanUp:
    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }

    LEAVES();
}

VOID onSctpSessionOutboundPacket(UINT64 customData, PBYTE pPacket, UINT32 packetLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    if (customData == 0) {
        return;
    }

    pKvsPeerConnection = (PKvsPeerConnection) customData;

    // PCAP: capture plaintext outbound SCTP
    if (pKvsPeerConnection->pPcapDump != NULL) {
        pcapDumpWriteSctpPacket(pKvsPeerConnection->pPcapDump, pPacket, packetLen, PCAP_PACKET_DIRECTION_SEND);
    }

    CHK_STATUS(dtlsSessionPutApplicationData(pKvsPeerConnection->pDtlsSession, pPacket, packetLen));

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
}

VOID onSctpSessionDataChannelMessage(UINT64 customData, UINT32 channelId, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    PKvsDataChannel pKvsDataChannel = NULL;
    UINT64 hashValue = 0;

    CHK(pKvsPeerConnection != NULL, STATUS_INTERNAL_ERROR);

    retStatus = hashTableGet(pKvsPeerConnection->pDataChannels, channelId, &hashValue);
    pKvsDataChannel = (PKvsDataChannel) hashValue;
    if (retStatus == STATUS_SUCCESS || retStatus == STATUS_HASH_KEY_NOT_PRESENT) {
        retStatus = STATUS_SUCCESS;
    } else {
        CHK(FALSE, retStatus);
    }
    CHK(pKvsDataChannel != NULL && pKvsDataChannel->onMessage != NULL, STATUS_INTERNAL_ERROR);
    ATOMIC_INCREMENT(&pKvsDataChannel->atomicMessagesReceived);
    ATOMIC_ADD(&pKvsDataChannel->atomicBytesReceived, (SIZE_T) pMessageLen);
    if (STATUS_FAILED(hashTableUpsert(pKvsPeerConnection->pDataChannels, channelId, (UINT64) pKvsDataChannel))) {
        DLOGW("Failed to update entry in hash table with recent changes to data channel");
    }
    pKvsDataChannel->onMessage(pKvsDataChannel->onMessageCustomData, &pKvsDataChannel->dataChannel, isBinary, pMessage, pMessageLen);

CleanUp:

    LEAVES();
}

VOID onSctpSessionDataChannelOpen(UINT64 customData, UINT32 channelId, PBYTE pName, UINT32 nameLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    PKvsDataChannel pKvsDataChannel = NULL;

    CHK(pKvsPeerConnection != NULL && pKvsPeerConnection->onDataChannel != NULL, STATUS_NULL_ARG);

    pKvsDataChannel = (PKvsDataChannel) MEMCALLOC(1, SIZEOF(KvsDataChannel));
    CHK(pKvsDataChannel != NULL, STATUS_NOT_ENOUGH_MEMORY);

    STRNCPY(pKvsDataChannel->dataChannel.name, (PCHAR) pName, nameLen);
    pKvsDataChannel->dataChannel.id = channelId;
    pKvsDataChannel->pRtcPeerConnection = (PRtcPeerConnection) pKvsPeerConnection;
    pKvsDataChannel->channelId = channelId;

    // Set the data channel parameters when data channel is created by peer
    pKvsDataChannel->rtcDataChannelDiagnostics.dataChannelIdentifier = channelId;
    pKvsDataChannel->rtcDataChannelDiagnostics.state = RTC_DATA_CHANNEL_STATE_OPEN;
    STRNCPY(pKvsDataChannel->rtcDataChannelDiagnostics.label, (PCHAR) pName, nameLen);
    CHK_STATUS(hashTablePut(pKvsPeerConnection->pDataChannels, channelId, (UINT64) pKvsDataChannel));
    pKvsPeerConnection->onDataChannel(pKvsPeerConnection->onDataChannelCustomData, &(pKvsDataChannel->dataChannel));

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
}

VOID onDtlsOutboundPacket(UINT64 customData, PBYTE pBuffer, UINT32 bufferLen)
{
    ENTERS();
    PKvsPeerConnection pKvsPeerConnection = NULL;
    if (customData == 0) {
        return;
    }

    pKvsPeerConnection = (PKvsPeerConnection) customData;
    iceAgentSendPacket(pKvsPeerConnection->pIceAgent, pBuffer, bufferLen);
    LEAVES();
}

VOID onDtlsStateChange(UINT64 customData, RTC_DTLS_TRANSPORT_STATE newDtlsState)
{
    ENTERS();
    PKvsPeerConnection pKvsPeerConnection = NULL;
    if (customData == 0) {
        return;
    }

    pKvsPeerConnection = (PKvsPeerConnection) customData;

    switch (newDtlsState) {
        case RTC_DTLS_TRANSPORT_STATE_CONNECTED:
            pKvsPeerConnection->peerConnectionDiagnostics.dtlsSessionSetupTime = pKvsPeerConnection->pDtlsSession->dtlsSessionSetupTime;
            break;
        case RTC_DTLS_TRANSPORT_STATE_CLOSED:
            changePeerConnectionState(pKvsPeerConnection, RTC_PEER_CONNECTION_STATE_CLOSED);
            break;
        default:
            /* explicit ignore */
            break;
    }
    LEAVES();
}

/* Generate a printable string that does not
 * need to be escaped when encoding in JSON
 */
STATUS generateJSONSafeString(PCHAR pDst, UINT32 len)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 i = 0;

    CHK(pDst != NULL, STATUS_NULL_ARG);

    for (i = 0; i < len; i++) {
        pDst[i] = VALID_CHAR_SET_FOR_JSON[RAND() % (ARRAY_SIZE(VALID_CHAR_SET_FOR_JSON) - 1)];
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS rtcpReportsCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    ENTERS();
    UNUSED_PARAM(timerId);
    STATUS retStatus = STATUS_SUCCESS;
    BOOL ready = FALSE;
    UINT64 ntpTime, rtpTime, delay;
    UINT32 packetCount, octetCount, packetLen, allocSize, ssrc;
    PBYTE rawPacket = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;

    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) customData;
    CHK(pKvsRtpTransceiver != NULL && pKvsRtpTransceiver->pJitterBuffer != NULL && pKvsRtpTransceiver->pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    pKvsPeerConnection = pKvsRtpTransceiver->pKvsPeerConnection;

    ssrc = pKvsRtpTransceiver->sender.ssrc;
    DLOGS("rtcpReportsCallback %" PRIu64 " ssrc: %u rtxssrc: %u", currentTime, ssrc, pKvsRtpTransceiver->sender.rtxSsrc);

    // check if ice agent is connected, reschedule in 200msec if not
    ready = pKvsPeerConnection->pSrtpSession != NULL && pKvsRtpTransceiver->sender.firstFrameWallClockTime != 0 &&
        currentTime - pKvsRtpTransceiver->sender.firstFrameWallClockTime >= 2500 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    if (!ready) {
        DLOGV("sender report no frames sent %u", ssrc);
    } else {
        // create rtcp sender report packet
        // https://tools.ietf.org/html/rfc3550#section-6.4.1
        ntpTime = convertTimestampToNTP(currentTime);
        rtpTime = pKvsRtpTransceiver->sender.rtpTimeOffset +
            CONVERT_TIMESTAMP_TO_RTP(pKvsRtpTransceiver->pJitterBuffer->clockRate, currentTime - pKvsRtpTransceiver->sender.firstFrameWallClockTime);
        MUTEX_LOCK(pKvsRtpTransceiver->statsLock);
        packetCount = pKvsRtpTransceiver->outboundStats.sent.packetsSent;
        octetCount = pKvsRtpTransceiver->outboundStats.sent.bytesSent;
        MUTEX_UNLOCK(pKvsRtpTransceiver->statsLock);
        DLOGV("sender report %u %" PRIu64 " %" PRIu64 " : %u packets %u bytes", ssrc, ntpTime, rtpTime, packetCount, octetCount);
        packetLen = RTCP_PACKET_HEADER_LEN + 24;

        // srtp_protect_rtcp() in encryptRtcpPacket() assumes memory availability to write 10 bytes of authentication tag and
        // SRTP_MAX_TRAILER_LEN + 4 following the actual rtcp Packet payload
        allocSize = packetLen + SRTP_AUTH_TAG_OVERHEAD + SRTP_MAX_TRAILER_LEN + 4;
        CHK(NULL != (rawPacket = (PBYTE) MEMALLOC(allocSize)), STATUS_NOT_ENOUGH_MEMORY);
        rawPacket[0] = RTCP_PACKET_VERSION_VAL << 6;
        rawPacket[RTCP_PACKET_TYPE_OFFSET] = RTCP_PACKET_TYPE_SENDER_REPORT;
        putUnalignedInt16BigEndian(rawPacket + RTCP_PACKET_LEN_OFFSET,
                                   (packetLen / RTCP_PACKET_LEN_WORD_SIZE) - 1); // The length of this RTCP packet in 32-bit words minus one
        putUnalignedInt32BigEndian(rawPacket + 4, ssrc);
        putUnalignedInt64BigEndian(rawPacket + 8, ntpTime);
        putUnalignedInt32BigEndian(rawPacket + 16, rtpTime);
        putUnalignedInt32BigEndian(rawPacket + 20, packetCount);
        putUnalignedInt32BigEndian(rawPacket + 24, octetCount);

        // PCAP: capture unencrypted outbound RTCP (Sender Report)
        if (pKvsPeerConnection->pPcapDump != NULL) {
            pcapDumpWritePacket(pKvsPeerConnection->pPcapDump, rawPacket, packetLen, TRUE, PCAP_PACKET_DIRECTION_SEND);
        }

        CHK_STATUS(encryptRtcpPacket(pKvsPeerConnection->pSrtpSession, rawPacket, (PINT32) &packetLen));
        CHK_STATUS(iceAgentSendPacket(pKvsPeerConnection->pIceAgent, rawPacket, packetLen));
    }

    // Send RTCP Receiver Report if we are receiving media on this transceiver
    if (pKvsPeerConnection->pSrtpSession != NULL && pKvsRtpTransceiver->jitterBufferSsrc != 0 && pKvsRtpTransceiver->rrSeqInitialized) {
        UINT32 rrExtMax, rrExpected, rrReceived, rrLost, rrExpectedInterval, rrReceivedInterval, rrLostInterval;
        UINT8 rrFraction;
        INT32 rrCumulativeLost;
        UINT32 rrPacketLen;
        UINT32 rrLastSRNtpMid;
        UINT64 rrLastSRReceivedTime;
        BYTE rrPacket[RTCP_PACKET_HEADER_LEN + 4 + RTCP_PACKET_RECEIVER_REPORT_BLOCK_LEN]; // header + sender SSRC + 1 report block

        MUTEX_LOCK(pKvsRtpTransceiver->statsLock);
        rrExtMax = pKvsRtpTransceiver->rrCycles + pKvsRtpTransceiver->rrMaxSeq;
        rrExpected = rrExtMax - pKvsRtpTransceiver->rrBaseSeq + 1;
        rrReceived = (UINT32) pKvsRtpTransceiver->inboundStats.received.packetsReceived;

        rrLost = rrExpected - rrReceived;

        rrExpectedInterval = rrExpected - pKvsRtpTransceiver->rrExpectedPrior;
        pKvsRtpTransceiver->rrExpectedPrior = rrExpected;
        rrReceivedInterval = rrReceived - pKvsRtpTransceiver->rrReceivedPrior;
        pKvsRtpTransceiver->rrReceivedPrior = rrReceived;
        rrLostInterval = rrExpectedInterval - rrReceivedInterval;

        if (rrExpectedInterval == 0 || rrLostInterval == 0) {
            rrFraction = 0;
        } else {
            rrFraction = (UINT8) ((rrLostInterval << 8) / rrExpectedInterval);
        }

        // Clamp cumulative lost to 24-bit signed range [-0x800000, 0x7FFFFF]
        if ((INT32) rrLost > 0x7FFFFF) {
            rrCumulativeLost = 0x7FFFFF;
        } else if ((INT32) rrLost < -0x800000) {
            rrCumulativeLost = -0x800000;
        } else {
            rrCumulativeLost = (INT32) rrLost;
        }
        rrLastSRNtpMid = pKvsRtpTransceiver->lastSRNtpMid;
        rrLastSRReceivedTime = pKvsRtpTransceiver->lastSRReceivedTime;
        MUTEX_UNLOCK(pKvsRtpTransceiver->statsLock);

        rrPacketLen = sizeof(rrPacket);
        MEMSET(rrPacket, 0, rrPacketLen);
        rrPacket[0] = (RTCP_PACKET_VERSION_VAL << 6) | 1; // V=2, RC=1
        rrPacket[RTCP_PACKET_TYPE_OFFSET] = RTCP_PACKET_TYPE_RECEIVER_REPORT;
        putUnalignedInt16BigEndian(rrPacket + RTCP_PACKET_LEN_OFFSET, (rrPacketLen / RTCP_PACKET_LEN_WORD_SIZE) - 1);
        putUnalignedInt32BigEndian(rrPacket + 4, ssrc); // sender SSRC (our SSRC)
        // Report block
        putUnalignedInt32BigEndian(rrPacket + 8, pKvsRtpTransceiver->jitterBufferSsrc); // SSRC_1 (source)
        rrPacket[12] = rrFraction;
        rrPacket[13] = (rrCumulativeLost >> 16) & 0xFF;
        rrPacket[14] = (rrCumulativeLost >> 8) & 0xFF;
        rrPacket[15] = rrCumulativeLost & 0xFF;
        putUnalignedInt32BigEndian(rrPacket + 16, rrExtMax);                                             // extended highest sequence number
        putUnalignedInt32BigEndian(rrPacket + 20, (UINT32) (pKvsRtpTransceiver->pJitterBuffer->jitter)); // interarrival jitter
        putUnalignedInt32BigEndian(rrPacket + 24, rrLastSRNtpMid);                                       // LSR

        // DLSR: delay since last SR in 1/65536 sec units
        if (rrLastSRReceivedTime != 0) {
            UINT64 dlsrTime = currentTime - rrLastSRReceivedTime;
            UINT32 dlsr = (UINT32) KVS_CONVERT_TIMESCALE(dlsrTime, HUNDREDS_OF_NANOS_IN_A_SECOND, DLSR_TIMESCALE);
            putUnalignedInt32BigEndian(rrPacket + 28, dlsr);
        }

        DLOGV("receiver report ssrc: %u src: %u frac: %u lost: %d seq: %u", ssrc, pKvsRtpTransceiver->jitterBufferSsrc, rrFraction, rrCumulativeLost,
              rrExtMax);
        {
            UINT32 rrAllocSize = rrPacketLen + SRTP_AUTH_TAG_OVERHEAD + SRTP_MAX_TRAILER_LEN + 4;
            PBYTE rrRawPacket = (PBYTE) MEMALLOC(rrAllocSize);
            CHK(rrRawPacket != NULL, STATUS_NOT_ENOUGH_MEMORY);
            MEMCPY(rrRawPacket, rrPacket, rrPacketLen);

            // PCAP: capture unencrypted outbound RTCP (Receiver Report)
            if (pKvsPeerConnection->pPcapDump != NULL) {
                pcapDumpWritePacket(pKvsPeerConnection->pPcapDump, rrRawPacket, rrPacketLen, TRUE, PCAP_PACKET_DIRECTION_SEND);
            }

            retStatus = encryptRtcpPacket(pKvsPeerConnection->pSrtpSession, rrRawPacket, (PINT32) &rrPacketLen);
            if (STATUS_SUCCEEDED(retStatus)) {
                retStatus = iceAgentSendPacket(pKvsPeerConnection->pIceAgent, rrRawPacket, rrPacketLen);
            }
            SAFE_MEMFREE(rrRawPacket);
            CHK_STATUS(retStatus);
        }
    }

    delay = 100 + (RAND() % 200);
    DLOGS("next sender report %u in %" PRIu64 " msec", ssrc, delay);
    // reschedule timer with 200msec +- 100ms
    CHK_STATUS(timerQueueAddTimer(pKvsPeerConnection->timerQueueHandle, delay * HUNDREDS_OF_NANOS_IN_A_MILLISECOND,
                                  TIMER_QUEUE_SINGLE_INVOCATION_PERIOD, rtcpReportsCallback, (UINT64) pKvsRtpTransceiver,
                                  &pKvsRtpTransceiver->rtcpReportsTimerId));

CleanUp:
    CHK_LOG_ERR(retStatus);
    SAFE_MEMFREE(rawPacket);

    LEAVES();
    return retStatus;
}

// Not thread safe
STATUS getStunAddr(PStunIpAddrContext pStunIpAddrCtx)
{
    ENTERS();
    INT32 errCode;
    STATUS retStatus = STATUS_SUCCESS;
    struct addrinfo *rp, *res;
    struct sockaddr_in* ipv4Addr;
    struct sockaddr_in6* ipv6Addr;
    BOOL ipv4Resolved = FALSE;
    BOOL ipv6Resolved = FALSE;

    // Initialize IP address families to a sentinel value
    // to indicate that they are not set.
    pStunIpAddrCtx->kvsIpAddresses.ipv4Address.family = KVS_IP_FAMILY_TYPE_NOT_SET;
    pStunIpAddrCtx->kvsIpAddresses.ipv6Address.family = KVS_IP_FAMILY_TYPE_NOT_SET;
    pStunIpAddrCtx->kvsIpAddresses.ipv4Address.port = 0;
    pStunIpAddrCtx->kvsIpAddresses.ipv6Address.port = 0;

    // Don't attempt to resolve IPv6 address if not in dual-stack mode.
    if (!isEnvVarEnabled(USE_DUAL_STACK_ENDPOINTS_ENV_VAR)) {
        ipv6Resolved = TRUE;
    }

    DLOGD("Resolving STUN server address for hostname: %s", pStunIpAddrCtx->hostname);

    errCode = getaddrinfo(pStunIpAddrCtx->hostname, NULL, NULL, &res);
    if (errCode != 0) {
        DLOGI("Failed to resolve hostname with errcode: %d", errCode);
        retStatus = STATUS_RESOLVE_HOSTNAME_FAILED;
    } else {
        for (rp = res; rp != NULL && !(ipv4Resolved && ipv6Resolved); rp = rp->ai_next) {
            if (!ipv4Resolved && rp->ai_family == AF_INET) {
                DLOGD("Found an IPv4 STUN addresss for hostname: %s", pStunIpAddrCtx->hostname);
                ipv4Addr = (struct sockaddr_in*) rp->ai_addr;
                pStunIpAddrCtx->kvsIpAddresses.ipv4Address.family = KVS_IP_FAMILY_TYPE_IPV4;
                MEMCPY(pStunIpAddrCtx->kvsIpAddresses.ipv4Address.address, &ipv4Addr->sin_addr, IPV4_ADDRESS_LENGTH);
                ipv4Resolved = TRUE;
            } else if (!ipv6Resolved && rp->ai_family == AF_INET6) {
                DLOGD("Found an IPv6 STUN addresss for hostname: %s", pStunIpAddrCtx->hostname);
                ipv6Addr = (struct sockaddr_in6*) rp->ai_addr;
                pStunIpAddrCtx->kvsIpAddresses.ipv6Address.family = KVS_IP_FAMILY_TYPE_IPV6;
                MEMCPY(pStunIpAddrCtx->kvsIpAddresses.ipv6Address.address, &ipv6Addr->sin6_addr, IPV6_ADDRESS_LENGTH);
                ipv6Resolved = TRUE;
            } else {
                DLOGD("Invalid family STUN addresss for hostname: %s", pStunIpAddrCtx->hostname);
            }
        }
        freeaddrinfo(res);
    }
    if (!(ipv4Resolved || ipv6Resolved)) {
        retStatus = STATUS_RESOLVE_HOSTNAME_FAILED;
    }

    CHK_LOG_ERR(retStatus);
    LEAVES();
    return retStatus;
}

STATUS onSetStunServerIp(UINT64 customData, PCHAR url, PDualKvsIpAddresses pIpAddresses)
{
    ENTERS();
    UNUSED_PARAM(customData);
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PWebRtcClientContext pWebRtcClientContext = getWebRtcClientInstance();
    CHK_WARN(ATOMIC_LOAD_BOOL(&pWebRtcClientContext->isContextInitialized), STATUS_NULL_ARG, "WebRTC Client object Object not initialized");

    UINT64 currentTime = GETTIME();

    MUTEX_LOCK(pWebRtcClientContext->stunCtxlock);
    locked = TRUE;

    // This covers a situation where say we receive a URL that is not the default STUN or the hostname is not populated
    // pWebRtcClientContext->pStunIpAddrCtx->status needs to be set to ensure we do not go ahead with resolution on thread
    // in case we receive the request early on
    if (STRCMP(url, pWebRtcClientContext->pStunIpAddrCtx->hostname) != 0) {
        retStatus = STATUS_PEERCONNECTION_EARLY_DNS_RESOLUTION_FAILED;
        // This is to ensure we do not go ahead with STUN resolution if this call is already made
        pWebRtcClientContext->pStunIpAddrCtx->status = STATUS_PEERCONNECTION_EARLY_DNS_RESOLUTION_FAILED;
    } else {
        if (pWebRtcClientContext->pStunIpAddrCtx->isIpInitialized) {
            DLOGI("Initialized successfully");
            if (currentTime > (pWebRtcClientContext->pStunIpAddrCtx->startTime + pWebRtcClientContext->pStunIpAddrCtx->expirationDuration)) {
                DLOGI("Expired...need to refresh STUN address");
                // Reset start time
                pWebRtcClientContext->pStunIpAddrCtx->startTime = 0;
                CHK_ERR(getStunAddr(pWebRtcClientContext->pStunIpAddrCtx) == STATUS_SUCCESS, retStatus, "Failed to resolve after cache expiry");
            }
            MEMCPY(pIpAddresses->ipv4Address.address, &pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv4Address,
                   SIZEOF(pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv4Address));
            MEMCPY(pIpAddresses->ipv6Address.address, &pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv6Address,
                   SIZEOF(pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv6Address));
        } else {
            DLOGE("Initialization failed");
        }
    }
CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pWebRtcClientContext->stunCtxlock);
    }
    DLOGD("Exiting from stun server IP callback");
    releaseHoldOnInstance(pWebRtcClientContext);
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

#ifdef ENABLE_KVS_THREADPOOL
PVOID resolveStunIceServerIp(PVOID args)
{
    ENTERS();
    UNUSED_PARAM(args);
    PWebRtcClientContext pWebRtcClientContext = getWebRtcClientInstance();
    BOOL locked = FALSE;
    CHAR addressResolvedIPv4[KVS_IP_ADDRESS_STRING_BUFFER_LEN + 1] = {'\0'};
    CHAR addressResolvedIPv6[KVS_IP_ADDRESS_STRING_BUFFER_LEN + 1] = {'\0'};
    PCHAR pRegion;
    PCHAR pHostnamePostfix;
    UINT64 stunDnsResolutionStartTime = 0;

    if (ATOMIC_LOAD_BOOL(&pWebRtcClientContext->isContextInitialized)) {
        MUTEX_LOCK(pWebRtcClientContext->stunCtxlock);
        locked = TRUE;
        if (pWebRtcClientContext->pStunIpAddrCtx == NULL) {
            DLOGE("Failed to resolve STUN IP address because webrtc client instance was not created");
        } else {
            if (pWebRtcClientContext->pStunIpAddrCtx->status != STATUS_PEERCONNECTION_EARLY_DNS_RESOLUTION_FAILED) {
                if ((pRegion = GETENV(DEFAULT_REGION_ENV_VAR)) == NULL) {
                    pRegion = DEFAULT_AWS_REGION;
                }

                if (isEnvVarEnabled(USE_DUAL_STACK_ENDPOINTS_ENV_VAR)) {
                    if (STRSTR(pRegion, "cn-")) {
                        pHostnamePostfix = KINESIS_VIDEO_DUALSTACK_STUN_URL_POSTFIX_CN;
                    } else {
                        pHostnamePostfix = KINESIS_VIDEO_DUALSTACK_STUN_URL_POSTFIX;
                    }
                } else {
                    if (STRSTR(pRegion, "cn-")) {
                        pHostnamePostfix = KINESIS_VIDEO_STUN_URL_POSTFIX_CN;
                    } else {
                        pHostnamePostfix = KINESIS_VIDEO_STUN_URL_POSTFIX;
                    }
                }

                SNPRINTF(pWebRtcClientContext->pStunIpAddrCtx->hostname, SIZEOF(pWebRtcClientContext->pStunIpAddrCtx->hostname),
                         KINESIS_VIDEO_STUN_URL_WITHOUT_PORT, pRegion, pHostnamePostfix);
                stunDnsResolutionStartTime = GETTIME();
                if (getStunAddr(pWebRtcClientContext->pStunIpAddrCtx) == STATUS_SUCCESS) {
                    if (pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv4Address.family != KVS_IP_FAMILY_TYPE_NOT_SET) {
                        // If the IPv4 family is set, then there must have been an IPv4 address resolved.
                        getIpAddrStr(&pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv4Address, addressResolvedIPv4,
                                     ARRAY_SIZE(addressResolvedIPv4));
                        DLOGI("ICE Server address for %s with getaddrinfo: %s", pWebRtcClientContext->pStunIpAddrCtx->hostname, addressResolvedIPv4);
                        pWebRtcClientContext->pStunIpAddrCtx->isIpInitialized = TRUE;
                    }
                    if (pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv6Address.family != KVS_IP_FAMILY_TYPE_NOT_SET) {
                        // If the IPv6 family is set, then there must have been an IPv6 address resolved.
                        getIpAddrStr(&pWebRtcClientContext->pStunIpAddrCtx->kvsIpAddresses.ipv6Address, addressResolvedIPv6,
                                     ARRAY_SIZE(addressResolvedIPv6));
                        DLOGI("ICE Server address for %s with getaddrinfo: %s", pWebRtcClientContext->pStunIpAddrCtx->hostname, addressResolvedIPv6);
                        pWebRtcClientContext->pStunIpAddrCtx->isIpInitialized = TRUE;
                    }

                } else {
                    DLOGE("Failed to resolve %s", pWebRtcClientContext->pStunIpAddrCtx->hostname);
                }
                pWebRtcClientContext->pStunIpAddrCtx->startTime = GETTIME();
            } else {
                DLOGW("Request already received to get the URL before resolution could even start...allowing higher layers to handle resolution");
            }
            PROFILE_WITH_START_TIME_OBJ(stunDnsResolutionStartTime, pWebRtcClientContext->pStunIpAddrCtx->stunDnsResolutionTime,
                                        "STUN DNS resolution time taken");
        }
        if (locked) {
            MUTEX_UNLOCK(pWebRtcClientContext->stunCtxlock);
        }
    } else {
        DLOGW("STUN DNS thread invoked without context being initialized");
    }
    releaseHoldOnInstance(pWebRtcClientContext);
    DLOGD("Exiting from stun server IP resolution thread");

    LEAVES();
    return NULL;
}
#endif

STATUS createPeerConnection(PRtcConfiguration pConfiguration, PRtcPeerConnection* ppPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    IceAgentCallbacks iceAgentCallbacks;
    DtlsSessionCallbacks dtlsSessionCallbacks;
    PConnectionListener pConnectionListener = NULL;
    UINT64 startTime = 0;
    UINT64 startTimeInMacro = 0;

    CHK(pConfiguration != NULL && ppPeerConnection != NULL, STATUS_NULL_ARG);

    startTime = GETTIME();
    MEMSET(&iceAgentCallbacks, 0, SIZEOF(IceAgentCallbacks));
    MEMSET(&dtlsSessionCallbacks, 0, SIZEOF(DtlsSessionCallbacks));

    pKvsPeerConnection = (PKvsPeerConnection) MEMCALLOC(1, SIZEOF(KvsPeerConnection));
    CHK(pKvsPeerConnection != NULL, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(timerQueueCreate(&pKvsPeerConnection->timerQueueHandle));

    pKvsPeerConnection->peerConnection.version = PEER_CONNECTION_CURRENT_VERSION;

    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIceUfrag, LOCAL_ICE_UFRAG_LEN));
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIcePwd, LOCAL_ICE_PWD_LEN));
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localCNAME, LOCAL_CNAME_LEN));

    PROFILE_CALL(CHK_STATUS(createDtlsSession(
                     &dtlsSessionCallbacks, pKvsPeerConnection->timerQueueHandle, pConfiguration->kvsRtcConfiguration.generatedCertificateBits,
                     pConfiguration->kvsRtcConfiguration.generateRSACertificate, pConfiguration->certificates, &pKvsPeerConnection->pDtlsSession)),
                 "Create DTLS Session object");
    CHK_STATUS(dtlsSessionOnOutBoundData(pKvsPeerConnection->pDtlsSession, (UINT64) pKvsPeerConnection, onDtlsOutboundPacket));
    CHK_STATUS(dtlsSessionOnStateChange(pKvsPeerConnection->pDtlsSession, (UINT64) pKvsPeerConnection, onDtlsStateChange));

    CHK_STATUS(hashTableCreateWithParams(CODEC_HASH_TABLE_BUCKET_COUNT, CODEC_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pCodecTable));
    CHK_STATUS(hashTableCreateWithParams(CODEC_HASH_TABLE_BUCKET_COUNT, CODEC_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pDataChannels));
    CHK_STATUS(hashTableCreateWithParams(RTX_HASH_TABLE_BUCKET_COUNT, RTX_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pRtxTable));
    CHK_STATUS(doubleListCreate(&(pKvsPeerConnection->pTransceivers)));
    CHK_STATUS(doubleListCreate(&(pKvsPeerConnection->pFakeTransceivers)));
    CHK_STATUS(doubleListCreate(&(pKvsPeerConnection->pAnswerTransceivers)));

    pKvsPeerConnection->pSrtpSessionLock = MUTEX_CREATE(TRUE);
    pKvsPeerConnection->peerConnectionObjLock = MUTEX_CREATE(FALSE);
    pKvsPeerConnection->connectionState = RTC_PEER_CONNECTION_STATE_NONE;
    pKvsPeerConnection->MTU = pConfiguration->kvsRtcConfiguration.maximumTransmissionUnit == 0
        ? DEFAULT_MTU_SIZE_BYTES
        : pConfiguration->kvsRtcConfiguration.maximumTransmissionUnit;
    pKvsPeerConnection->jitterBufferMaxLatency = pConfiguration->kvsRtcConfiguration.jitterBufferMaxLatency == 0
        ? DEFAULT_JITTER_BUFFER_MAX_LATENCY
        : pConfiguration->kvsRtcConfiguration.jitterBufferMaxLatency;
    pKvsPeerConnection->useRealTimeJitterBuffer = pConfiguration->kvsRtcConfiguration.useRealTimeJitterBuffer;
    ATOMIC_STORE_BOOL(&pKvsPeerConnection->sctpIsEnabled, FALSE);
    ATOMIC_STORE_BOOL(&pKvsPeerConnection->receiveEnabled, TRUE);

    iceAgentCallbacks.customData = (UINT64) pKvsPeerConnection;
    iceAgentCallbacks.inboundPacketFn = onInboundPacket;
    iceAgentCallbacks.connectionStateChangedFn = onIceConnectionStateChange;
    iceAgentCallbacks.newLocalCandidateFn = onNewIceLocalCandidate;
    iceAgentCallbacks.setStunServerIpFn = onSetStunServerIp;

    PROFILE_CALL(CHK_STATUS(createConnectionListener(&pConnectionListener)), "Create connection listener");
    // IceAgent will own the lifecycle of pConnectionListener;
    PROFILE_CALL(CHK_STATUS(createIceAgent(pKvsPeerConnection->localIceUfrag, pKvsPeerConnection->localIcePwd, &iceAgentCallbacks, pConfiguration,
                                           pKvsPeerConnection->timerQueueHandle, pConnectionListener, &pKvsPeerConnection->pIceAgent)),
                 "Create ICE agent object");

    NULLABLE_SET_EMPTY(pKvsPeerConnection->canTrickleIce);

    if (!pConfiguration->kvsRtcConfiguration.disableSenderSideBandwidthEstimation) {
        pKvsPeerConnection->twccLock = MUTEX_CREATE(TRUE);
        pKvsPeerConnection->pTwccManager = (PTwccManager) MEMCALLOC(1, SIZEOF(TwccManager));
        CHK(pKvsPeerConnection->pTwccManager != NULL, STATUS_NOT_ENOUGH_MEMORY);
        CHK_STATUS(hashTableCreateWithParams(TWCC_HASH_TABLE_BUCKET_COUNT, TWCC_HASH_TABLE_BUCKET_LENGTH,
                                             &pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable));
        // Set default TWCC extension ID so the offer SDP includes the extmap.
        // If the remote peer offers a different ID, it will be overwritten
        // during setRemoteDescription.
        pKvsPeerConnection->twccExtId = TWCC_DEFAULT_EXT_ID;
    }

    // TWCC feedback generation (receiver side)
    pKvsPeerConnection->twccReceiverLock = MUTEX_CREATE(TRUE);
    CHK_STATUS(createTwccReceiverManager(&pKvsPeerConnection->pTwccReceiverManager));
    pKvsPeerConnection->twccFeedbackTimerId = MAX_UINT32; // Invalid timer ID
#ifdef ENABLE_NATIVE_SCTP
    pKvsPeerConnection->sctpTimerCallbackId = MAX_UINT32;
#endif

    // PCAP dump
    if (pConfiguration->kvsRtcConfiguration.pcapFilePath[0] != '\0') {
        CHK_STATUS(pcapDumpCreate(pConfiguration->kvsRtcConfiguration.pcapFilePath, &pKvsPeerConnection->pPcapDump));
        DLOGI("PCAP dump enabled: %s", pConfiguration->kvsRtcConfiguration.pcapFilePath);
    }

    *ppPeerConnection = (PRtcPeerConnection) pKvsPeerConnection;

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        freePeerConnection((PRtcPeerConnection*) &pKvsPeerConnection);
    } else {
        PROFILE_WITH_START_TIME_OBJ(startTime, pKvsPeerConnection->peerConnectionDiagnostics.peerConnectionCreationTime,
                                    "Peer connection object creation time");
    }

    LEAVES();
    return retStatus;
}

STATUS freeHashEntry(UINT64 customData, PHashEntry pHashEntry)
{
    ENTERS();
    UNUSED_PARAM(customData);
    MEMFREE((PVOID) pHashEntry->value);

    LEAVES();
    return STATUS_SUCCESS;
}

/*
 * NOT thread-safe
 */
STATUS freePeerConnection(PRtcPeerConnection* ppPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    PDoubleListNode pCurNode = NULL;
    UINT64 item = 0;
    UINT64 startTime;
    BOOL twccLocked = FALSE;

    CHK(ppPeerConnection != NULL, STATUS_NULL_ARG);

    pKvsPeerConnection = (PKvsPeerConnection) *ppPeerConnection;

    CHK(pKvsPeerConnection != NULL, retStatus);

    startTime = GETTIME();
    /* Prevent the receive thread from processing any more inbound packets
     * while we are tearing down.  This must happen before iceAgentShutdown
     * because packets may already be in-flight in the callback chain. */
    ATOMIC_STORE_BOOL(&pKvsPeerConnection->receiveEnabled, FALSE);
    ATOMIC_STORE_BOOL(&pKvsPeerConnection->sctpIsEnabled, FALSE);

    /* Shutdown IceAgent first so there is no more incoming packets which can cause
     * SCTP to be allocated again after SCTP is freed. */
    CHK_LOG_ERR(iceAgentShutdown(pKvsPeerConnection->pIceAgent));

    // free timer queue first to remove liveness provided by timer
    if (IS_VALID_TIMER_QUEUE_HANDLE(pKvsPeerConnection->timerQueueHandle)) {
        if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->twccLock)) {
            UINT32 twccTimerId;
            MUTEX_LOCK(pKvsPeerConnection->twccLock);
            twccTimerId = pKvsPeerConnection->twccFeedbackTimerId;
            pKvsPeerConnection->twccFeedbackTimerId = MAX_UINT32;
            MUTEX_UNLOCK(pKvsPeerConnection->twccLock);
            if (twccTimerId != MAX_UINT32) {
                timerQueueCancelTimer(pKvsPeerConnection->timerQueueHandle, twccTimerId, (UINT64) pKvsPeerConnection);
            }
        }
#ifdef ENABLE_NATIVE_SCTP
        if (pKvsPeerConnection->sctpTimerCallbackId != MAX_UINT32) {
            timerQueueCancelTimer(pKvsPeerConnection->timerQueueHandle, pKvsPeerConnection->sctpTimerCallbackId, (UINT64) pKvsPeerConnection);
            pKvsPeerConnection->sctpTimerCallbackId = MAX_UINT32;
        }
#endif
        if (pKvsPeerConnection->pPacer != NULL) {
            pacerStop(pKvsPeerConnection->pPacer);
        }
        timerQueueShutdown(pKvsPeerConnection->timerQueueHandle);
    }

    /* Free structs that have their own thread. SCTP has threads created by SCTP library. IceAgent has the
     * connectionListener thread. Free SCTP first so it wont try to send anything through ICE. */
#ifdef ENABLE_DATA_CHANNEL
    CHK_LOG_ERR(freeSctpSession(&pKvsPeerConnection->pSctpSession));
#endif

    // free transceivers
    CHK_LOG_ERR(doubleListGetHeadNode(pKvsPeerConnection->pTransceivers, &pCurNode));
    while (pCurNode != NULL) {
        CHK_LOG_ERR(doubleListGetNodeData(pCurNode, &item));
        CHK_LOG_ERR(freeKvsRtpTransceiver((PKvsRtpTransceiver*) &item));

        pCurNode = pCurNode->pNext;
    }

    CHK_LOG_ERR(doubleListGetHeadNode(pKvsPeerConnection->pFakeTransceivers, &pCurNode));
    while (pCurNode != NULL) {
        CHK_LOG_ERR(doubleListGetNodeData(pCurNode, &item));
        CHK_LOG_ERR(freeKvsRtpTransceiver((PKvsRtpTransceiver*) &item));

        pCurNode = pCurNode->pNext;
    }

    // Free DataChannels
    CHK_LOG_ERR(hashTableIterateEntries(pKvsPeerConnection->pDataChannels, 0, freeHashEntry));
    CHK_LOG_ERR(hashTableFree(pKvsPeerConnection->pDataChannels));

    // free rest of structs
    if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->pSrtpSessionLock)) {
        MUTEX_LOCK(pKvsPeerConnection->pSrtpSessionLock);
        CHK_LOG_ERR(freeSrtpSession(&pKvsPeerConnection->pSrtpSession));
        MUTEX_UNLOCK(pKvsPeerConnection->pSrtpSessionLock);
    }
    CHK_LOG_ERR(freeDtlsSession(&pKvsPeerConnection->pDtlsSession));
    // Since ICE agent has a callback invoked from DTLS during handshake,
    // it is safer to free the ICE agent after DTLS session
    CHK_LOG_ERR(freeIceAgent(&pKvsPeerConnection->pIceAgent));
    CHK_LOG_ERR(doubleListFree(pKvsPeerConnection->pTransceivers));
    CHK_LOG_ERR(doubleListFree(pKvsPeerConnection->pFakeTransceivers));
    CHK_LOG_ERR(doubleListFree(pKvsPeerConnection->pAnswerTransceivers));
    CHK_LOG_ERR(hashTableFree(pKvsPeerConnection->pCodecTable));
    CHK_LOG_ERR(hashTableFree(pKvsPeerConnection->pRtxTable));
    if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->pSrtpSessionLock)) {
        MUTEX_FREE(pKvsPeerConnection->pSrtpSessionLock);
    }

    if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->peerConnectionObjLock)) {
        MUTEX_FREE(pKvsPeerConnection->peerConnectionObjLock);
    }

    // Free pacer before timer queue since pacerStop cancels its timer
    if (pKvsPeerConnection->pPacer != NULL) {
        CHK_LOG_ERR(freePacer(&pKvsPeerConnection->pPacer));
    }

    if (IS_VALID_TIMER_QUEUE_HANDLE(pKvsPeerConnection->timerQueueHandle)) {
        timerQueueFree(&pKvsPeerConnection->timerQueueHandle);
    }

    // Free pacer (after timer queue since pacer uses timers)
    if (pKvsPeerConnection->pPacer != NULL) {
        CHK_LOG_ERR(freePacer(&pKvsPeerConnection->pPacer));
    }

    if (pKvsPeerConnection->pTwccManager != NULL) {
        MUTEX_LOCK(pKvsPeerConnection->twccLock);
        twccLocked = TRUE;

        CHK_LOG_ERR(hashTableIterateEntries(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable, 0, freeHashEntry));
        CHK_LOG_ERR(hashTableFree(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable));

        SAFE_MEMFREE(pKvsPeerConnection->pTwccManager);
    }

    // Incase the `RemoteSessionDescription` has not already been freed.
    SAFE_MEMFREE(pKvsPeerConnection->pRemoteSessionDescription);

    if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->twccLock)) {
        if (twccLocked) {
            MUTEX_UNLOCK(pKvsPeerConnection->twccLock);
            twccLocked = FALSE;
        }
        MUTEX_FREE(pKvsPeerConnection->twccLock);
        pKvsPeerConnection->twccLock = INVALID_MUTEX_VALUE;
    }

    // Free TWCC receiver manager
    if (pKvsPeerConnection->pTwccReceiverManager != NULL) {
        CHK_LOG_ERR(freeTwccReceiverManager(&pKvsPeerConnection->pTwccReceiverManager));
    }

    if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->twccReceiverLock)) {
        MUTEX_FREE(pKvsPeerConnection->twccReceiverLock);
        pKvsPeerConnection->twccReceiverLock = INVALID_MUTEX_VALUE;
    }

    // Free PCAP dump
    if (pKvsPeerConnection->pPcapDump != NULL) {
        pcapDumpFree(&pKvsPeerConnection->pPcapDump);
    }

    PROFILE_WITH_START_TIME_OBJ(startTime, pKvsPeerConnection->peerConnectionDiagnostics.freePeerConnectionTime, "Free peer connection");
    SAFE_MEMFREE(*ppPeerConnection);

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS peerConnectionOnIceCandidate(PRtcPeerConnection pRtcPeerConnection, UINT64 customData, RtcOnIceCandidate rtcOnIceCandidate)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL && rtcOnIceCandidate != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onIceCandidate = rtcOnIceCandidate;
    pKvsPeerConnection->onIceCandidateCustomData = customData;

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionOnDataChannel(PRtcPeerConnection pRtcPeerConnection, UINT64 customData, RtcOnDataChannel rtcOnDataChannel)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL && rtcOnDataChannel != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onDataChannel = rtcOnDataChannel;
    pKvsPeerConnection->onDataChannelCustomData = customData;

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionOnConnectionStateChange(PRtcPeerConnection pRtcPeerConnection, UINT64 customData,
                                             RtcOnConnectionStateChange rtcOnConnectionStateChange)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL && rtcOnConnectionStateChange != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onConnectionStateChange = rtcOnConnectionStateChange;
    pKvsPeerConnection->onConnectionStateChangeCustomData = customData;

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionOnSenderBandwidthEstimation(PRtcPeerConnection pRtcPeerConnection, UINT64 customData,
                                                 RtcOnSenderBandwidthEstimation rtcOnSenderBandwidthEstimation)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL && rtcOnSenderBandwidthEstimation != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onSenderBandwidthEstimation = rtcOnSenderBandwidthEstimation;
    pKvsPeerConnection->onSenderBandwidthEstimationCustomData = customData;

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionOnTwccPacketReport(PRtcPeerConnection pRtcPeerConnection, UINT64 customData, RtcOnTwccPacketReport rtcOnTwccPacketReport)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onTwccPacketReport = rtcOnTwccPacketReport;
    pKvsPeerConnection->onTwccPacketReportCustomData = customData;

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionEnablePacing(PRtcPeerConnection pRtcPeerConnection, PRtcPacerConfig pConfig)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;
    PacerConfig pacerConfig;
    PPacer pPacer = NULL;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    // Don't create if already exists
    CHK(pKvsPeerConnection->pPacer == NULL, retStatus);

    // Convert public config to internal config
    MEMSET(&pacerConfig, 0, SIZEOF(PacerConfig));
    if (pConfig != NULL) {
        pacerConfig.initialBitrateBps = pConfig->initialBitrateBps;
        pacerConfig.maxQueueSize = pConfig->maxQueueSize;
        pacerConfig.maxQueueBytes = pConfig->maxQueueBytes;
        pacerConfig.pacingFactor = pConfig->pacingFactor;
        pacerConfig.maxQueueTimeKvs = pConfig->maxQueueTimeKvs;
    }
    pacerConfig.enabled = TRUE;

    // Create the pacer
    CHK_STATUS(createPacer(&pKvsPeerConnection->pPacer, pKvsPeerConnection->timerQueueHandle, &pacerConfig));
    pPacer = pKvsPeerConnection->pPacer;

    // Drop the peer connection lock before starting the pacer timer to avoid
    // lock-order-inversion: this path acquires peerConnectionObjLock then
    // timer queue lock, while timer callbacks (e.g. onNewIceLocalCandidate)
    // acquire timer queue lock then peerConnectionObjLock.
    MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = FALSE;

    // Start the pacer without holding peerConnectionObjLock
    CHK_STATUS(pacerStart(pPacer, pKvsPeerConnection));

    DLOGI("Pacing enabled for peer connection");

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionSetPacerBitrate(PRtcPeerConnection pRtcPeerConnection, UINT64 bitrateBps)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    CHK(pKvsPeerConnection->pPacer != NULL, STATUS_INVALID_OPERATION);

    CHK_STATUS(pacerSetTargetBitrate(pKvsPeerConnection->pPacer, bitrateBps));

CleanUp:
    LEAVES();
    return retStatus;
}

UINT64 peerConnectionGetPacerBitrate(PRtcPeerConnection pRtcPeerConnection)
{
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    if (pKvsPeerConnection == NULL || pKvsPeerConnection->pPacer == NULL) {
        return 0;
    }

    return pacerGetTargetBitrate(pKvsPeerConnection->pPacer);
}

STATUS peerConnectionSetPacerMaxQueueTime(PRtcPeerConnection pRtcPeerConnection, UINT64 maxQueueTimeKvs)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    CHK(pKvsPeerConnection->pPacer != NULL, STATUS_INVALID_OPERATION);

    CHK_STATUS(pacerSetMaxQueueTime(pKvsPeerConnection->pPacer, maxQueueTimeKvs));

CleanUp:
    LEAVES();
    return retStatus;
}

UINT64 peerConnectionGetPacerMaxQueueTime(PRtcPeerConnection pRtcPeerConnection)
{
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    if (pKvsPeerConnection == NULL || pKvsPeerConnection->pPacer == NULL) {
        return 0;
    }

    return pacerGetMaxQueueTime(pKvsPeerConnection->pPacer);
}

STATUS peerConnectionGetLocalDescription(PRtcPeerConnection pRtcPeerConnection, PRtcSessionDescriptionInit pRtcSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSessionDescription pSessionDescription = NULL;
    UINT32 serializeLen = 0;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    CHK(pRtcPeerConnection != NULL && pRtcSessionDescriptionInit != NULL, STATUS_NULL_ARG);

    CHK(NULL != (pSessionDescription = (PSessionDescription) MEMCALLOC(1, SIZEOF(SessionDescription))), STATUS_NOT_ENOUGH_MEMORY);

    if (pKvsPeerConnection->isOffer) {
        pRtcSessionDescriptionInit->type = SDP_TYPE_OFFER;
    } else {
        pRtcSessionDescriptionInit->type = SDP_TYPE_ANSWER;
    }

    CHK_STATUS(populateSessionDescription(pKvsPeerConnection, pKvsPeerConnection->pRemoteSessionDescription, pSessionDescription));
    CHK_STATUS(serializeSessionDescription(pSessionDescription, NULL, &serializeLen));
    CHK(serializeLen <= MAX_SESSION_DESCRIPTION_INIT_SDP_LEN, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(serializeSessionDescription(pSessionDescription, pRtcSessionDescriptionInit->sdp, &serializeLen));

CleanUp:
    SAFE_MEMFREE(pSessionDescription);
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionGetCurrentLocalDescription(PRtcPeerConnection pRtcPeerConnection, PRtcSessionDescriptionInit pRtcSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSessionDescription pSessionDescription = NULL;
    UINT32 serializeLen = 0;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    CHK(pRtcPeerConnection != NULL && pRtcSessionDescriptionInit != NULL, STATUS_NULL_ARG);
    // do nothing if remote session description hasn't been received
    CHK(pKvsPeerConnection->pRemoteSessionDescription != NULL, STATUS_SUCCESS);

    pSessionDescription = (PSessionDescription) MEMCALLOC(1, SIZEOF(SessionDescription));
    CHK(pSessionDescription != NULL, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(populateSessionDescription(pKvsPeerConnection, pKvsPeerConnection->pRemoteSessionDescription, pSessionDescription));

    CHK_STATUS(serializeSessionDescription(pSessionDescription, NULL, &serializeLen));
    CHK(serializeLen <= MAX_SESSION_DESCRIPTION_INIT_SDP_LEN, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(serializeSessionDescription(pSessionDescription, pRtcSessionDescriptionInit->sdp, &serializeLen));

CleanUp:
    CHK_LOG_ERR(retStatus);

    SAFE_MEMFREE(pSessionDescription);

    LEAVES();
    return retStatus;
}

/**
 *  @brief parses string of form "$number $whatever" returns $number as uint32
 *  @return 0 if value is not parsable or null
 */
UINT32 parseExtId(PCHAR extmapValue)
{
    ENTERS();
    UINT32 extid = 0;
    if (extmapValue == NULL || STRCHR(extmapValue, ' ') == NULL) {
        LEAVES();
        return 0;
    }
    if (STATUS_FAILED(STRTOUI32(extmapValue, STRCHR(extmapValue, ' '), 10, &extid))) {
        LEAVES();
        return 0;
    }

    LEAVES();
    return extid;
}

STATUS setRemoteDescription(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR remoteIceUfrag = NULL, remoteIcePwd = NULL;
    UINT32 i, j;
    PSessionDescription pSessionDescription;

    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pPeerConnection != NULL && pSessionDescriptionInit != NULL, STATUS_NULL_ARG);

    // In master mode, this should be freed once `createAnswer` is invoked for the session.
    // In viewer mode, this should be freed once `setRemoteDescription` is completed for the session.
    pKvsPeerConnection->pRemoteSessionDescription = (PSessionDescription) MEMCALLOC(1, SIZEOF(SessionDescription));
    pSessionDescription = pKvsPeerConnection->pRemoteSessionDescription;
    CHK(pSessionDescription != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pKvsPeerConnection->dtlsIsServer = FALSE;
    /* Assume cant trickle at first */
    NULLABLE_SET_VALUE(pKvsPeerConnection->canTrickleIce, FALSE);

    CHK_STATUS(deserializeSessionDescription(pSessionDescription, pSessionDescriptionInit->sdp));

    for (i = 0; i < pSessionDescription->sessionAttributesCount; i++) {
        if (STRCMP(pSessionDescription->sdpAttributes[i].attributeName, "fingerprint") == 0) {
            PCHAR attrValue = pSessionDescription->sdpAttributes[i].attributeValue;
            PCHAR space = STRCHR(attrValue, ' ');
            if (space != NULL && (space - attrValue) == 7 && STRNCMP(attrValue, "sha-256", 7) == 0) {
                STRNCPY(pKvsPeerConnection->remoteCertificateFingerprint, space + 1, CERTIFICATE_FINGERPRINT_LENGTH);
            } else {
                DLOGW("Skipping session-level fingerprint with unsupported hash algorithm: %.*s",
                      space != NULL ? (INT32) (space - attrValue) : (INT32) STRLEN(attrValue), attrValue);
            }
        } else if (pKvsPeerConnection->isOffer && STRCMP(pSessionDescription->sdpAttributes[i].attributeName, "setup") == 0) {
            // possible values are actpass, passive and active. If the incoming SDP has active, it indicates it is taking up a client role
            // In case of actpass and passive, the other peer is taking up a server role and is waiting for incoming connection
            // Reference: https://www.rfc-editor.org/rfc/rfc4572#section-6.2
            pKvsPeerConnection->dtlsIsServer = STRCMP(pSessionDescription->sdpAttributes[i].attributeValue, "active") == 0;
        } else if (STRCMP(pSessionDescription->sdpAttributes[i].attributeName, "ice-options") == 0 &&
                   STRSTR(pSessionDescription->sdpAttributes[i].attributeValue, "trickle") != NULL) {
            NULLABLE_SET_VALUE(pKvsPeerConnection->canTrickleIce, TRUE);
        }
    }

    for (i = 0; i < pSessionDescription->mediaCount; i++) {
#ifdef ENABLE_DATA_CHANNEL
        if (STRNCMP(pSessionDescription->mediaDescriptions[i].mediaName, "application", SIZEOF("application") - 1) == 0) {
            if (!pKvsPeerConnection->isOffer && !ATOMIC_LOAD_BOOL(&pKvsPeerConnection->sctpIsEnabled)) {
                ATOMIC_STORE_BOOL(&pKvsPeerConnection->sctpIsEnabled, TRUE);
            }
        }
#endif

        for (j = 0; j < pSessionDescription->mediaDescriptions[i].mediaAttributesCount; j++) {
            if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "ice-ufrag") == 0) {
                remoteIceUfrag = pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue;
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "ice-pwd") == 0) {
                remoteIcePwd = pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue;
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "candidate") == 0) {
                // Ignore the return value, we have candidates we don't support yet like TURN
                iceAgentAddRemoteCandidate(pKvsPeerConnection->pIceAgent, pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue);
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "fingerprint") == 0) {
                PCHAR attrValue = pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue;
                PCHAR space = STRCHR(attrValue, ' ');
                if (space != NULL && (space - attrValue) == 7 && STRNCMP(attrValue, "sha-256", 7) == 0) {
                    STRNCPY(pKvsPeerConnection->remoteCertificateFingerprint, space + 1, CERTIFICATE_FINGERPRINT_LENGTH);
                } else {
                    DLOGW("Skipping media-level fingerprint with unsupported hash algorithm: %.*s",
                          space != NULL ? (INT32) (space - attrValue) : (INT32) STRLEN(attrValue), attrValue);
                }
            } else if (pKvsPeerConnection->isOffer &&
                       STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "setup") == 0) {
                pKvsPeerConnection->dtlsIsServer = STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue, "active") == 0;
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "ice-options") == 0 &&
                       STRSTR(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue, "trickle") != NULL) {
                NULLABLE_SET_VALUE(pKvsPeerConnection->canTrickleIce, TRUE);
                // This code is only here because Chrome does NOT adhere to the standard and adds ice-options as a media level attribute
                // The standard dictates clearly that it should be a session level attribute:  https://tools.ietf.org/html/rfc5245#page-76
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "extmap") == 0 &&
                       STRSTR(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue, TWCC_EXT_URL) != NULL) {
                pKvsPeerConnection->twccExtId = parseExtId(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue);
            }
        }
    }

    CHK(remoteIceUfrag != NULL && remoteIcePwd != NULL, STATUS_SESSION_DESCRIPTION_MISSING_ICE_VALUES);
    if (pKvsPeerConnection->remoteCertificateFingerprint[0] == '\0') {
        DLOGE("No sha-256 fingerprint found in remote SDP. Only sha-256 is supported.");
    }
    CHK(pKvsPeerConnection->remoteCertificateFingerprint[0] != '\0', STATUS_SESSION_DESCRIPTION_MISSING_CERTIFICATE_FINGERPRINT);

    if (!IS_EMPTY_STRING(pKvsPeerConnection->remoteIceUfrag) && !IS_EMPTY_STRING(pKvsPeerConnection->remoteIcePwd) &&
        STRNCMP(pKvsPeerConnection->remoteIceUfrag, remoteIceUfrag, MAX_ICE_UFRAG_LEN) != 0 &&
        STRNCMP(pKvsPeerConnection->remoteIcePwd, remoteIcePwd, MAX_ICE_PWD_LEN) != 0) {
        CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIceUfrag, LOCAL_ICE_UFRAG_LEN));
        CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIcePwd, LOCAL_ICE_PWD_LEN));
        CHK_STATUS(iceAgentRestart(pKvsPeerConnection->pIceAgent, pKvsPeerConnection->localIceUfrag, pKvsPeerConnection->localIcePwd));
        // This starts the gathering process timer callback that periodically checks for local candidate list
        CHK_STATUS(iceAgentStartGathering(pKvsPeerConnection->pIceAgent));
    }

    STRNCPY(pKvsPeerConnection->remoteIceUfrag, remoteIceUfrag, MAX_ICE_UFRAG_LEN);
    STRNCPY(pKvsPeerConnection->remoteIcePwd, remoteIcePwd, MAX_ICE_PWD_LEN);

    // This starts the state machine timer callback that transitions states periodically
    CHK_STATUS(iceAgentStartAgent(pKvsPeerConnection->pIceAgent, pKvsPeerConnection->remoteIceUfrag, pKvsPeerConnection->remoteIcePwd,
                                  pKvsPeerConnection->isOffer));

    if (!pKvsPeerConnection->isOffer) {
        CHK_STATUS(setPayloadTypesFromOffer(pKvsPeerConnection->pCodecTable, pKvsPeerConnection->pRtxTable, pSessionDescription));
    }
    CHK_STATUS(setTransceiverPayloadTypes(pKvsPeerConnection->pCodecTable, pKvsPeerConnection->pRtxTable, pKvsPeerConnection->pTransceivers));
    CHK_STATUS(setReceiversSsrc(pSessionDescription, pKvsPeerConnection->pTransceivers));

    if (NULL != GETENV(DEBUG_LOG_SDP)) {
        DLOGD("REMOTE_SDP:%s\n", pSessionDescriptionInit->sdp);
    }

    // Start TWCC feedback timer when remote offers TWCC
    if (pKvsPeerConnection->twccExtId != 0 && pKvsPeerConnection->twccFeedbackTimerId == MAX_UINT32) {
        CHK_STATUS(timerQueueAddTimer(pKvsPeerConnection->timerQueueHandle, TWCC_FEEDBACK_INITIAL_DELAY, TIMER_QUEUE_SINGLE_INVOCATION_PERIOD,
                                      twccFeedbackCallback, (UINT64) pKvsPeerConnection, &pKvsPeerConnection->twccFeedbackTimerId));
        DLOGI("Started TWCC feedback timer with extension ID %u", pKvsPeerConnection->twccExtId);
    }

CleanUp:
    if (pKvsPeerConnection != NULL && pKvsPeerConnection->isOffer) {
        SAFE_MEMFREE(pKvsPeerConnection->pRemoteSessionDescription);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS createOffer(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSessionDescription pSessionDescription = NULL;
    UINT32 serializeLen = 0;

    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pSessionDescriptionInit != NULL, STATUS_NULL_ARG);

    // SessionDescription is large enough structure to not define on the stack and use heap memory
    pSessionDescription = (PSessionDescription) MEMCALLOC(1, SIZEOF(SessionDescription));
    CHK(pSessionDescription != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pSessionDescriptionInit->type = SDP_TYPE_OFFER;
    pKvsPeerConnection->isOffer = TRUE;
    if (pSessionDescriptionInit->useTrickleIce) {
        NULLABLE_SET_VALUE(pKvsPeerConnection->canTrickleIce, TRUE);
    }

#ifdef ENABLE_DATA_CHANNEL
    ATOMIC_STORE_BOOL(&pKvsPeerConnection->sctpIsEnabled, TRUE);
#endif

    CHK_STATUS(setPayloadTypesForOffer(pKvsPeerConnection->pCodecTable));
    CHK_STATUS(populateSessionDescription(pKvsPeerConnection, pKvsPeerConnection->pRemoteSessionDescription, pSessionDescription));
    CHK_STATUS(serializeSessionDescription(pSessionDescription, NULL, &serializeLen));
    CHK(serializeLen <= MAX_SESSION_DESCRIPTION_INIT_SDP_LEN, STATUS_NOT_ENOUGH_MEMORY);
    CHK_STATUS(serializeSessionDescription(pSessionDescription, pSessionDescriptionInit->sdp, &serializeLen));
    // If embedded SDK acts as the viewer
    if (NULL != GETENV(DEBUG_LOG_SDP)) {
        DLOGD("LOCAL_SDP:%s", pSessionDescriptionInit->sdp);
    }
CleanUp:
    SAFE_MEMFREE(pSessionDescription);
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS createAnswer(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pSessionDescriptionInit != NULL, STATUS_NULL_ARG);
    CHK(pKvsPeerConnection->pRemoteSessionDescription != NULL, STATUS_PEERCONNECTION_CREATE_ANSWER_WITHOUT_REMOTE_DESCRIPTION);

    pSessionDescriptionInit->type = SDP_TYPE_ANSWER;
    pKvsPeerConnection->isOffer = FALSE;

    CHK_STATUS(peerConnectionGetCurrentLocalDescription(pPeerConnection, pSessionDescriptionInit));
    // If embedded SDK acts as the master
    if (NULL != GETENV(DEBUG_LOG_SDP)) {
        DLOGD("LOCAL_SDP:%s", pSessionDescriptionInit->sdp);
    }

    // Once answer is created, remote SDP is not needed anymore. We also clear only if
    // answer SDP is successfully created
    SAFE_MEMFREE(pKvsPeerConnection->pRemoteSessionDescription);
CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS setLocalDescription(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pSessionDescriptionInit != NULL, STATUS_NULL_ARG);

    CHK_STATUS(iceAgentStartGathering(pKvsPeerConnection->pIceAgent));
CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS configureTransceiverRollingBuffer(PRtcRtpTransceiver pRtcRtpTransceiver, PRtcMediaStreamTrack pRtcMediaStreamTrack,
                                         DOUBLE rollingBufferDurationSec, DOUBLE rollingBufferBitratebps)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;
    CHK_WARN(pKvsRtpTransceiver != NULL || pRtcMediaStreamTrack != NULL, STATUS_NULL_ARG,
             "Transceiver is not created. This needs to be invoked after addTransceiver is invoked");

    CHK_STATUS(setUpRollingBufferConfigInternal(pKvsRtpTransceiver, pRtcMediaStreamTrack, rollingBufferDurationSec, rollingBufferBitratebps));
CleanUp:
    return retStatus;
}

STATUS addTransceiver(PRtcPeerConnection pPeerConnection, PRtcMediaStreamTrack pRtcMediaStreamTrack, PRtcRtpTransceiverInit pRtcRtpTransceiverInit,
                      PRtcRtpTransceiver* ppRtcRtpTransceiver)
{
    UNUSED_PARAM(pRtcRtpTransceiverInit);
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = NULL;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    PJitterBuffer pJitterBuffer = NULL;
    DepayRtpPayloadFunc depayFunc;
    UINT32 clockRate = 0;
    UINT32 ssrc = (UINT32) RAND(), rtxSsrc = (UINT32) RAND();
    RTC_RTP_TRANSCEIVER_DIRECTION direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
    RtcMediaStreamTrack videoTrack;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    if (pRtcRtpTransceiverInit != NULL) {
        direction = pRtcRtpTransceiverInit->direction;
    }

    if (direction == RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY && pRtcMediaStreamTrack == NULL) {
        MEMSET(&videoTrack, 0x00, SIZEOF(RtcMediaStreamTrack));
        videoTrack.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
        videoTrack.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
        STRCPY(videoTrack.streamId, "myKvsVideoStream");
        STRCPY(videoTrack.trackId, "myVideoTrack");
        pRtcMediaStreamTrack = &videoTrack;
        // rollingBufferDurationSec will be DEFAULT_ROLLING_BUFFER_DURATION_IN_SECONDS
        // rollingBufferBitratebps will be DEFAULT_EXPECTED_VIDEO_BIT_RATE
    }

    switch (pRtcMediaStreamTrack->codec) {
        case RTC_CODEC_OPUS:
            depayFunc = depayOpusFromRtpPayload;
            clockRate = OPUS_CLOCKRATE;
            break;

        case RTC_CODEC_MULAW:
        case RTC_CODEC_ALAW:
            depayFunc = depayG711FromRtpPayload;
            clockRate = PCM_CLOCKRATE;
            break;

        case RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE:
            depayFunc = depayH264FromRtpPayload;
            clockRate = VIDEO_CLOCKRATE;
            break;

        case RTC_CODEC_VP8:
            depayFunc = depayVP8FromRtpPayload;
            clockRate = VIDEO_CLOCKRATE;
            break;
        case RTC_CODEC_H265:
            depayFunc = depayH265FromRtpPayload;
            clockRate = VIDEO_CLOCKRATE;
            break;

        default:
            DLOGW("[TrackID: %s, StreamID: %s, kind: %d] contains unsupported codec: %d", pRtcMediaStreamTrack->trackId,
                  pRtcMediaStreamTrack->streamId, pRtcMediaStreamTrack->kind, pRtcMediaStreamTrack->codec);
            CHK(FALSE, STATUS_NOT_IMPLEMENTED);
    }

    // TODO: Add ssrc duplicate detection here not only relying on RAND()
    CHK_STATUS(createKvsRtpTransceiver(direction, pKvsPeerConnection, ssrc, rtxSsrc, pRtcMediaStreamTrack, NULL, pRtcMediaStreamTrack->codec,
                                       &pKvsRtpTransceiver));
    // Audio codecs (Opus per RFC 7587, G.711) never fragment frames across RTP packets
    BOOL alwaysSinglePacketFrames = (pRtcMediaStreamTrack->codec == RTC_CODEC_OPUS || pRtcMediaStreamTrack->codec == RTC_CODEC_MULAW ||
                                     pRtcMediaStreamTrack->codec == RTC_CODEC_ALAW);
    if (pKvsPeerConnection->useRealTimeJitterBuffer) {
        CHK_STATUS(createRealTimeJitterBuffer(onFrameReadyFunc, onFrameDroppedFunc, depayFunc, pKvsPeerConnection->jitterBufferMaxLatency, clockRate,
                                              (UINT64) pKvsRtpTransceiver, alwaysSinglePacketFrames, &pJitterBuffer));
    } else {
        CHK_STATUS(createJitterBuffer(onFrameReadyFunc, onFrameDroppedFunc, depayFunc, pKvsPeerConnection->jitterBufferMaxLatency, clockRate,
                                      (UINT64) pKvsRtpTransceiver, alwaysSinglePacketFrames, &pJitterBuffer));
    }
    CHK_STATUS(kvsRtpTransceiverSetJitterBuffer(pKvsRtpTransceiver, pJitterBuffer));

    // after pKvsRtpTransceiver is successfully created, jitterBuffer will be freed by pKvsRtpTransceiver.
    pJitterBuffer = NULL;

    CHK_STATUS(doubleListInsertItemHead(pKvsPeerConnection->pTransceivers, (UINT64) pKvsRtpTransceiver));
    *ppRtcRtpTransceiver = (PRtcRtpTransceiver) pKvsRtpTransceiver;

    CHK_STATUS(timerQueueAddTimer(pKvsPeerConnection->timerQueueHandle, RTCP_FIRST_REPORT_DELAY, TIMER_QUEUE_SINGLE_INVOCATION_PERIOD,
                                  rtcpReportsCallback, (UINT64) pKvsRtpTransceiver, &pKvsRtpTransceiver->rtcpReportsTimerId));

    pKvsRtpTransceiver = NULL;

CleanUp:
    if (pJitterBuffer != NULL) {
        CHK_LOG_ERR(freeJitterBuffer(&pJitterBuffer));
    }

    if (pKvsRtpTransceiver != NULL) {
        CHK_LOG_ERR(freeKvsRtpTransceiver(&pKvsRtpTransceiver));
    }

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS addSupportedCodec(PRtcPeerConnection pPeerConnection, RTC_CODEC rtcCodec)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    if (rtcCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
        CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, DEFAULT_PAYLOAD_H264));
    } else if (rtcCodec == RTC_CODEC_VP8) {
        CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, DEFAULT_PAYLOAD_VP8));
    } else if (rtcCodec == RTC_CODEC_H265) {
        CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, DEFAULT_PAYLOAD_H265));
    } else if (rtcCodec == RTC_CODEC_OPUS) {
        CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, DEFAULT_PAYLOAD_OPUS));
    } else if (rtcCodec == RTC_CODEC_MULAW) {
        CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, DEFAULT_PAYLOAD_MULAW));
    } else if (rtcCodec == RTC_CODEC_ALAW) {
        CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, DEFAULT_PAYLOAD_ALAW));
    } else {
        CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, 0));
    }
CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS addIceCandidate(PRtcPeerConnection pPeerConnection, PCHAR pIceCandidate)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pIceCandidate != NULL, STATUS_NULL_ARG);
    DLOGD("%s", pIceCandidate);
    iceAgentAddRemoteCandidate(pKvsPeerConnection->pIceAgent, pIceCandidate);

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS restartIce(PRtcPeerConnection pPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    /* generate new local uFrag and uPwd and clear out remote uFrag and uPwd */
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIceUfrag, LOCAL_ICE_UFRAG_LEN));
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIcePwd, LOCAL_ICE_PWD_LEN));
    pKvsPeerConnection->remoteIceUfrag[0] = '\0';
    pKvsPeerConnection->remoteIcePwd[0] = '\0';
    CHK_STATUS(iceAgentRestart(pKvsPeerConnection->pIceAgent, pKvsPeerConnection->localIceUfrag, pKvsPeerConnection->localIcePwd));

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS closePeerConnection(PRtcPeerConnection pPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    UINT64 startTime = GETTIME();
    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    CHK_LOG_ERR(dtlsSessionShutdown(pKvsPeerConnection->pDtlsSession));
    CHK_LOG_ERR(iceAgentShutdown(pKvsPeerConnection->pIceAgent));

    // Cancel TWCC feedback timer so it stops re-scheduling itself.
    // This is needed because on Android THREAD_CANCEL is a no-op, so
    // timerQueueShutdown cannot forcefully stop the executor thread.
    // Cancelling timers first lets the executor exit cleanly.
    // Snapshot and reset twccFeedbackTimerId under twccLock to avoid
    // racing with twccFeedbackCallback which writes it on the timer thread.
    if (IS_VALID_TIMER_QUEUE_HANDLE(pKvsPeerConnection->timerQueueHandle) && IS_VALID_MUTEX_VALUE(pKvsPeerConnection->twccLock)) {
        UINT32 twccTimerId;
        MUTEX_LOCK(pKvsPeerConnection->twccLock);
        twccTimerId = pKvsPeerConnection->twccFeedbackTimerId;
        pKvsPeerConnection->twccFeedbackTimerId = MAX_UINT32;
        MUTEX_UNLOCK(pKvsPeerConnection->twccLock);
        if (twccTimerId != MAX_UINT32) {
            timerQueueCancelTimer(pKvsPeerConnection->timerQueueHandle, twccTimerId, (UINT64) pKvsPeerConnection);
        }
    }

    // Stop pacer timer
    if (pKvsPeerConnection->pPacer != NULL) {
        pacerStop(pKvsPeerConnection->pPacer);
    }

    PROFILE_WITH_START_TIME_OBJ(startTime, pKvsPeerConnection->peerConnectionDiagnostics.closePeerConnectionTime, "Close peer connection");

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

NullableBool canTrickleIceCandidates(PRtcPeerConnection pPeerConnection)
{
    ENTERS();
    NullableBool canTrickle = {FALSE, FALSE};
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    if (pKvsPeerConnection != NULL) {
        canTrickle = pKvsPeerConnection->canTrickleIce;
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return canTrickle;
}

STATUS initKvsWebRtc(VOID)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHK(!ATOMIC_LOAD_BOOL(&gKvsWebRtcInitialized), retStatus);
    DLOGI("Initializing WebRTC library...");
    SRAND(GETTIME());

    CHK(srtp_init() == srtp_err_status_ok, STATUS_SRTP_INIT_FAILED);

    // init endianness handling
    initializeEndianness();

    KVS_CRYPTO_INIT();
    LOG_GIT_HASH();

    SET_INSTRUMENTED_ALLOCATORS();
#ifdef ENABLE_DATA_CHANNEL
    CHK_STATUS(initSctpSession());
#endif
#ifdef ENABLE_KVS_THREADPOOL
    DLOGI("KVS WebRtc library using thread pool");
    CHK_STATUS(createWebRtcClientInstance());
    CHK_STATUS(createThreadPoolContext());
    CHK_STATUS(threadpoolContextPush(resolveStunIceServerIp, NULL));
#endif
    ATOMIC_STORE_BOOL(&gKvsWebRtcInitialized, TRUE);

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS cleanupWebRtcClientInstance()
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    // Stun object cleanup
    PWebRtcClientContext pWebRtcClientContext = getWebRtcClientInstance();

    DLOGD("Releasing webrtc client context instance from cleanupWebRtcClientInstance");
    releaseHoldOnInstance(pWebRtcClientContext);

    CHK_WARN(ATOMIC_LOAD_BOOL(&pWebRtcClientContext->isContextInitialized), STATUS_INVALID_OPERATION,
             "WebRtc context not initialized, nothing to clean up");

    ATOMIC_STORE_BOOL(&pWebRtcClientContext->isContextInitialized, FALSE);

    while (ATOMIC_LOAD(&pWebRtcClientContext->contextRefCnt) > 0) {
        DLOGV("Waiting on all references to be returned...%d", pWebRtcClientContext->contextRefCnt);
        THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    /* Start of handling STUN object */
    // Need this check to ensure we do not clean up the object in the next
    // step while the resolve thread is ongoing
    CHK_WARN(pWebRtcClientContext->pStunIpAddrCtx != NULL, STATUS_NULL_ARG, "Destroying STUN object without setting up");
    MUTEX_LOCK(pWebRtcClientContext->stunCtxlock);
    SAFE_MEMFREE(pWebRtcClientContext->pStunIpAddrCtx);
    pWebRtcClientContext->pStunIpAddrCtx = NULL;
    DLOGI("Destroyed STUN IP object");
    MUTEX_UNLOCK(pWebRtcClientContext->stunCtxlock);
    /* End of handling STUN object */

    if (IS_VALID_MUTEX_VALUE(pWebRtcClientContext->stunCtxlock)) {
        MUTEX_FREE(pWebRtcClientContext->stunCtxlock);
        pWebRtcClientContext->stunCtxlock = INVALID_MUTEX_VALUE;
    }

    DLOGI("Destroyed WebRtc client context");

CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS deinitKvsWebRtc(VOID)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHK(ATOMIC_LOAD_BOOL(&gKvsWebRtcInitialized), retStatus);

#ifdef ENABLE_DATA_CHANNEL
    deinitSctpSession();
#endif

    srtp_shutdown();

#ifdef ENABLE_KVS_THREADPOOL
    cleanupWebRtcClientInstance();
    destroyThreadPoolContext();
    DLOGI("Destroyed threadpool");
    RESET_INSTRUMENTED_ALLOCATORS();
#endif
    ATOMIC_STORE_BOOL(&gKvsWebRtcInitialized, FALSE);
CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

// Not thread safe. Ensure this function is invoked in a guarded section
static STATUS twccRollingWindowDeletion(PKvsPeerConnection pKvsPeerConnection, PRtpPacket pRtpPacket, UINT16 endingSeqNum)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 updatedSeqNum = 0;
    PTwccRtpPacketInfo tempTwccRtpPktInfo = NULL;
    UINT64 ageOfOldest = 0, firstRtpTime = 0;
    UINT64 twccPacketValue = 0;
    BOOL isCheckComplete = FALSE;

    CHK(pKvsPeerConnection != NULL && pRtpPacket != NULL && pKvsPeerConnection->pTwccManager != NULL, STATUS_NULL_ARG);

    updatedSeqNum = pKvsPeerConnection->pTwccManager->firstSeqNumInRollingWindow;
    do {
        // If the seqNum is not present in the hash table, it is ok. We move on to the next
        if (STATUS_SUCCEEDED(hashTableGet(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable, updatedSeqNum, &twccPacketValue))) {
            tempTwccRtpPktInfo = (PTwccRtpPacketInfo) twccPacketValue;
            if (tempTwccRtpPktInfo != NULL) {
                firstRtpTime = tempTwccRtpPktInfo->localTimeKvs;
                // Would be the case if the timestamps are not monotonically increasing.
                if (pRtpPacket->sentTime >= firstRtpTime) {
                    ageOfOldest = pRtpPacket->sentTime - firstRtpTime;
                    if (ageOfOldest > TWCC_ESTIMATOR_TIME_WINDOW) {
                        // If the seqNum is not present in the hash table, move on. However, this case should not happen
                        // given this function is holding the lock and tempTwccRtpPktInfo is populated because it exists
                        if (STATUS_SUCCEEDED(hashTableRemove(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable, updatedSeqNum))) {
                            SAFE_MEMFREE(tempTwccRtpPktInfo);
                        }
                        updatedSeqNum++;
                    } else {
                        isCheckComplete = TRUE;
                    }
                } else {
                    // Move to the next seqNum to check if we can remove the next one atleast
                    DLOGV("Non-monotonic timestamp detected for RTP packet seqNum %d [ts: %" PRIu64 ". Current RTP packets' ts: %" PRIu64,
                          updatedSeqNum, firstRtpTime, pRtpPacket->sentTime);
                    updatedSeqNum++;
                }
            } else {
                CHK_STATUS(hashTableRemove(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable, updatedSeqNum));
                updatedSeqNum++;
            }
        } else {
            updatedSeqNum++;
        }
        // reset before next iteration
        tempTwccRtpPktInfo = NULL;
    } while (!isCheckComplete && updatedSeqNum != (UINT16) (endingSeqNum + 1));

    // Update regardless. The loop checks until current RTP packets seq number irrespective of the failure
    pKvsPeerConnection->pTwccManager->firstSeqNumInRollingWindow = updatedSeqNum;
CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS twccManagerOnPacketSent(PKvsPeerConnection pKvsPeerConnection, PRtpPacket pRtpPacket)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    UINT16 seqNum = 0;
    PTwccRtpPacketInfo pTwccRtpPktInfo = NULL;

    CHK(pKvsPeerConnection != NULL && pRtpPacket != NULL, STATUS_NULL_ARG);
    CHK(pKvsPeerConnection->onSenderBandwidthEstimation != NULL && pKvsPeerConnection->pTwccManager != NULL, STATUS_SUCCESS);
    CHK(TWCC_EXT_PROFILE == pRtpPacket->header.extensionProfile, STATUS_SUCCESS);

    MUTEX_LOCK(pKvsPeerConnection->twccLock);
    locked = TRUE;

    CHK((pTwccRtpPktInfo = MEMCALLOC(1, SIZEOF(TwccRtpPacketInfo))) != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pTwccRtpPktInfo->packetSize = pRtpPacket->payloadLength;
    pTwccRtpPktInfo->localTimeKvs = pRtpPacket->sentTime;
    pTwccRtpPktInfo->remoteTimeKvs = TWCC_PACKET_LOST_TIME;
    seqNum = TWCC_SEQNUM(pRtpPacket->header.extensionPayload);
    CHK_STATUS(hashTableUpsert(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable, seqNum, (UINT64) pTwccRtpPktInfo));

    // Ensure twccRollingWindowDeletion is run in a guarded section
    CHK_STATUS(twccRollingWindowDeletion(pKvsPeerConnection, pRtpPacket, seqNum));
CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->twccLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS twccManagerOnPacedPacketSent(PKvsPeerConnection pKvsPeerConnection, UINT16 twccSeqNum, UINT32 packetSize, UINT64 sentTimeKvs)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PTwccRtpPacketInfo pTwccRtpPktInfo = NULL;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    CHK((pKvsPeerConnection->onSenderBandwidthEstimation != NULL || pKvsPeerConnection->onTwccPacketReport != NULL) &&
            pKvsPeerConnection->pTwccManager != NULL,
        STATUS_SUCCESS);

    MUTEX_LOCK(pKvsPeerConnection->twccLock);
    locked = TRUE;

    CHK((pTwccRtpPktInfo = MEMCALLOC(1, SIZEOF(TwccRtpPacketInfo))) != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pTwccRtpPktInfo->packetSize = packetSize;
    pTwccRtpPktInfo->localTimeKvs = sentTimeKvs;
    pTwccRtpPktInfo->remoteTimeKvs = TWCC_PACKET_LOST_TIME;
    CHK_STATUS(hashTableUpsert(pKvsPeerConnection->pTwccManager->pTwccRtpPktInfosHashTable, twccSeqNum, (UINT64) pTwccRtpPktInfo));

    // Note: We skip twccRollingWindowDeletion here since we don't have the full RtpPacket
    // The rolling window will be cleaned up during normal TWCC feedback processing

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->twccLock);
    }
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS peerConnectionGetMetrics(PRtcPeerConnection pPeerConnection, PPeerConnectionMetrics pPeerConnectionMetrics)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    PWebRtcClientContext pWebRtcClientContext = getWebRtcClientInstance();

    CHK(pKvsPeerConnection != NULL && pPeerConnectionMetrics != NULL, STATUS_NULL_ARG);
    if (pPeerConnectionMetrics->version > PEER_CONNECTION_METRICS_CURRENT_VERSION) {
        DLOGW("Peer connection metrics object version invalid..setting to highest default version %d", PEER_CONNECTION_METRICS_CURRENT_VERSION);
        pPeerConnectionMetrics->version = PEER_CONNECTION_METRICS_CURRENT_VERSION;
    }
#ifdef ENABLE_KVS_THREADPOOL
    MUTEX_LOCK(pWebRtcClientContext->stunCtxlock);
    if (pWebRtcClientContext->isContextInitialized) {
        if (pWebRtcClientContext->pStunIpAddrCtx->isIpInitialized) {
            pPeerConnectionMetrics->peerConnectionStats.stunDnsResolutionTime = pWebRtcClientContext->pStunIpAddrCtx->stunDnsResolutionTime;
        }
    }
    MUTEX_UNLOCK(pWebRtcClientContext->stunCtxlock);
#endif

    pPeerConnectionMetrics->peerConnectionStats.peerConnectionCreationTime = pKvsPeerConnection->peerConnectionDiagnostics.peerConnectionCreationTime;
    pPeerConnectionMetrics->peerConnectionStats.dtlsSessionSetupTime = pKvsPeerConnection->peerConnectionDiagnostics.dtlsSessionSetupTime;
    pPeerConnectionMetrics->peerConnectionStats.iceHolePunchingTime = pKvsPeerConnection->peerConnectionDiagnostics.iceHolePunchingTime;
    // Cannot record these 2 in here because peer connection object would become NULL after clearing. Need another strategy
    pPeerConnectionMetrics->peerConnectionStats.closePeerConnectionTime = pKvsPeerConnection->peerConnectionDiagnostics.closePeerConnectionTime;
    pPeerConnectionMetrics->peerConnectionStats.freePeerConnectionTime = pKvsPeerConnection->peerConnectionDiagnostics.freePeerConnectionTime;
CleanUp:
    releaseHoldOnInstance(pWebRtcClientContext);
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS iceAgentGetMetrics(PRtcPeerConnection pPeerConnection, PKvsIceAgentMetrics pKvsIceAgentMetrics)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    CHK(pKvsPeerConnection != NULL && pKvsIceAgentMetrics != NULL, STATUS_NULL_ARG);

    if (pKvsIceAgentMetrics->version > ICE_AGENT_METRICS_CURRENT_VERSION) {
        DLOGW("ICE agent metrics object version invalid..setting to highest default version %d", PEER_CONNECTION_METRICS_CURRENT_VERSION);
        pKvsIceAgentMetrics->version = ICE_AGENT_METRICS_CURRENT_VERSION;
    }
    CHK_STATUS(getIceAgentStats(pKvsPeerConnection->pIceAgent, pKvsIceAgentMetrics));
CleanUp:
    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}
