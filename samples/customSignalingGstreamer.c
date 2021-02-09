#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

struct MySession {
    RtcConfiguration rtcConfig;
    PRtcPeerConnection pPeerConnection;

    RtcMediaStreamTrack videoTrack;
    PRtcRtpTransceiver transceiver;
    RTC_PEER_CONNECTION_STATE connectionState;

    BOOL iceGatheringDone;
};

static const char* ConnectionStateNames[] = {
    "RTC_PEER_CONNECTION_STATE_NONE",      "RTC_PEER_CONNECTION_STATE_NEW",          "RTC_PEER_CONNECTION_STATE_CONNECTING",
    "RTC_PEER_CONNECTION_STATE_CONNECTED", "RTC_PEER_CONNECTION_STATE_DISCONNECTED", "RTC_PEER_CONNECTION_STATE_FAILED",
    "RTC_PEER_CONNECTION_STATE_CLOSED",    "RTC_PEER_CONNECTION_TOTAL_STATE_COUNT",  NULL};

static const char* connection_state_to_string(RTC_PEER_CONNECTION_STATE state)
{
    BOOL rangeok = state >= RTC_PEER_CONNECTION_STATE_NONE && state <= RTC_PEER_CONNECTION_TOTAL_STATE_COUNT;
    return rangeok ? ConnectionStateNames[state] : "RTC_PEER_CONNECTION_TOTAL_STATE_UNKNOWN";
}

// https://stackoverflow.com/questions/39546500/how-to-make-scanf-to-read-more-than-4095-characters-given-as-input
int clear_icanon(void)
{
    struct termios settings = {0};
    if (tcgetattr(STDIN_FILENO, &settings) < 0) {
        perror("error in tcgetattr");
        return 0;
    }

    settings.c_lflag &= ~ICANON;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) < 0) {
        perror("error in tcsetattr");
        return 0;
    }
    return 1;
}

static struct MySession session = {0};
VOID onIceCandidate(UINT64 session64, PCHAR candidate)
{
    STATUS retStatus = 0;

    printf("onIceCandidate: %s\n", candidate != NULL ? candidate : "NULL");
    if (candidate == NULL) {
        RtcSessionDescriptionInit answerSdp = {0};
        CHK_STATUS(createAnswer(session.pPeerConnection, &answerSdp));
        CHK_STATUS(setLocalDescription(session.pPeerConnection, &answerSdp));
        printf("answer: '%s'\n", answerSdp.sdp);
        char json[8192] = {0};
        UINT32 sz = 8192;
        CHK_STATUS(serializeSessionDescriptionInit(&answerSdp, json, &sz));
        printf("---- Please copy and send this message to the other peer ----\n");
        printf("%s\n", json);
        session.iceGatheringDone = TRUE;
    }
CleanUp:
    if (STATUS_FAILED(retStatus))
        printf("onIceCandidate failed 0x%x\n", retStatus);
}

VOID onConnectionStateChange_(UINT64 session64, RTC_PEER_CONNECTION_STATE state)
{
    printf("onConnectionStateChange: %s\n", connection_state_to_string(state));
    session.connectionState = state;
}

void onRemoteMessage(UINT64 session64, PRtcDataChannel pRtcDataChannel, BOOL binary, PBYTE data, UINT32 len)
{
    char text[128] = {0};
    snprintf(text, MIN(len, 127) + 1, "%s", data);
    printf("onRemoteMessage %s %s %s\n", pRtcDataChannel->name, binary ? "bin" : "text", text);
    dataChannelSend(pRtcDataChannel, FALSE, "pong", 4);
}

void onRemoteDataChannel(UINT64 session64, PRtcDataChannel pRtcDataChannel)
{
    printf("remote data channel '%s'\n", pRtcDataChannel->name);
    dataChannelOnMessage(pRtcDataChannel, session64, onRemoteMessage);
}

void onBandwidthEstimation(UINT64 session64, DOUBLE bitrate)
{
    printf("onBandwidthEstimation %dkbps\n", (int) (bitrate / 1000));
}

