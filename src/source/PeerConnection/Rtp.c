#define LOG_CLASS "RtcRtp"

#include "../Include_i.h"

typedef STATUS (*RtpPayloadFunc)(UINT32, PBYTE, UINT32, PBYTE, PUINT32, PUINT32, PUINT32);

STATUS createKvsRtpTransceiver(RTC_RTP_TRANSCEIVER_DIRECTION direction, PKvsPeerConnection pKvsPeerConnection, UINT32 ssrc, UINT32 rtxSsrc,
                               PRtcMediaStreamTrack pRtcMediaStreamTrack, PJitterBuffer pJitterBuffer, RTC_CODEC rtcCodec,
                               PKvsRtpTransceiver* ppKvsRtpTransceiver)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = NULL;

    CHK(ppKvsRtpTransceiver != NULL && pKvsPeerConnection != NULL && pRtcMediaStreamTrack != NULL, STATUS_NULL_ARG);

    pKvsRtpTransceiver = (PKvsRtpTransceiver) MEMCALLOC(1, SIZEOF(KvsRtpTransceiver));
    CHK(pKvsRtpTransceiver != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pKvsRtpTransceiver->peerFrameBufferSize = DEFAULT_PEER_FRAME_BUFFER_SIZE;
    pKvsRtpTransceiver->peerFrameBuffer = (PBYTE) MEMALLOC(pKvsRtpTransceiver->peerFrameBufferSize);
    CHK(pKvsRtpTransceiver->peerFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);
    pKvsRtpTransceiver->pKvsPeerConnection = pKvsPeerConnection;
    pKvsRtpTransceiver->statsLock = MUTEX_CREATE(FALSE);
    pKvsRtpTransceiver->sender.ssrc = ssrc;
    pKvsRtpTransceiver->sender.rtxSsrc = rtxSsrc;
    pKvsRtpTransceiver->sender.track = *pRtcMediaStreamTrack;
    pKvsRtpTransceiver->sender.packetBuffer = NULL;
    pKvsRtpTransceiver->sender.retransmitter = NULL;
    pKvsRtpTransceiver->pJitterBuffer = pJitterBuffer;
    pKvsRtpTransceiver->transceiver.receiver.track.codec = rtcCodec;
    pKvsRtpTransceiver->transceiver.receiver.track.kind = pRtcMediaStreamTrack->kind;
    pKvsRtpTransceiver->transceiver.direction = direction;

    pKvsRtpTransceiver->outboundStats.sent.rtpStream.ssrc = ssrc;
    STRNCPY(pKvsRtpTransceiver->outboundStats.sent.rtpStream.kind, pRtcMediaStreamTrack->kind == MEDIA_STREAM_TRACK_KIND_AUDIO ? "audio" : "video",
            MAX_STATS_STRING_LENGTH);
    STRNCPY(pKvsRtpTransceiver->outboundStats.trackId, pRtcMediaStreamTrack->trackId, MAX_STATS_STRING_LENGTH);

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        freeKvsRtpTransceiver(&pKvsRtpTransceiver);
    }

    if (ppKvsRtpTransceiver != NULL) {
        *ppKvsRtpTransceiver = pKvsRtpTransceiver;
    }

    return retStatus;
}

