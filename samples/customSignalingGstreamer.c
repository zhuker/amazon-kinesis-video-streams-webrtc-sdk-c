#include <unistd.h>
#include <string.h>
#include <signal.h>
#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include "MySignaling.h"
#include "../source/Include_i.h"

STATUS myGenerateTimestampStr(UINT64 timestamp, PCHAR formatStr, PCHAR pDestBuffer, UINT32 destBufferLen, PUINT32 pFormattedStrLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    time_t timestampSeconds;
    UINT32 formattedStrLen;
    CHK(pDestBuffer != NULL, STATUS_NULL_ARG);
    CHK(STRNLEN(formatStr, MAX_TIMESTAMP_FORMAT_STR_LEN + 1) <= MAX_TIMESTAMP_FORMAT_STR_LEN, STATUS_MAX_TIMESTAMP_FORMAT_STR_LEN_EXCEEDED);

    UINT64 timestampMillis = (timestamp / HUNDREDS_OF_NANOS_IN_A_MILLISECOND) % 1000;
    timestampSeconds = timestamp / HUNDREDS_OF_NANOS_IN_A_SECOND;
    formattedStrLen = 0;
    *pFormattedStrLen = 0;

    char hms[128] = {0};
    formattedStrLen = (UINT32) STRFTIME(hms, 127, formatStr, GMTIME(&timestampSeconds));
    CHK(formattedStrLen != 0, STATUS_STRFTIME_FALIED);

    SNPRINTF(pDestBuffer, destBufferLen, "%s%03lu ", hms, timestampMillis);
    formattedStrLen += 4;

    pDestBuffer[formattedStrLen] = '\0';
    *pFormattedStrLen = formattedStrLen;

CleanUp:

    return retStatus;
}

PCHAR myGetLogLevelStr(UINT32 loglevel)
{
    switch (loglevel) {
        case LOG_LEVEL_VERBOSE:
            return LOG_LEVEL_VERBOSE_STR;
        case LOG_LEVEL_DEBUG:
            return LOG_LEVEL_DEBUG_STR;
        case LOG_LEVEL_INFO:
            return LOG_LEVEL_INFO_STR;
        case LOG_LEVEL_WARN:
            return LOG_LEVEL_WARN_STR;
        case LOG_LEVEL_ERROR:
            return LOG_LEVEL_ERROR_STR;
        case LOG_LEVEL_FATAL:
            return LOG_LEVEL_FATAL_STR;
        default:
            return LOG_LEVEL_SILENT_STR;
    }
}

VOID myAddLogMetadata(PCHAR buffer, UINT32 bufferLen, PCHAR fmt, UINT32 logLevel)
{
    UINT32 timeStrLen = 0;
    /* space for "yyyy-mm-dd HH:MM:SS\0" + msec + space + null */
    CHAR timeString[MAX_TIMESTAMP_FORMAT_STR_LEN + 4 + 1 + 1];
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 offset = 0;

#ifdef ENABLE_LOG_THREAD_ID
    // MAX_THREAD_ID_STR_LEN + null
    CHAR tidString[MAX_THREAD_ID_STR_LEN + 1];
    TID threadId = GETTID();
    SNPRINTF(tidString, ARRAY_SIZE(tidString), "(thread-0x%" PRIx64 ")", threadId);
#endif

    /* if something fails in getting time, still print the log, just without timestamp */
    retStatus = myGenerateTimestampStr(globalGetTime(), "%Y-%m-%d %H:%M:%S.", timeString, (UINT32) ARRAY_SIZE(timeString), &timeStrLen);
    if (STATUS_FAILED(retStatus)) {
        PRINTF("Fail to get time with status code is %08x\n", retStatus);
        timeString[0] = '\0';
    }

    offset = (UINT32) SNPRINTF(buffer, bufferLen, "%s%-*s ", timeString, MAX_LOG_LEVEL_STRLEN, myGetLogLevelStr(logLevel));
#ifdef ENABLE_LOG_THREAD_ID
    offset += SNPRINTF(buffer + offset, bufferLen - offset, "%s ", tidString);
#endif
    SNPRINTF(buffer + offset, bufferLen - offset, "%s\n", fmt);
}