GstFlowReturn on_new_sample(GstElement* sink, gpointer data, UINT64 trackid)
{
    GstBuffer* buffer;
    BOOL isDroppable, delta;
    GstFlowReturn ret = GST_FLOW_OK;
    GstSample* sample = NULL;
    GstMapInfo info;
    GstSegment* segment;
    GstClockTime buf_pts;
    Frame frame;
    STATUS status;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) data;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
    UINT32 i;

    if (pSampleConfiguration == NULL) {
        printf("[KVS GStreamer Master] on_new_sample(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    info.data = NULL;
    sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));

    buffer = gst_sample_get_buffer(sample);
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
        (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
        (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
        // drop if buffer contains header only and has invalid timestamp
        !GST_BUFFER_PTS_IS_VALID(buffer);

    if (!isDroppable) {
        delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        frame.flags = delta ? FRAME_FLAG_NONE : FRAME_FLAG_KEY_FRAME;

        // convert from segment timestamp to running time in live mode.
        segment = gst_sample_get_segment(sample);
        buf_pts = gst_segment_to_running_time(segment, GST_FORMAT_TIME, buffer->pts);
        if (!GST_CLOCK_TIME_IS_VALID(buf_pts)) {
            printf("[KVS GStreamer Master] Frame contains invalid PTS dropping the frame. \n");
        }

        if (!(gst_buffer_map(buffer, &info, GST_MAP_READ))) {
            printf("[KVS GStreamer Master] on_new_sample(): Gst buffer mapping failed\n");
            goto CleanUp;
        }

        frame.trackId = trackid;
        frame.duration = 0;
        frame.version = FRAME_CURRENT_VERSION;
        frame.size = (UINT32) info.size;
        frame.frameData = (PBYTE) info.data;

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
            frame.index = (UINT32) ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);

            if (trackid == DEFAULT_AUDIO_TRACK_ID) {
                pRtcRtpTransceiver = pSampleStreamingSession->pAudioRtcRtpTransceiver;
                frame.presentationTs = pSampleStreamingSession->audioTimestamp;
                frame.decodingTs = frame.presentationTs;
                pSampleStreamingSession->audioTimestamp +=
                    SAMPLE_AUDIO_FRAME_DURATION; // assume audio frame size is 20ms, which is default in opusenc
            } else {
                pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                frame.decodingTs = frame.presentationTs;
                pSampleStreamingSession->videoTimestamp += SAMPLE_VIDEO_FRAME_DURATION; // assume video fps is 30
            }
            status = writeFrame(pRtcRtpTransceiver, &frame);
            if (status != STATUS_SRTP_NOT_READY_YET && status != STATUS_SUCCESS) {
#ifdef VERBOSE
                printf("writeFrame() failed with 0x%08x", status);
#endif
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
    }

CleanUp:

    if (info.data != NULL) {
        gst_buffer_unmap(buffer, &info);
    }

    if (sample != NULL) {
        gst_sample_unref(sample);
    }

    if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        ret = GST_FLOW_EOS;
    }

    return ret;
}

GstFlowReturn on_new_video_sample(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_VIDEO_TRACK_ID);
}