STATUS setUpRollingBufferConfigInternal(PKvsRtpTransceiver pKvsRtpTransceiver, PRtcMediaStreamTrack pRtcMediaStreamTrack,
                                        DOUBLE rollingBufferDurationSec, DOUBLE rollingBufferBitratebps)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK_ERR(pKvsRtpTransceiver != NULL || pRtcMediaStreamTrack != NULL, STATUS_NULL_ARG,
            "Media track and transceiver not set. Make sure to set up transceiver with addTransceiver()");

    //  Do not attempt to alloc for a new RollingBufferConfig if one is still not freed.
    if (pKvsRtpTransceiver->pRollingBufferConfig == NULL) {
        pKvsRtpTransceiver->pRollingBufferConfig = (PRollingBufferConfig) MEMCALLOC(1, SIZEOF(RollingBufferConfig));
        CHK(pKvsRtpTransceiver->pRollingBufferConfig != NULL, STATUS_NOT_ENOUGH_MEMORY);
    }

    // Validate configured buffer duration is within acceptable range, else set to default duration.
    if (rollingBufferDurationSec >= MIN_ROLLING_BUFFER_DURATION_IN_SECONDS && rollingBufferDurationSec <= MAX_ROLLING_BUFFER_DURATION_IN_SECONDS) {
        DLOGI("Rolling buffer duration set to %lf seconds.", rollingBufferDurationSec);
        pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferDurationSec = rollingBufferDurationSec;
    } else if (rollingBufferDurationSec != 0) {
        DLOGW("Rolling buffer duration does not fit range (%lf sec - %lf sec). Setting to default %lf sec", MIN_ROLLING_BUFFER_DURATION_IN_SECONDS,
              MAX_ROLLING_BUFFER_DURATION_IN_SECONDS, DEFAULT_ROLLING_BUFFER_DURATION_IN_SECONDS);
        pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferDurationSec = DEFAULT_ROLLING_BUFFER_DURATION_IN_SECONDS;
    } else if (rollingBufferDurationSec == 0) {
        DLOGI("Setting to default buffer duration of %lf sec", DEFAULT_ROLLING_BUFFER_DURATION_IN_SECONDS);
        pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferDurationSec = DEFAULT_ROLLING_BUFFER_DURATION_IN_SECONDS;
    }

    // Validate configured expected bitrate is within acceptable range, else set to default bitrate.
    if (rollingBufferBitratebps >= MIN_EXPECTED_BIT_RATE && rollingBufferBitratebps <= MAX_EXPECTED_BIT_RATE) {
        if (pRtcMediaStreamTrack->kind == MEDIA_STREAM_TRACK_KIND_VIDEO) {
            DLOGI("Rolling buffer expected bitrate set to %lf bps for video.", rollingBufferBitratebps);
        } else if (pRtcMediaStreamTrack->kind == MEDIA_STREAM_TRACK_KIND_AUDIO) {
            DLOGI("Rolling buffer expected bitrate set to %lf bps for audio.", rollingBufferBitratebps);
        } else {
            DLOGI("Rolling buffer expected bitrate set to %lf bps for unkown codec.", rollingBufferBitratebps);
        }
        pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferBitratebps = rollingBufferBitratebps;
    } else if (rollingBufferBitratebps != 0) {
        if (pRtcMediaStreamTrack->kind == MEDIA_STREAM_TRACK_KIND_VIDEO) {
            DLOGW("Rolling buffer bitrate does not fit range (%lf bps - %lf bps) for video. Setting to default %lf bps.", MIN_EXPECTED_BIT_RATE,
                  MAX_EXPECTED_BIT_RATE, DEFAULT_EXPECTED_VIDEO_BIT_RATE);
            pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferBitratebps = DEFAULT_EXPECTED_VIDEO_BIT_RATE;
        } else if (pRtcMediaStreamTrack->kind == MEDIA_STREAM_TRACK_KIND_AUDIO) {
            DLOGW("Rolling buffer bitrate does not fit range (%lf bps - %lf bps) for audio. Setting to default %lf bps.", MIN_EXPECTED_BIT_RATE,
                  MAX_EXPECTED_BIT_RATE, DEFAULT_EXPECTED_AUDIO_BIT_RATE);
            pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferBitratebps = DEFAULT_EXPECTED_AUDIO_BIT_RATE;
        } else {
            DLOGW("Rolling buffer bitrate does not fit range (%lf bps - %lf bps) for unknown codec. Setting to default %lf bps.",
                  MIN_EXPECTED_BIT_RATE, MAX_EXPECTED_BIT_RATE, DEFAULT_EXPECTED_VIDEO_BIT_RATE);
            pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferBitratebps = DEFAULT_EXPECTED_VIDEO_BIT_RATE;
        }
    } else if (rollingBufferBitratebps == 0) {
        if (pRtcMediaStreamTrack->kind == MEDIA_STREAM_TRACK_KIND_VIDEO) {
            DLOGI("Setting to default rolling buffer bitrate of %lf bps for video.", DEFAULT_EXPECTED_VIDEO_BIT_RATE);
            pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferBitratebps = DEFAULT_EXPECTED_VIDEO_BIT_RATE;
        } else if (pRtcMediaStreamTrack->kind == MEDIA_STREAM_TRACK_KIND_AUDIO) {
            DLOGI("Setting to default rolling buffer bitrate of %lf bps for audio.", DEFAULT_EXPECTED_AUDIO_BIT_RATE);
            pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferBitratebps = DEFAULT_EXPECTED_AUDIO_BIT_RATE;
        } else {
            DLOGI("Setting to default rolling buffer bitrate of %lf bps for unknown codec.", DEFAULT_EXPECTED_VIDEO_BIT_RATE);
            pKvsRtpTransceiver->pRollingBufferConfig->rollingBufferBitratebps = DEFAULT_EXPECTED_VIDEO_BIT_RATE;
        }
    }