//
// Default logger function
//
VOID myDefaultLogPrint(UINT32 level, PCHAR tag, PCHAR fmt, ...)
{
    CHAR logFmtString[MAX_LOG_FORMAT_LENGTH + 1];
    UINT32 logLevel = GET_LOGGER_LOG_LEVEL();

    UNUSED_PARAM(tag);

    if (level >= logLevel) {
        myAddLogMetadata(logFmtString, (UINT32) ARRAY_SIZE(logFmtString), fmt, level);

        va_list valist;
        va_start(valist, fmt);
        vprintf(logFmtString, valist);
        va_end(valist);
    }
}

typedef uint32_t (*GetBitrateKbpsFn)(GstElement*);
typedef void (*SetBitrateKbpsFn)(GstElement*, uint32_t kbps);

uint32_t x264enc_getbitrate(GstElement* encoder264)
{
    UINT32 current = 0;
    g_object_get(encoder264, "bitrate", &current, NULL);
    return current;
}

void x264enc_setbitrate(GstElement* encoder264, uint32_t kbps)
{
    g_object_set(encoder264, "bitrate", kbps, NULL);
}

uint32_t v4l2h264enc_getbitrate(GstElement* encoder264)
{
    UINT32 current = 0;
    GstStructure* extra = NULL;
    g_object_get(encoder264, "extra-controls", &extra, NULL);
    if (extra != NULL) {
        gst_structure_get_int(extra, "video_bitrate", &current);
    }
    return current / 1024;
}

void v4l2h264enc_setbitrate(GstElement* encoder264, uint32_t kbps)
{
    GstStructure* extra = gst_structure_new("s", "video_bitrate", G_TYPE_INT, kbps * 1024, NULL);
    g_object_set(encoder264, "extra-controls", extra, NULL);
    gst_structure_free(extra);
}

struct MySession {
    RtcConfiguration rtcConfig;
    PRtcPeerConnection pPeerConnection;

    RtcMediaStreamTrack videoTrack;
    PRtcRtpTransceiver transceiver;
    RTC_PEER_CONNECTION_STATE connectionState;