PVOID sendGstreamerVideo(PSampleConfiguration pSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *appsinkVideo = NULL, *pipeline = NULL;
    GstBus* bus;
    GstMessage* msg;
    GError* error = NULL;

    if (pSampleConfiguration == NULL) {
        printf("[KVS GStreamer Master] sendGstreamerAudioVideo(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    pipeline = gst_parse_launch(
        "v4l2src device=/dev/video1 ! image/jpeg,width=1280,height=720,framerate=30/1 ! jpegdec ! videoconvert !"
        "x264enc bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
        "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE name=appsink-video",
        &error);

    if (pipeline == NULL) {
        printf("[KVS GStreamer Master] sendGstreamerAudioVideo(): Failed to launch gstreamer, operation returned status code: 0x%08x \n",
               STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    appsinkVideo = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-video");

    if (appsinkVideo == NULL) {
        printf("[KVS GStreamer Master] sendGstreamerVideo(): cant find appsink, operation returned status code: 0x%08x \n", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    if (appsinkVideo != NULL) {
        g_signal_connect(appsinkVideo, "new-sample", G_CALLBACK(on_new_video_sample), (gpointer) pSampleConfiguration);
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* block until error or EOS */
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    /* Free resources */
    if (msg != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

CleanUp:

    if (error != NULL) {
        printf("%s", error->message);
        g_clear_error(&error);
    }

    return (PVOID)(ULONG_PTR) retStatus;
}

INT32 main(INT32 argc, CHAR* argv[])
{
    SET_LOGGER_LOG_LEVEL(LOG_LEVEL_DEBUG);
    clear_icanon(); // Changes the input mode of terminal from canonical mode to non canonical mode to allow copy-paste of over 4096 bytes
    // equivalent to running "stty -icanon"
    gst_init(&argc, &argv);

    STATUS retStatus = 0;
    CHK_STATUS(initKvsWebRtc());

    printf("---- Please paste in the message here from the other peer ----\n");
    RtcSessionDescriptionInit offerSdp = {SDP_TYPE_OFFER};

    char offer[8192] = {0};
    fgets(offer, 8192, stdin);
    CHK_STATUS(deserializeSessionDescriptionInit(offer, STRLEN(offer), &offerSdp));
    printf("%s\n", offerSdp.sdp);

    session.rtcConfig.kvsRtcConfiguration.iceLocalCandidateGatheringTimeout = (5 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    session.rtcConfig.kvsRtcConfiguration.iceCandidateNominationTimeout = (120 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    session.rtcConfig.kvsRtcConfiguration.iceConnectionCheckTimeout = (60 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    UINT64 session64 = (UINT64) &session;
    STRNCPY(session.rtcConfig.iceServers[0].urls, "stun:stun.l.google.com:19302", MAX_ICE_CONFIG_URI_LEN);

    CHK_STATUS(createPeerConnection(&session.rtcConfig, &session.pPeerConnection));
    CHK_STATUS(peerConnectionOnIceCandidate(session.pPeerConnection, session64, onIceCandidate));
    CHK_STATUS(peerConnectionOnConnectionStateChange(session.pPeerConnection, session64, onConnectionStateChange_));
    CHK_STATUS(peerConnectionOnDataChannel(session.pPeerConnection, session64, onRemoteDataChannel));
    CHK_STATUS(addSupportedCodec(session.pPeerConnection, RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE));
    CHK_STATUS(addSupportedCodec(session.pPeerConnection, RTC_CODEC_OPUS));
    RtcRtpTransceiverInit trackinit = {RTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY};
    session.videoTrack.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    session.videoTrack.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
    STRNCPY(session.videoTrack.streamId, "streamId", MAX_MEDIA_STREAM_ID_LEN);
    STRNCPY(session.videoTrack.trackId, "trackId", MAX_MEDIA_STREAM_ID_LEN);

    CHK_STATUS(addTransceiver(session.pPeerConnection, &session.videoTrack, &trackinit, &session.transceiver));
    CHK_STATUS(transceiverOnBandwidthEstimation(session.transceiver, session64, onBandwidthEstimation));

    RtcSessionDescriptionInit answerSdp = {0};

    CHK_STATUS(setRemoteDescription(session.pPeerConnection, &offerSdp));
    CHK_STATUS(setLocalDescription(session.pPeerConnection, &answerSdp));

    while (1) {
        if (!session.iceGatheringDone) {
            printf("gathering...\n");
        }
        if (session.connectionState == RTC_PEER_CONNECTION_STATE_CONNECTED) {
            break;
        }
        sleep(1);
    }
    if (session.connectionState == RTC_PEER_CONNECTION_STATE_CONNECTED) {
        // send frames
        SampleConfiguration config = {0};
        config.streamingSessionListReadLock = MUTEX_CREATE(FALSE);
        SampleStreamingSession sampleStreamingSession = {0};
        sampleStreamingSession.pVideoRtcRtpTransceiver = session.transceiver;
        config.streamingSessionCount = 1;
        config.sampleStreamingSessionList[0] = &sampleStreamingSession;
        sendGstreamerVideo(&config);
    }

    return 0;
CleanUp:
    return retStatus;
}