CleanUp:
    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS freeTransceiver(PRtcRtpTransceiver* pRtcRtpTransceiver)
{
    UNUSED_PARAM(pRtcRtpTransceiver);
    return STATUS_NOT_IMPLEMENTED;
}

STATUS freeRollingBufferConfig(PRollingBufferConfig pRollingBufferConfig)
{
    SAFE_MEMFREE(pRollingBufferConfig);
    return STATUS_SUCCESS;
}

STATUS freeKvsRtpTransceiver(PKvsRtpTransceiver* ppKvsRtpTransceiver)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = NULL;

    CHK(ppKvsRtpTransceiver != NULL, STATUS_NULL_ARG);
    pKvsRtpTransceiver = *ppKvsRtpTransceiver;
    // free is idempotent
    CHK(pKvsRtpTransceiver != NULL, retStatus);

    if (pKvsRtpTransceiver->pJitterBuffer != NULL) {
        freeJitterBuffer(&pKvsRtpTransceiver->pJitterBuffer);
    }

    if (pKvsRtpTransceiver->sender.packetBuffer != NULL) {
        freeRtpRollingBuffer(&pKvsRtpTransceiver->sender.packetBuffer);
    }

    if (pKvsRtpTransceiver->sender.retransmitter != NULL) {
        freeRetransmitter(&pKvsRtpTransceiver->sender.retransmitter);
    }

    if (pKvsRtpTransceiver->sender.pRedSenderState != NULL) {
        freeRedSenderState(&pKvsRtpTransceiver->sender.pRedSenderState);
    }

    freeRollingBufferConfig(pKvsRtpTransceiver->pRollingBufferConfig);

    MUTEX_FREE(pKvsRtpTransceiver->statsLock);

    SAFE_MEMFREE(pKvsRtpTransceiver->peerFrameBuffer);
    SAFE_MEMFREE(pKvsRtpTransceiver->sender.payloadArray.payloadBuffer);
    SAFE_MEMFREE(pKvsRtpTransceiver->sender.payloadArray.payloadSubLength);

    SAFE_MEMFREE(pKvsRtpTransceiver);

    *ppKvsRtpTransceiver = NULL;

CleanUp:

    return retStatus;
}

STATUS kvsRtpTransceiverSetJitterBuffer(PKvsRtpTransceiver pKvsRtpTransceiver, PJitterBuffer pJitterBuffer)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pKvsRtpTransceiver != NULL && pJitterBuffer != NULL, STATUS_NULL_ARG);

    pKvsRtpTransceiver->pJitterBuffer = pJitterBuffer;

CleanUp:

    return retStatus;
}

STATUS transceiverOnFrame(PRtcRtpTransceiver pRtcRtpTransceiver, UINT64 customData, RtcOnFrame rtcOnFrame)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;

    CHK(pKvsRtpTransceiver != NULL && rtcOnFrame != NULL, STATUS_NULL_ARG);

    pKvsRtpTransceiver->onFrame = rtcOnFrame;
    pKvsRtpTransceiver->onFrameCustomData = customData;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS transceiverOnPartialFrame(PRtcRtpTransceiver pRtcRtpTransceiver, UINT64 customData, RtcOnFrame rtcOnPartialFrame)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;

    CHK(pKvsRtpTransceiver != NULL && rtcOnPartialFrame != NULL, STATUS_NULL_ARG);

    pKvsRtpTransceiver->onPartialFrame = rtcOnPartialFrame;
    pKvsRtpTransceiver->onPartialFrameCustomData = customData;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS transceiverOnBandwidthEstimation(PRtcRtpTransceiver pRtcRtpTransceiver, UINT64 customData, RtcOnBandwidthEstimation rtcOnBandwidthEstimation)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;

    CHK(pKvsRtpTransceiver != NULL && rtcOnBandwidthEstimation != NULL, STATUS_NULL_ARG);

    pKvsRtpTransceiver->onBandwidthEstimation = rtcOnBandwidthEstimation;
    pKvsRtpTransceiver->onBandwidthEstimationCustomData = customData;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS transceiverOnPictureLoss(PRtcRtpTransceiver pRtcRtpTransceiver, UINT64 customData, RtcOnPictureLoss onPictureLoss)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;

    CHK(pKvsRtpTransceiver != NULL && onPictureLoss != NULL, STATUS_NULL_ARG);

    pKvsRtpTransceiver->onPictureLoss = onPictureLoss;
    pKvsRtpTransceiver->onPictureLossCustomData = customData;

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS updateEncoderStats(PRtcRtpTransceiver pRtcRtpTransceiver, PRtcEncoderStats encoderStats)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;
    CHK(pKvsRtpTransceiver != NULL && encoderStats != NULL, STATUS_NULL_ARG);
    MUTEX_LOCK(pKvsRtpTransceiver->statsLock);
    pKvsRtpTransceiver->outboundStats.totalEncodeTime += encoderStats->encodeTimeMsec;
    pKvsRtpTransceiver->outboundStats.targetBitrate = encoderStats->targetBitrate;
    if (encoderStats->width < pKvsRtpTransceiver->outboundStats.frameWidth || encoderStats->height < pKvsRtpTransceiver->outboundStats.frameHeight) {
        pKvsRtpTransceiver->outboundStats.qualityLimitationResolutionChanges++;
    }

    pKvsRtpTransceiver->outboundStats.frameWidth = encoderStats->width;
    pKvsRtpTransceiver->outboundStats.frameHeight = encoderStats->height;
    pKvsRtpTransceiver->outboundStats.frameBitDepth = encoderStats->bitDepth;
    pKvsRtpTransceiver->outboundStats.voiceActivityFlag = encoderStats->voiceActivity;
    if (encoderStats->encoderImplementation[0] != '\0')
        STRNCPY(pKvsRtpTransceiver->outboundStats.encoderImplementation, encoderStats->encoderImplementation, MAX_STATS_STRING_LENGTH);

    MUTEX_UNLOCK(pKvsRtpTransceiver->statsLock);