    BOOL iceGatheringDone;
    GstElement* encoder264; // to manipulate bitrate
    GstElement* texttopleft;
    RtcSessionDescriptionInit answerSdp;
    struct SimpleSignaling* signaling;
    StackQueue txKbpsHistory;
    StackQueue rxKbpsHistory;
    UINT32 bandwidthEstimationsSinceLastBitrateChange;
    GetBitrateKbpsFn getBitrateKbps;
    SetBitrateKbpsFn setBitrateKbps;
    UINT32 minBitrateKbps;
    UINT32 maxBitrateKbps;
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

static struct MySession session = {0};
VOID onIceCandidate(UINT64 session64, PCHAR candidate)
{
    STATUS retStatus = 0;

    printf("onIceCandidate: %s\n", candidate != NULL ? candidate : "NULL");
    if (candidate == NULL) {
        CHK_STATUS(createAnswer(session.pPeerConnection, &session.answerSdp));
        CHK_STATUS(setLocalDescription(session.pPeerConnection, &session.answerSdp));
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
    if (RTC_PEER_CONNECTION_STATE_FAILED == state || RTC_PEER_CONNECTION_STATE_CLOSED == state) {
        printf("connection failed exiting\n");
        exit(1);
    }
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

#define FOREACH_IN_SINGLELIST(pdlist)                                                                                                                \
    PSingleListNode pCurNode = NULL;                                                                                                                 \
    UINT64 item = 0;                                                                                                                                 \
    if (STATUS_SUCCESS == singleListGetHeadNode(pdlist, &pCurNode))                                                                                  \
        for (; pCurNode != NULL && STATUS_SUCCEEDED(singleListGetNodeData(pCurNode, &item)); pCurNode = pCurNode->pNext)

#define KBPS(bytes, msec) ((msec) == 0 ? 0 : ((bytes) *8 * 1000 / 1024) / (msec))

VOID onSenderBandwidth(UINT64 ipPeerConnection, UINT32 txBytes, UINT32 rxBytes, UINT32 txPacketsCnt, UINT32 rxPacketsCnt, UINT64 localDuration,
                       UINT64 remoteDuration)
{
    UINT64 localMsec = KVS_CONVERT_TIMESCALE(localDuration, HUNDREDS_OF_NANOS_IN_A_SECOND, MILLISECONDS_PER_SECOND);
    UINT64 remoteMsec = KVS_CONVERT_TIMESCALE(remoteDuration, HUNDREDS_OF_NANOS_IN_A_SECOND, MILLISECONDS_PER_SECOND);
    UINT64 txkbps = KBPS(txBytes, localMsec);
    UINT64 rxkbps = KBPS(rxBytes, remoteMsec);
    DLOGD("TX: %lu kbps RX: %lu kbps TX %u b (%u pkts) in %lu msec RX %u b (%u pkts) in %lu msec", txkbps, rxkbps, txBytes, txPacketsCnt, localMsec,
          rxBytes, rxPacketsCnt, remoteMsec);
    if (session.minBitrateKbps >= session.maxBitrateKbps)
        return;
    if (session.encoder264 == NULL)
        return;
    stackQueueEnqueue(&session.txKbpsHistory, txkbps);
    stackQueueEnqueue(&session.rxKbpsHistory, rxkbps);
    session.bandwidthEstimationsSinceLastBitrateChange++;
    if (session.txKbpsHistory.count < 30) {
        //        printf("not enough tx history\n");
        // not enough data
        return;
    }
    while (session.txKbpsHistory.count > 30) {
        UINT64 unused = 0;
        stackQueueDequeue(&session.txKbpsHistory, &unused);
    }
    if (session.rxKbpsHistory.count > 30) {
        UINT64 unused = 0;
        stackQueueDequeue(&session.rxKbpsHistory, &unused);
    }

    if (session.bandwidthEstimationsSinceLastBitrateChange < 10) {
        return;
    }
    session.bandwidthEstimationsSinceLastBitrateChange = 0;

    UINT64 sumtx = 0;
    {
        FOREACH_IN_SINGLELIST(&session.txKbpsHistory)
        {
            sumtx += item;
        }
    }

    UINT64 sumrx = 0;
    {
        FOREACH_IN_SINGLELIST(&session.rxKbpsHistory)
        {
            sumrx += item;
        }
    }

    txkbps = sumtx / session.txKbpsHistory.count;
    rxkbps = sumrx / session.rxKbpsHistory.count;

    UINT32 current = session.getBitrateKbps(session.encoder264);
//    DLOGD("current: %u", current);

    DOUBLE suggested = 0;
    if (rxkbps >= txkbps * 0.98) {
        suggested = (current * 1.02);
        DLOGD("increase current %u to %.0f next", current, suggested);
    } else if (rxkbps < txkbps * 0.9) {
        suggested = (current * 0.9);
        DLOGD("decrease current %u to %.0f next", current, suggested);
    }
    INT32 reportedNext = current;
    if (suggested > 0) {
        INT32 next = MAX(session.minBitrateKbps, (INT32) suggested);
        next = MIN(session.maxBitrateKbps, next);
        reportedNext = next;
//        DLOGD("next: %d", next);

        INT32 i = current - next;
        if (ABS(i) > 0) {
            DLOGS("current - next: %d", ABS(i));
            session.setBitrateKbps(session.encoder264, next);
        }
    }
    if (session.texttopleft != NULL) {
        char kbpsstr[80] = {0};
        snprintf(kbpsstr, 80, "%lu/%lu %u/%d kbps", txkbps, rxkbps, current, reportedNext);
        g_object_set(session.texttopleft, "text", kbpsstr, NULL);
    }
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
        DLOGS("buf_pts %lu", buf_pts);
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
                frame.presentationTs = buf_pts / 100LL;
                frame.decodingTs = buf_pts / 100LL;
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

typedef GstFlowReturn (*OnNewVideoSample)(GstElement*, gpointer);

GstFlowReturn on_new_video_sample(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_VIDEO_TRACK_ID);
}

#define ENCODER264_LABEL    "encoder264"
#define APPSINK_VIDEO_LABEL "appsink-video"
#define TEXT_LABEL          "texttopleft"
PVOID sendGstreamerVideo(PSampleConfiguration pSampleConfiguration, OnNewVideoSample on_video_sample)
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

    pipeline = gst_parse_launch(pSampleConfiguration->gstreamerInputPipeline, &error);

    if (pipeline == NULL) {
        printf("[KVS GStreamer Master] sendGstreamerAudioVideo(): Failed to launch gstreamer, operation returned status code: 0x%08x \n",
               STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    appsinkVideo = gst_bin_get_by_name(GST_BIN(pipeline), APPSINK_VIDEO_LABEL);
    session.encoder264 = gst_bin_get_by_name(GST_BIN(pipeline), ENCODER264_LABEL);
    session.texttopleft = gst_bin_get_by_name(GST_BIN(pipeline), TEXT_LABEL);
    gchar* encodertype = GST_OBJECT(gst_element_get_factory(session.encoder264))->name;
    if (!STRCMP("x264enc", encodertype)) {
        session.getBitrateKbps = x264enc_getbitrate;
        session.setBitrateKbps = x264enc_setbitrate;
    } else if (!STRCMP("v4l2h264enc", encodertype)) {
        session.getBitrateKbps = v4l2h264enc_getbitrate;
        session.setBitrateKbps = v4l2h264enc_setbitrate;
    } else {
        retStatus = STATUS_INTERNAL_ERROR;
        printf("unsupported encoder type '%s'\n", encodertype);
        goto CleanUp;
    }
    if (appsinkVideo == NULL) {
        printf("[KVS GStreamer Master] sendGstreamerVideo(): cant find appsink, operation returned status code: 0x%08x \n", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    if (appsinkVideo != NULL) {
        g_signal_connect(appsinkVideo, "new-sample", G_CALLBACK(on_video_sample), (gpointer) pSampleConfiguration);
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

    return (PVOID) (ULONG_PTR) retStatus;
}
static void sigint_handler(int sig)
{
    session.signaling->interrupted = TRUE;
}

BOOL allowInterface(UINT64 customData, PCHAR networkInt)
{
    PCHAR allowedInterface = (PCHAR) customData;
    BOOL useInterface = FALSE;
    if (STRNCMP(networkInt, allowedInterface, STRLEN(allowedInterface)) == 0) {
        useInterface = TRUE;
    }
    DLOGD("%s %s", networkInt, (useInterface) ? ("allowed. Candidates to be gathered") : ("blocked. Candidates will not be gathered"));
    return useInterface;
}
#define NETWORK_INTERFACE_NAME_PARAM "-i"
#define SIGNALING_LOCAL_ID_PARAM     "--our-id"
#define SIGNALING_REMOTE_ID_PARAM    "--remote-id"
#define STUN_HOSTNAME_PARAM          "-s"
#define VIDEO_DEV_PARAM              "-v"
#define HARDWARE_ENCODING_PARAM      "--hwenc"
#define MIN_BITRATE_PARAM            "--min-bitrate-kbps"
#define MAX_BITRATE_PARAM            "--max-bitrate-kbps"
//"stun:stun.l.google.com:19302"

GstFlowReturn noop_on_new_video_sample(GstElement* sink, gpointer data)
{
    GstFlowReturn ret = GST_FLOW_OK;
    DLOGD("noop_on_new_video_sample");
    return ret;
}

INT32 main5(INT32 argc, CHAR* argv[])
{
    globalCustomLogPrintFn = myDefaultLogPrint;
    SET_LOGGER_LOG_LEVEL(LOG_LEVEL_DEBUG);
    gst_init(&argc, &argv);
    SampleConfiguration config = {0};
    config.streamingSessionListReadLock = MUTEX_CREATE(FALSE);
    SampleStreamingSession sampleStreamingSession = {0};
    sampleStreamingSession.pVideoRtcRtpTransceiver = session.transceiver;
    config.streamingSessionCount = 1;
    config.sampleStreamingSessionList[0] = &sampleStreamingSession;
    // v4l2src device=/dev/video0 name=camerainput num-buffers=200 ! image/jpeg,width=1280,height=720,framerate=30/1 ! v4l2jpegdec ! textoverlay
    // halignment=left valignment=t op name=texttopleft  ! v4l2h264enc extra-controls=s,video_bitrate=1420000 name=encoder264 !
    // 'video/x-h264,level=(string)3.2'
    SNPRINTF(config.gstreamerInputPipeline, SIZEOF(config.gstreamerInputPipeline),
             "v4l2src device=/dev/video0 name=camerainput"
             " ! image/jpeg,width=1280,height=720,framerate=30/1"
             " ! v4l2jpegdec "
             " ! textoverlay halignment=left valignment=top name=" TEXT_LABEL
             " ! v4l2h264enc extra-controls=s,video_bitrate=1000000 name=" ENCODER264_LABEL " ! video/x-h264,level=(string)3.2"
             " ! appsink sync=TRUE emit-signals=TRUE name=" APPSINK_VIDEO_LABEL);
    printf("%s\n", config.gstreamerInputPipeline);

    sendGstreamerVideo(&config, noop_on_new_video_sample);
}

struct Args {
    PCHAR interfaceName;
    PCHAR ourId;
    PCHAR remoteId;
    PCHAR stunServer;
    PCHAR videoDev;
    UINT32 minBitrateKbps;
    UINT32 maxBitrateKbps;
    BOOL hwEnc;
};

void make_default_args(struct Args* args)
{
    args->interfaceName = NULL;
    args->ourId = NULL;
    args->remoteId = NULL;
    args->stunServer = NULL;
    args->videoDev = "/dev/video0";
    args->minBitrateKbps = 200;
    args->maxBitrateKbps = 1024;
    args->hwEnc = FALSE;
}

STATUS parse_args(struct Args* args, INT32 argc, CHAR* argv[])
{
    for (int i = 1; i < argc; ++i) {
        CHAR* param = argv[i];
        if (STRCMP(param, NETWORK_INTERFACE_NAME_PARAM) == 0) {
            args->interfaceName = argv[i + 1];
            i++;
        } else if (STRCMP(param, SIGNALING_LOCAL_ID_PARAM) == 0) {
            args->ourId = argv[i + 1];
            i++;
        } else if (STRCMP(param, SIGNALING_REMOTE_ID_PARAM) == 0) {
            args->remoteId = argv[i + 1];
            i++;
        } else if (STRCMP(param, STUN_HOSTNAME_PARAM) == 0) {
            args->stunServer = argv[i + 1];
            i++;
        } else if (STRCMP(param, VIDEO_DEV_PARAM) == 0) {
            args->videoDev = argv[i + 1];
            i++;
        } else if (STRCMP(param, MAX_BITRATE_PARAM) == 0) {
            if (!STATUS_SUCCEEDED(strtoui32(argv[i + 1], NULL, 10, &args->maxBitrateKbps))) {
                printf("Cant set max bitrate to '%s'\n", argv[i + 1]);
                return 4;
            }
            i++;
        } else if (STRCMP(param, MIN_BITRATE_PARAM) == 0) {
            if (!STATUS_SUCCEEDED(strtoui32(argv[i + 1], NULL, 10, &args->minBitrateKbps))) {
                printf("Cant set min bitrate to '%s'\n", argv[i + 1]);
                return 4;
            }
            i++;
        } else if (STRCMP(param, HARDWARE_ENCODING_PARAM) == 0) {
            args->hwEnc = TRUE;
        } else {
            printf("Unknown param %s\n", param);
            return 3;
        }
    }
    if (!args->ourId || !args->remoteId || args->maxBitrateKbps < args->minBitrateKbps) {
        printf("Usage: ./customSignalingGst\n"
               " --hwenc\n"
               " -v video_device (default: /dev/video0)\n"
               " -s stun:server.com:port (default: none)\n"
               " -i network-interface-name (default: any)\n"
               " " MIN_BITRATE_PARAM " kbps (default: 200)\n"
               " " MAX_BITRATE_PARAM " kbps (default: 1024)\n"
               " --our-id OURID\n"
               " --remote-id REMOTEID\n"
               "\n");
        return (2);
    }
    return STATUS_SUCCESS;
}

INT32 main(INT32 argc, CHAR* argv[])
{
    globalCustomLogPrintFn = myDefaultLogPrint;
    //    signal(SIGINT, sigint_handler);
    SET_LOGGER_LOG_LEVEL(LOG_LEVEL_DEBUG);
    gst_init(&argc, &argv);
    struct Args args = {0};
    make_default_args(&args);
    STATUS retStatus = parse_args(&args, argc, argv);
    if (retStatus != STATUS_SUCCESS) {
        return retStatus;
    }

    CHK_STATUS(initKvsWebRtc());
    DLOGD("init ok");
    //    struct CopyPasteSignaling copyPasteSignaling = {0};
    //    copyPasteSignalingInit(&copyPasteSignaling);
    struct WebsocketSignaling realSignaling = {0};
    STRNCPY(realSignaling.ourId, args.ourId, STRLEN(args.ourId));
    STRNCPY(realSignaling.remoteId, args.remoteId, STRLEN(args.remoteId));
    websocketSignalingInit(&realSignaling);
    struct SimpleSignaling* signaling = (struct SimpleSignaling*) &realSignaling;
    session.signaling = signaling;

    CHK_STATUS(signaling->connect(signaling));

    RtcSessionDescriptionInit offerSdp = {0};
    CHK_STATUS(signaling->receiveOffer(signaling, &offerSdp));

    session.rtcConfig.kvsRtcConfiguration.iceLocalCandidateGatheringTimeout = (5 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    session.rtcConfig.kvsRtcConfiguration.iceCandidateNominationTimeout = (120 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    session.rtcConfig.kvsRtcConfiguration.iceConnectionCheckTimeout = (60 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    if (args.interfaceName) {
        session.rtcConfig.kvsRtcConfiguration.iceSetInterfaceFilterFunc = allowInterface;
        session.rtcConfig.kvsRtcConfiguration.filterCustomData = (UINT64) args.interfaceName;
    }
    UINT64 session64 = (UINT64) &session;
    if (args.stunServer) {
        DLOGD("using stun %s", args.stunServer);
        STRNCPY(session.rtcConfig.iceServers[0].urls, args.stunServer, MAX_ICE_CONFIG_URI_LEN);
    }

    CHK_STATUS(createPeerConnection(&session.rtcConfig, &session.pPeerConnection));
    // twcc bandwidth estimation
    CHK_STATUS(peerConnectionOnSenderBandwidthEstimation(session.pPeerConnection, (UINT64) session.pPeerConnection, onSenderBandwidth));

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

    RtcSessionDescriptionInit emptyAnswer = {0};

    CHK_STATUS(setRemoteDescription(session.pPeerConnection, &offerSdp));
    CHK_STATUS(setLocalDescription(session.pPeerConnection, &emptyAnswer));

    BOOL answerSent = FALSE;
    while (1) {
        if (!session.iceGatheringDone) {
            printf("gathering...\n");
        } else if (!answerSent) {
            printf("sendAnswer\n");
            session.signaling->sendAnswer(session.signaling, &session.answerSdp);
            answerSent = TRUE;
        }
        if (session.connectionState == RTC_PEER_CONNECTION_STATE_CONNECTED) {
            break;
        }
        sleep(1);
    }
    session.signaling->destroy(session.signaling);

    if (session.connectionState == RTC_PEER_CONNECTION_STATE_CONNECTED) {
        // send frames
        SampleConfiguration config = {0};
        config.streamingSessionListReadLock = MUTEX_CREATE(FALSE);
        SampleStreamingSession sampleStreamingSession = {0};
        sampleStreamingSession.pVideoRtcRtpTransceiver = session.transceiver;
        config.streamingSessionCount = 1;
        config.sampleStreamingSessionList[0] = &sampleStreamingSession;
        session.minBitrateKbps = args.minBitrateKbps;
        session.maxBitrateKbps = args.maxBitrateKbps;
        PCHAR swEncFmt = "v4l2src device=%s name=camerainput"
                         " ! image/jpeg,width=1280,height=720,framerate=30/1"
                         " ! jpegdec"
                         " ! videoconvert"
                         " ! textoverlay halignment=left valignment=top name=" TEXT_LABEL " ! video/x-raw,format=I420"
                         " ! videoconvert"
                         " ! x264enc bframes=0 speed-preset=fast bitrate=%d byte-stream=TRUE tune=zerolatency name=" ENCODER264_LABEL
                         " ! video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline"
                         " ! appsink sync=TRUE emit-signals=TRUE name=" APPSINK_VIDEO_LABEL;

        PCHAR hwEncFmt = "v4l2src device=%s name=camerainput"
                         " ! image/jpeg,width=1280,height=720,framerate=30/1"
                         " ! v4l2jpegdec "
                         " ! textoverlay halignment=left valignment=top name=" TEXT_LABEL
                         " ! v4l2h264enc extra-controls=s,video_bitrate=%d name=" ENCODER264_LABEL " ! video/x-h264,level=(string)3.2"
                         " ! appsink sync=TRUE emit-signals=TRUE name=" APPSINK_VIDEO_LABEL;

        if (args.hwEnc) {
            SNPRINTF(config.gstreamerInputPipeline, SIZEOF(config.gstreamerInputPipeline), hwEncFmt, args.videoDev, args.maxBitrateKbps * 1024);
        } else {
            SNPRINTF(config.gstreamerInputPipeline, SIZEOF(config.gstreamerInputPipeline), swEncFmt, args.videoDev, args.maxBitrateKbps);
        }
        printf("%s\n", config.gstreamerInputPipeline);
        sendGstreamerVideo(&config, on_new_video_sample);
    }
    printf("done\n");
    return 0;
CleanUp:
    printf("error %x\n", retStatus);
    return retStatus;
}

int main2(int argc, const char** argv)
{
    signal(SIGINT, sigint_handler);
    struct WebsocketSignaling wssig = {0};
    struct SimpleSignaling* signaling = (struct SimpleSignaling*) &wssig;
    websocketSignalingInit(&wssig);

    signaling->connect(signaling);
    RtcSessionDescriptionInit offer = {0};
    signaling->receiveOffer(signaling, &offer);
    printf("offer: '%s'", offer.sdp);

    signaling->destroy(signaling);
    return 0;
}

#define FOREACH_IN_DOUBLELIST(pdlist)                                                                                                                \
    PDoubleListNode pCurNode = NULL;                                                                                                                 \
    UINT64 item = 0;                                                                                                                                 \
    CHK_STATUS(doubleListGetHeadNode(pdlist, &pCurNode));                                                                                            \
    for (; pCurNode != NULL && STATUS_SUCCEEDED(doubleListGetNodeData(pCurNode, &item)); pCurNode = pCurNode->pNext)

int main3(int argc, const char** argv)
{
    STATUS retStatus;
    DoubleList dlist = {0};
    doubleListInsertItemTail(&dlist, 11);
    doubleListInsertItemTail(&dlist, 22);
    doubleListInsertItemTail(&dlist, 33);

    FOREACH_IN_DOUBLELIST(&dlist)
    {
        printf("%lu\n", item);
    }
CleanUp:
    return 0;
}