CleanUp:
    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS writeFrame(PRtcRtpTransceiver pRtcRtpTransceiver, PFrame pFrame)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;
    PRtpPacket pPacketList = NULL, pRtpPacket = NULL;
    UINT32 i = 0, packetLen = 0, headerLen = 0, allocSize;
    PBYTE rawPacket = NULL;
    PPayloadArray pPayloadArray = NULL;
    RtpPayloadFunc rtpPayloadFunc = NULL;
    UINT64 randomRtpTimeoffset = 0; // TODO: spec requires random rtp time offset
    UINT64 rtpTimestamp = 0;
    UINT64 now = GETTIME();

    // stats updates
    DOUBLE fps = 0.0;
    UINT32 frames = 0, keyframes = 0, bytesSent = 0, packetsSent = 0, headerBytesSent = 0, framesSent = 0;
    UINT32 packetsDiscardedOnSend = 0, bytesDiscardedOnSend = 0, framesDiscardedOnSend = 0;
    UINT64 lastPacketSentTimestamp = 0;

    // temp vars :(
    UINT64 tmpFrames, tmpTime;
    UINT32 extpayload;
    STATUS sendStatus;

    // Batch pacing: collect packets for atomic frame enqueue
    PPacerPacketInfo pPacerPackets = NULL;
    UINT32 pacerPacketCount = 0;
    BOOL useBatchPacing = FALSE;

    CHK(pKvsRtpTransceiver != NULL && pFrame != NULL, STATUS_NULL_ARG);
    pKvsPeerConnection = pKvsRtpTransceiver->pKvsPeerConnection;
    pPayloadArray = &(pKvsRtpTransceiver->sender.payloadArray);
    if (MEDIA_STREAM_TRACK_KIND_VIDEO == pKvsRtpTransceiver->sender.track.kind) {
        frames++;
        if (0 != (pFrame->flags & FRAME_FLAG_KEY_FRAME)) {
            keyframes++;
        }
        if (pKvsRtpTransceiver->sender.lastKnownFrameCountTime == 0) {
            pKvsRtpTransceiver->sender.lastKnownFrameCountTime = now;
            pKvsRtpTransceiver->sender.lastKnownFrameCount = pKvsRtpTransceiver->outboundStats.framesEncoded + frames;
        } else if (now - pKvsRtpTransceiver->sender.lastKnownFrameCountTime > HUNDREDS_OF_NANOS_IN_A_SECOND) {
            tmpFrames = (pKvsRtpTransceiver->outboundStats.framesEncoded + frames) - pKvsRtpTransceiver->sender.lastKnownFrameCount;
            tmpTime = now - pKvsRtpTransceiver->sender.lastKnownFrameCountTime;
            fps = (DOUBLE) (tmpFrames * HUNDREDS_OF_NANOS_IN_A_SECOND) / (DOUBLE) tmpTime;
        }
    }

    CHK(pKvsPeerConnection->pSrtpSession != NULL, STATUS_SRTP_NOT_READY_YET); // Discard packets till SRTP is ready
    BOOL useOpusRed = FALSE;
    UINT8 effectivePayloadType = pKvsRtpTransceiver->sender.payloadType;
    switch (pKvsRtpTransceiver->sender.track.codec) {
        case RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE:
            rtpPayloadFunc = createPayloadForH264;
            rtpTimestamp = CONVERT_TIMESTAMP_TO_RTP(VIDEO_CLOCKRATE, pFrame->presentationTs);
            break;

        case RTC_CODEC_H265:
            rtpPayloadFunc = createPayloadForH265;
            rtpTimestamp = CONVERT_TIMESTAMP_TO_RTP(VIDEO_CLOCKRATE, pFrame->presentationTs);
            break;

        case RTC_CODEC_OPUS:
            rtpPayloadFunc = createPayloadForOpus;
            rtpTimestamp = CONVERT_TIMESTAMP_TO_RTP(OPUS_CLOCKRATE, pFrame->presentationTs);
            useOpusRed = (pKvsRtpTransceiver->sender.redPayloadType != 0 && pKvsRtpTransceiver->sender.pRedSenderState != NULL);
            break;

        case RTC_CODEC_MULAW:
        case RTC_CODEC_ALAW:
            rtpPayloadFunc = createPayloadForG711;
            rtpTimestamp = CONVERT_TIMESTAMP_TO_RTP(PCM_CLOCKRATE, pFrame->presentationTs);
            break;

        case RTC_CODEC_VP8:
            rtpPayloadFunc = createPayloadForVP8;
            rtpTimestamp = CONVERT_TIMESTAMP_TO_RTP(VIDEO_CLOCKRATE, pFrame->presentationTs);
            break;

        default:
            CHK(FALSE, STATUS_NOT_IMPLEMENTED);
    }

    rtpTimestamp += randomRtpTimeoffset;

    if (useOpusRed) {
        // Two-call pattern: first NULL for size, then with buffer.
        BOOL isFallback = FALSE;
        UINT32 subLenCap = 1;
        CHK_STATUS(createPayloadForOpusRed(pKvsPeerConnection->MTU, (PBYTE) pFrame->frameData, pFrame->size, (UINT32) rtpTimestamp,
                                           pKvsRtpTransceiver->sender.pRedSenderState, NULL, &(pPayloadArray->payloadLength), NULL, &subLenCap,
                                           &isFallback));
        if (pPayloadArray->payloadLength > pPayloadArray->maxPayloadLength) {
            SAFE_MEMFREE(pPayloadArray->payloadBuffer);
            pPayloadArray->payloadBuffer = (PBYTE) MEMALLOC(pPayloadArray->payloadLength);
            pPayloadArray->maxPayloadLength = pPayloadArray->payloadLength;
        }
        pPayloadArray->payloadSubLenSize = 1;
        if (pPayloadArray->maxPayloadSubLenSize < 1) {
            SAFE_MEMFREE(pPayloadArray->payloadSubLength);
            pPayloadArray->payloadSubLength = (PUINT32) MEMALLOC(SIZEOF(UINT32));
            pPayloadArray->maxPayloadSubLenSize = 1;
        }
        UINT32 subLenCap2 = 1;
        CHK_STATUS(createPayloadForOpusRed(pKvsPeerConnection->MTU, (PBYTE) pFrame->frameData, pFrame->size, (UINT32) rtpTimestamp,
                                           pKvsRtpTransceiver->sender.pRedSenderState, pPayloadArray->payloadBuffer, &(pPayloadArray->payloadLength),
                                           pPayloadArray->payloadSubLength, &subLenCap2, &isFallback));
        pPayloadArray->payloadSubLenSize = 1;
        effectivePayloadType = isFallback ? pKvsRtpTransceiver->sender.opusPayloadTypeForRed : pKvsRtpTransceiver->sender.redPayloadType;
    } else {
        CHK_STATUS(rtpPayloadFunc(pKvsPeerConnection->MTU, (PBYTE) pFrame->frameData, pFrame->size, NULL, &(pPayloadArray->payloadLength), NULL,
                                  &(pPayloadArray->payloadSubLenSize)));
        if (pPayloadArray->payloadLength > pPayloadArray->maxPayloadLength) {
            SAFE_MEMFREE(pPayloadArray->payloadBuffer);
            pPayloadArray->payloadBuffer = (PBYTE) MEMALLOC(pPayloadArray->payloadLength);
            pPayloadArray->maxPayloadLength = pPayloadArray->payloadLength;
        }
        if (pPayloadArray->payloadSubLenSize > pPayloadArray->maxPayloadSubLenSize) {
            SAFE_MEMFREE(pPayloadArray->payloadSubLength);
            pPayloadArray->payloadSubLength = (PUINT32) MEMALLOC(pPayloadArray->payloadSubLenSize * SIZEOF(UINT32));
            pPayloadArray->maxPayloadSubLenSize = pPayloadArray->payloadSubLenSize;
        }
        CHK_STATUS(rtpPayloadFunc(pKvsPeerConnection->MTU, (PBYTE) pFrame->frameData, pFrame->size, pPayloadArray->payloadBuffer,
                                  &(pPayloadArray->payloadLength), pPayloadArray->payloadSubLength, &(pPayloadArray->payloadSubLenSize)));
    }
    pPacketList = (PRtpPacket) MEMALLOC(pPayloadArray->payloadSubLenSize * SIZEOF(RtpPacket));

    if (!pKvsRtpTransceiver->sender.seqInitialized) {
        pKvsRtpTransceiver->sender.initialSequenceNumber = pKvsRtpTransceiver->sender.sequenceNumber;
        pKvsRtpTransceiver->sender.seqInitialized = TRUE;
    }
    CHK_STATUS(constructRtpPackets(pPayloadArray, effectivePayloadType, pKvsRtpTransceiver->sender.sequenceNumber, rtpTimestamp,
                                   pKvsRtpTransceiver->sender.ssrc, pPacketList, pPayloadArray->payloadSubLenSize));
    pKvsRtpTransceiver->sender.sequenceNumber = GET_UINT16_SEQ_NUM(pKvsRtpTransceiver->sender.sequenceNumber + pPayloadArray->payloadSubLenSize);

    // Queue video in the pacer when pacing is enabled; audio always sends immediately
    useBatchPacing = (pKvsPeerConnection->pPacer != NULL && pacerIsEnabled(pKvsPeerConnection->pPacer) &&
                      pKvsRtpTransceiver->sender.track.kind == MEDIA_STREAM_TRACK_KIND_VIDEO);
    if (useBatchPacing) {
        pPacerPackets = (PPacerPacketInfo) MEMALLOC(pPayloadArray->payloadSubLenSize * SIZEOF(PacerPacketInfo));
        CHK(pPacerPackets != NULL, STATUS_NOT_ENOUGH_MEMORY);
    }

    for (i = 0; i < pPayloadArray->payloadSubLenSize; i++) {
        pRtpPacket = pPacketList + i;
        if (pKvsPeerConnection->twccExtId != 0) {
            pRtpPacket->header.extension = TRUE;
            pRtpPacket->header.extensionProfile = TWCC_EXT_PROFILE;
            pRtpPacket->header.extensionLength = SIZEOF(UINT32);
            // Placeholder TWSN=0; real value assigned at send time by pacerSendRtpPacket
            extpayload = TWCC_PAYLOAD(pKvsPeerConnection->twccExtId, 0);
            pRtpPacket->header.extensionPayload = (PBYTE) &extpayload;
        }
        CHK_STATUS(createBytesFromRtpPacket(pRtpPacket, NULL, &packetLen));

        allocSize = packetLen + SRTP_AUTH_TAG_OVERHEAD;
        CHK(NULL != (rawPacket = (PBYTE) MEMALLOC(allocSize)), STATUS_NOT_ENOUGH_MEMORY);
        CHK_STATUS(createBytesFromRtpPacket(pRtpPacket, rawPacket, &packetLen));

        // Always buffer unencrypted bytes for RTX retransmission
        pRtpPacket->pRawPacket = rawPacket;
        pRtpPacket->rawPacketLength = packetLen;
        CHK_STATUS(rtpRollingBufferAddRtpPacket(pKvsRtpTransceiver->sender.packetBuffer, pRtpPacket));

        if (useBatchPacing) {
            pPacerPackets[pacerPacketCount].pData = rawPacket;
            pPacerPackets[pacerPacketCount].size = packetLen;
            pacerPacketCount++;
            rawPacket = NULL;

            headerLen = RTP_HEADER_LEN(pRtpPacket);
            bytesSent += packetLen + SRTP_AUTH_TAG_OVERHEAD - headerLen;
            packetsSent++;
            lastPacketSentTimestamp = KVS_CONVERT_TIMESCALE(GETTIME(), HUNDREDS_OF_NANOS_IN_A_SECOND, 1000);
            headerBytesSent += headerLen;
        } else {
            // Send immediately through pacer send function (assigns TWSN, encrypts, sends)
            MUTEX_LOCK(pKvsPeerConnection->pPacer->lock);
            sendStatus = pacerSendRtpPacket(pKvsPeerConnection->pPacer, rawPacket, packetLen);
            MUTEX_UNLOCK(pKvsPeerConnection->pPacer->lock);

            if (sendStatus == STATUS_SEND_DATA_FAILED) {
                packetsDiscardedOnSend++;
                bytesDiscardedOnSend += packetLen + SRTP_AUTH_TAG_OVERHEAD - headerLen;
                framesDiscardedOnSend = 1;
                SAFE_MEMFREE(rawPacket);
                continue;
            }
            CHK_STATUS(sendStatus);

            headerLen = RTP_HEADER_LEN(pRtpPacket);
            bytesSent += packetLen + SRTP_AUTH_TAG_OVERHEAD - headerLen;
            packetsSent++;
            lastPacketSentTimestamp = KVS_CONVERT_TIMESCALE(GETTIME(), HUNDREDS_OF_NANOS_IN_A_SECOND, 1000);
            headerBytesSent += headerLen;

            SAFE_MEMFREE(rawPacket);
        }
    }

    // Batch enqueue collected packets to pacer (ensures frame-deadline pacing sees full frame)
    if (useBatchPacing && pacerPacketCount > 0) {
        sendStatus = pacerEnqueueFrame(pKvsPeerConnection->pPacer, pPacerPackets, pacerPacketCount);
        if (STATUS_SUCCEEDED(sendStatus)) {
            pacerPacketCount = 0; // Pacer owns all packet data now
        } else {
            // pacerEnqueueFrame already freed all packet data on failure
            packetsDiscardedOnSend += pacerPacketCount;
            framesDiscardedOnSend = 1;
            pacerPacketCount = 0;
        }
    }

    if (MEDIA_STREAM_TRACK_KIND_VIDEO == pKvsRtpTransceiver->sender.track.kind) {
        framesSent++;
    }

    if (pKvsRtpTransceiver->sender.firstFrameWallClockTime == 0) {
        pKvsRtpTransceiver->sender.rtpTimeOffset = randomRtpTimeoffset;
        pKvsRtpTransceiver->sender.firstFrameWallClockTime = now;
    }

CleanUp:
    MUTEX_LOCK(pKvsRtpTransceiver->statsLock);
    pKvsRtpTransceiver->outboundStats.totalEncodedBytesTarget += pFrame->size;
    pKvsRtpTransceiver->outboundStats.framesEncoded += frames;
    pKvsRtpTransceiver->outboundStats.keyFramesEncoded += keyframes;
    if (fps > 0.0) {
        pKvsRtpTransceiver->outboundStats.framesPerSecond = fps;
    }
    pKvsRtpTransceiver->sender.lastKnownFrameCountTime = now;
    pKvsRtpTransceiver->sender.lastKnownFrameCount = pKvsRtpTransceiver->outboundStats.framesEncoded;
    pKvsRtpTransceiver->outboundStats.sent.bytesSent += bytesSent;
    pKvsRtpTransceiver->outboundStats.sent.packetsSent += packetsSent;
    if (lastPacketSentTimestamp > 0) {
        pKvsRtpTransceiver->outboundStats.lastPacketSentTimestamp = lastPacketSentTimestamp;
    }
    pKvsRtpTransceiver->outboundStats.headerBytesSent += headerBytesSent;
    pKvsRtpTransceiver->outboundStats.framesSent += framesSent;
    if (pKvsRtpTransceiver->outboundStats.framesPerSecond > 0.0) {
        if (pFrame->size >=
            pKvsRtpTransceiver->outboundStats.targetBitrate / pKvsRtpTransceiver->outboundStats.framesPerSecond * HUGE_FRAME_MULTIPLIER) {
            pKvsRtpTransceiver->outboundStats.hugeFramesSent++;
        }
    }
    // iceAgentSendPacket tries to send packet immediately, explicitly settings totalPacketSendDelay to 0
    pKvsRtpTransceiver->outboundStats.totalPacketSendDelay = 0;

    pKvsRtpTransceiver->outboundStats.framesDiscardedOnSend += framesDiscardedOnSend;
    pKvsRtpTransceiver->outboundStats.packetsDiscardedOnSend += packetsDiscardedOnSend;
    pKvsRtpTransceiver->outboundStats.bytesDiscardedOnSend += bytesDiscardedOnSend;
    MUTEX_UNLOCK(pKvsRtpTransceiver->statsLock);

    // Free un-enqueued pacer packets (if we jumped to CleanUp before batch enqueue)
    if (pPacerPackets != NULL) {
        for (i = 0; i < pacerPacketCount; i++) {
            SAFE_MEMFREE(pPacerPackets[i].pData);
        }
    }
    SAFE_MEMFREE(pPacerPackets);
    SAFE_MEMFREE(rawPacket);
    SAFE_MEMFREE(pPacketList);
    if (retStatus != STATUS_SRTP_NOT_READY_YET) {
        CHK_LOG_ERR(retStatus);
    }

    return retStatus;
}

STATUS writeRtpPacket(PKvsPeerConnection pKvsPeerConnection, PRtpPacket pRtpPacket)
{
    STATUS retStatus = STATUS_SUCCESS;
    PBYTE pRawPacket = NULL;

    CHK(pKvsPeerConnection != NULL && pRtpPacket != NULL && pRtpPacket->pRawPacket != NULL, STATUS_NULL_ARG);
    CHK(pKvsPeerConnection->pSrtpSession != NULL && pKvsPeerConnection->pPacer != NULL, STATUS_SUCCESS);

    pRawPacket = MEMALLOC(pRtpPacket->rawPacketLength + SRTP_AUTH_TAG_OVERHEAD);
    CHK(pRawPacket != NULL, STATUS_NOT_ENOUGH_MEMORY);
    MEMCPY(pRawPacket, pRtpPacket->pRawPacket, pRtpPacket->rawPacketLength);

    MUTEX_LOCK(pKvsPeerConnection->pPacer->lock);
    retStatus = pacerSendRtpPacket(pKvsPeerConnection->pPacer, pRawPacket, pRtpPacket->rawPacketLength);
    MUTEX_UNLOCK(pKvsPeerConnection->pPacer->lock);

    if (STATUS_SUCCEEDED(retStatus)) {
        pRawPacket = NULL; // encrypted in place, don't free
    }

CleanUp:
    SAFE_MEMFREE(pRawPacket);

    return retStatus;
}

STATUS hasTransceiverWithSsrc(PKvsPeerConnection pKvsPeerConnection, UINT32 ssrc)
{
    PKvsRtpTransceiver p = NULL;
    return findTransceiverBySsrc(pKvsPeerConnection, &p, ssrc);
}

STATUS findTransceiverBySsrc(PKvsPeerConnection pKvsPeerConnection, PKvsRtpTransceiver* ppTransceiver, UINT32 ssrc)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    UINT64 item = 0;
    PKvsRtpTransceiver pTransceiver = NULL;
    CHK(pKvsPeerConnection != NULL && ppTransceiver != NULL, STATUS_NULL_ARG);

    CHK_STATUS(doubleListGetHeadNode(pKvsPeerConnection->pTransceivers, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &item));
        pTransceiver = (PKvsRtpTransceiver) item;
        if (pTransceiver->sender.ssrc == ssrc || pTransceiver->sender.rtxSsrc == ssrc || pTransceiver->jitterBufferSsrc == ssrc) {
            break;
        }
        pTransceiver = NULL;
        pCurNode = pCurNode->pNext;
    }
    CHK(pTransceiver != NULL, STATUS_NOT_FOUND);
    *ppTransceiver = pTransceiver;

CleanUp:
    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS transceiverSendPli(PRtcRtpTransceiver pRtcRtpTransceiver)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;

    CHK(pKvsRtpTransceiver != NULL && pKvsRtpTransceiver->pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    CHK_STATUS(sendRtcpPLI(pKvsRtpTransceiver->pKvsPeerConnection, pKvsRtpTransceiver->sender.ssrc, pKvsRtpTransceiver->jitterBufferSsrc));

CleanUp:
    return retStatus;
}

STATUS transceiverSendFir(PRtcRtpTransceiver pRtcRtpTransceiver)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) pRtcRtpTransceiver;

    CHK(pKvsRtpTransceiver != NULL && pKvsRtpTransceiver->pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    CHK_STATUS(sendRtcpFIR(pKvsRtpTransceiver->pKvsPeerConnection, pKvsRtpTransceiver->sender.ssrc, pKvsRtpTransceiver->jitterBufferSsrc,
                           &pKvsRtpTransceiver->firSequenceNumber));

CleanUp:
    return retStatus;
}

STATUS requestKeyFrame(PRtcRtpTransceiver pRtcRtpTransceiver)
{
    return transceiverSendPli(pRtcRtpTransceiver);
}
