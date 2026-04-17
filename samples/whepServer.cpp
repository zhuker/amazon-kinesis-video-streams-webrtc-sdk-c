/**
 * Simple WHEP Server - HTTP-based WebRTC signaling server
 * Streams H264 video to browser clients using WebRTC
 */

#include "httplib.h"

extern "C" {
#include "SamplesCommon.h"
}

#include <atomic>
#include <thread>
#include <fstream>
#include <sstream>

// Session state for single connection
struct WhepSession {
    RtcConfiguration rtcConfig;
    PRtcPeerConnection pPeerConnection;
    RtcMediaStreamTrack videoTrack;
    RtcMediaStreamTrack audioTrack;
    PRtcRtpTransceiver pVideoTransceiver;
    PRtcRtpTransceiver pAudioTransceiver;
    RTC_PEER_CONNECTION_STATE connectionState;
    std::atomic<bool> iceGatheringDone;
    std::atomic<bool> shouldExit;
    RtcSessionDescriptionInit answerSdp;
    PBYTE pVideoFrameBuffer;
    UINT32 videoBufferSize;
    PBYTE pAudioFrameBuffer;
    UINT32 audioBufferSize;
    httplib::Server* pServer;
    std::thread videoThread;
    std::thread audioThread;
    char stunServer[256];
    bool enableAudio;
};

static WhepSession g_session;

static const char* connectionStateToString(RTC_PEER_CONNECTION_STATE state)
{
    switch (state) {
        case RTC_PEER_CONNECTION_STATE_NONE:
            return "NONE";
        case RTC_PEER_CONNECTION_STATE_NEW:
            return "NEW";
        case RTC_PEER_CONNECTION_STATE_CONNECTING:
            return "CONNECTING";
        case RTC_PEER_CONNECTION_STATE_CONNECTED:
            return "CONNECTED";
        case RTC_PEER_CONNECTION_STATE_DISCONNECTED:
            return "DISCONNECTED";
        case RTC_PEER_CONNECTION_STATE_FAILED:
            return "FAILED";
        case RTC_PEER_CONNECTION_STATE_CLOSED:
            return "CLOSED";
        default:
            return "UNKNOWN";
    }
}

// Video streaming thread function
static void videoStreamingThread(WhepSession* session)
{
    Frame frame;
    MEMSET(&frame, 0, SIZEOF(Frame));
    UINT32 fileIndex = 0;
    UINT32 frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT64 startTime = GETTIME();
    UINT64 lastFrameTime = startTime;

    printf("[WHEP] Video streaming started\n");

    while (!session->shouldExit.load()) {
        fileIndex = fileIndex % NUMBER_OF_H264_FRAME_FILES + 1;
        SNPRINTF(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%04d.h264", fileIndex);

        // Get frame size first
        status = readFrameFromDisk(NULL, &frameSize, filePath);
        if (STATUS_FAILED(status)) {
            printf("[WHEP] Failed to read frame size from %s: 0x%08x\n", filePath, status);
            break;
        }

        // Reallocate buffer if needed
        if (frameSize > session->videoBufferSize) {
            session->pVideoFrameBuffer = (PBYTE) MEMREALLOC(session->pVideoFrameBuffer, frameSize);
            if (session->pVideoFrameBuffer == NULL) {
                printf("[WHEP] Failed to allocate video buffer\n");
                break;
            }
            session->videoBufferSize = frameSize;
        }

        frame.frameData = session->pVideoFrameBuffer;
        frame.size = frameSize;

        // Read frame data
        status = readFrameFromDisk(frame.frameData, &frameSize, filePath);
        if (STATUS_FAILED(status)) {
            printf("[WHEP] Failed to read frame data: 0x%08x\n", status);
            break;
        }

        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;

        // Send frame
        status = writeFrame(session->pVideoTransceiver, &frame);
        if (status != STATUS_SUCCESS && status != STATUS_SRTP_NOT_READY_YET) {
            // Don't spam logs for common errors
            if (status != STATUS_SRTP_NOT_READY_YET) {
                printf("[WHEP] writeFrame failed: 0x%08x\n", status);
            }
        }

        // Sleep for frame duration (25 FPS)
        UINT64 elapsed = lastFrameTime - startTime;
        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION - elapsed % SAMPLE_VIDEO_FRAME_DURATION);
        lastFrameTime = GETTIME();
    }

    printf("[WHEP] Video streaming stopped\n");
}

// Audio streaming thread function
static void audioStreamingThread(WhepSession* session)
{
    Frame frame;
    MEMSET(&frame, 0, SIZEOF(Frame));
    UINT32 fileIndex = 0;
    UINT32 frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT64 startTime = GETTIME();
    UINT64 lastFrameTime = startTime;

    printf("[WHEP] Audio streaming started\n");

    while (!session->shouldExit.load()) {
        fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES;
        SNPRINTF(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);

        // Get frame size first
        status = readFrameFromDisk(NULL, &frameSize, filePath);
        if (STATUS_FAILED(status)) {
            printf("[WHEP] Failed to read audio frame size from %s: 0x%08x\n", filePath, status);
            break;
        }

        // Reallocate buffer if needed
        if (frameSize > session->audioBufferSize) {
            session->pAudioFrameBuffer = (PBYTE) MEMREALLOC(session->pAudioFrameBuffer, frameSize);
            if (session->pAudioFrameBuffer == NULL) {
                printf("[WHEP] Failed to allocate audio buffer\n");
                break;
            }
            session->audioBufferSize = frameSize;
        }

        frame.frameData = session->pAudioFrameBuffer;
        frame.size = frameSize;

        // Read frame data
        status = readFrameFromDisk(frame.frameData, &frameSize, filePath);
        if (STATUS_FAILED(status)) {
            printf("[WHEP] Failed to read audio frame data: 0x%08x\n", status);
            break;
        }

        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        // Send frame
        status = writeFrame(session->pAudioTransceiver, &frame);
        if (status != STATUS_SUCCESS && status != STATUS_SRTP_NOT_READY_YET) {
            // Don't spam logs for common errors
        }

        fileIndex++;

        // Sleep for audio frame duration (20ms for Opus)
        UINT64 elapsed = lastFrameTime - startTime;
        THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION - elapsed % SAMPLE_AUDIO_FRAME_DURATION);
        lastFrameTime = GETTIME();
    }

    printf("[WHEP] Audio streaming stopped\n");
}

// Data channel message callback
static VOID onDataChannelMessage(UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMessage, UINT32 messageLen)
{
    CHAR msgBuffer[256];
    UINT32 printLen = MIN(messageLen, SIZEOF(msgBuffer) - 1);
    MEMCPY(msgBuffer, pMessage, printLen);
    msgBuffer[printLen] = '\0';

    printf("[WHEP] DataChannel '%s' message (%s, %u bytes): %s\n", pDataChannel->name, isBinary ? "binary" : "text", messageLen, msgBuffer);

    // Respond with "pong"
    STATUS status = dataChannelSend(pDataChannel, FALSE, (PBYTE) "pong", 4);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to send pong: 0x%08x\n", status);
    }
}

// Data channel callback (called when remote peer opens a data channel)
static VOID onDataChannelCallback(UINT64 customData, PRtcDataChannel pDataChannel)
{
    printf("[WHEP] DataChannel opened: '%s'\n", pDataChannel->name);

    STATUS status = dataChannelOnMessage(pDataChannel, customData, onDataChannelMessage);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to set data channel message callback: 0x%08x\n", status);
    }
}

// ICE candidate callback
static VOID onIceCandidateCallback(UINT64 customData, PCHAR candidateJson)
{
    WhepSession* session = (WhepSession*) customData;

    if (candidateJson == NULL) {
        printf("[WHEP] ICE gathering complete\n");
        session->iceGatheringDone.store(true);
    } else {
        printf("[WHEP] ICE candidate: %.80s...\n", candidateJson);
    }
}

// Connection state change callback
static VOID onConnectionStateChangeCallback(UINT64 customData, RTC_PEER_CONNECTION_STATE newState)
{
    WhepSession* session = (WhepSession*) customData;

    printf("[WHEP] Connection state: %s\n", connectionStateToString(newState));
    session->connectionState = newState;

    switch (newState) {
        case RTC_PEER_CONNECTION_STATE_CONNECTED:
            printf("[WHEP] Starting %sstreaming...\n", session->enableAudio ? "audio/video " : "video ");
            session->videoThread = std::thread(videoStreamingThread, session);
            if (session->enableAudio) {
                session->audioThread = std::thread(audioStreamingThread, session);
            }
            break;

        case RTC_PEER_CONNECTION_STATE_DISCONNECTED:
        case RTC_PEER_CONNECTION_STATE_FAILED:
        case RTC_PEER_CONNECTION_STATE_CLOSED:
            printf("[WHEP] Connection ended, shutting down...\n");
            session->shouldExit.store(true);
            if (session->pServer) {
                session->pServer->stop();
            }
            break;

        default:
            break;
    }
}

// Handle POST /offer endpoint
static void handleOffer(WhepSession* session, const std::string& body, httplib::Response& res)
{
    STATUS status = STATUS_SUCCESS;

    printf("[WHEP] Received offer (%zu bytes)\n", body.length());

    // Check if already connected
    if (session->pPeerConnection != NULL) {
        res.status = 409;
        res.set_content("{\"error\": \"Already connected\"}", "application/json");
        return;
    }

    // Parse incoming SDP offer from JSON
    RtcSessionDescriptionInit offerSdp;
    MEMSET(&offerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
    offerSdp.type = SDP_TYPE_OFFER;

    // Copy body to mutable buffer (SDK expects non-const PCHAR)
    CHAR* offerJson = (CHAR*) MEMALLOC(body.length() + 1);
    if (offerJson == NULL) {
        res.status = 500;
        res.set_content("{\"error\": \"Memory allocation failed\"}", "application/json");
        return;
    }
    MEMCPY(offerJson, body.c_str(), body.length());
    offerJson[body.length()] = '\0';

    status = deserializeSessionDescriptionInit(offerJson, (UINT32) body.length(), &offerSdp);
    MEMFREE(offerJson);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to parse SDP offer: 0x%08x\n", status);
        res.status = 400;
        res.set_content("{\"error\": \"Invalid SDP\"}", "application/json");
        return;
    }

    // Create peer connection
    status = createPeerConnection(&session->rtcConfig, &session->pPeerConnection);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to create peer connection: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to create peer connection\"}", "application/json");
        return;
    }

    // Setup callbacks
    status = peerConnectionOnIceCandidate(session->pPeerConnection, (UINT64) session, onIceCandidateCallback);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to set ICE callback: 0x%08x\n", status);
    }

    status = peerConnectionOnConnectionStateChange(session->pPeerConnection, (UINT64) session, onConnectionStateChangeCallback);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to set connection state callback: 0x%08x\n", status);
    }

    status = peerConnectionOnDataChannel(session->pPeerConnection, (UINT64) session, onDataChannelCallback);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to set data channel callback: 0x%08x\n", status);
    }

    // Add video codec
    status = addSupportedCodec(session->pPeerConnection, RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to add video codec: 0x%08x\n", status);
    }

    // Add audio codec (Opus) if enabled
    if (session->enableAudio) {
        status = addSupportedCodec(session->pPeerConnection, RTC_CODEC_OPUS);
        if (STATUS_FAILED(status)) {
            printf("[WHEP] Failed to add audio codec: 0x%08x\n", status);
        }
    }

    // Setup video track and transceiver
    MEMSET(&session->videoTrack, 0, SIZEOF(RtcMediaStreamTrack));
    session->videoTrack.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    session->videoTrack.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
    STRCPY(session->videoTrack.streamId, "whep-stream");
    STRCPY(session->videoTrack.trackId, "video0");

    RtcRtpTransceiverInit transceiverInit;
    MEMSET(&transceiverInit, 0, SIZEOF(RtcRtpTransceiverInit));
    transceiverInit.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;

    status = addTransceiver(session->pPeerConnection, &session->videoTrack, &transceiverInit, &session->pVideoTransceiver);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to add video transceiver: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to add video transceiver\"}", "application/json");
        return;
    }

    // Setup audio track and transceiver (if enabled)
    if (session->enableAudio) {
        MEMSET(&session->audioTrack, 0, SIZEOF(RtcMediaStreamTrack));
        session->audioTrack.codec = RTC_CODEC_OPUS;
        session->audioTrack.kind = MEDIA_STREAM_TRACK_KIND_AUDIO;
        STRCPY(session->audioTrack.streamId, "whep-stream");
        STRCPY(session->audioTrack.trackId, "audio0");

        RtcRtpTransceiverInit audioTransceiverInit;
        MEMSET(&audioTransceiverInit, 0, SIZEOF(RtcRtpTransceiverInit));
        audioTransceiverInit.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;

        status = addTransceiver(session->pPeerConnection, &session->audioTrack, &audioTransceiverInit, &session->pAudioTransceiver);
        if (STATUS_FAILED(status)) {
            printf("[WHEP] Failed to add audio transceiver: 0x%08x\n", status);
            res.status = 500;
            res.set_content("{\"error\": \"Failed to add audio transceiver\"}", "application/json");
            return;
        }
    }

    // Set remote description (offer)
    status = setRemoteDescription(session->pPeerConnection, &offerSdp);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to set remote description: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to set remote description\"}", "application/json");
        return;
    }

    // Create and set local description (triggers ICE gathering)
    MEMSET(&session->answerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
    status = setLocalDescription(session->pPeerConnection, &session->answerSdp);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to set local description: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to set local description\"}", "application/json");
        return;
    }

    // Wait for ICE gathering to complete (with timeout)
    printf("[WHEP] Waiting for ICE gathering...\n");
    UINT64 timeout = GETTIME() + (10 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    while (!session->iceGatheringDone.load() && GETTIME() < timeout) {
        THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    if (!session->iceGatheringDone.load()) {
        printf("[WHEP] ICE gathering timeout\n");
        res.status = 504;
        res.set_content("{\"error\": \"ICE gathering timeout\"}", "application/json");
        return;
    }

    // Create final answer with gathered candidates
    status = createAnswer(session->pPeerConnection, &session->answerSdp);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to create answer: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to create answer\"}", "application/json");
        return;
    }

    // Serialize answer to JSON
    CHAR answerJson[MAX_SESSION_DESCRIPTION_INIT_SDP_LEN + 256];
    UINT32 jsonLen = SIZEOF(answerJson);
    status = serializeSessionDescriptionInit(&session->answerSdp, answerJson, &jsonLen);
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to serialize answer: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to serialize answer\"}", "application/json");
        return;
    }

    printf("[WHEP] Sending answer (%u bytes)\n", jsonLen);
    res.set_content(answerJson, "application/json");
}

// Read file content helper
static std::string readFileContent(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Setup HTTP routes
static void setupRoutes(httplib::Server& svr, WhepSession* session)
{
    // Serve index.html
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::string content = readFileContent("./examples/index.html");
        if (content.empty()) {
            res.status = 404;
            res.set_content("File not found", "text/plain");
        } else {
            res.set_content(content, "text/html");
        }
    });

    // Serve client.js
    svr.Get("/client.js", [](const httplib::Request&, httplib::Response& res) {
        std::string content = readFileContent("./examples/client.js");
        if (content.empty()) {
            res.status = 404;
            res.set_content("File not found", "text/plain");
        } else {
            res.set_content(content, "application/javascript");
        }
    });

    // Return ICE server configuration
    svr.Get("/ice-servers", [session](const httplib::Request&, httplib::Response& res) {
        CHAR json[512];
        SNPRINTF(json, SIZEOF(json), "[{\"urls\": \"%s\"}]", session->stunServer);
        res.set_content(json, "application/json");
    });

    // Handle WebRTC offer
    svr.Post("/offer", [session](const httplib::Request& req, httplib::Response& res) { handleOffer(session, req.body, res); });
}

// Initialize session
static void initSession(WhepSession* session, const char* stunServer, bool enableAudio)
{
    MEMSET(session, 0, SIZEOF(WhepSession));

    // Configure ICE
    session->rtcConfig.kvsRtcConfiguration.iceLocalCandidateGatheringTimeout = 500 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    session->rtcConfig.kvsRtcConfiguration.iceCandidateNominationTimeout = 10 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    session->rtcConfig.kvsRtcConfiguration.iceConnectionCheckTimeout = 10 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    session->rtcConfig.kvsRtcConfiguration.useRedForOpus = TRUE;
    if (STRLEN(stunServer) > 0) {
        STRNCPY(session->rtcConfig.iceServers[0].urls, stunServer, MAX_ICE_CONFIG_URI_LEN);
        STRNCPY(session->stunServer, stunServer, SIZEOF(session->stunServer) - 1);
    }

    // Audio config
    session->enableAudio = enableAudio;

    // Initialize atomics
    session->iceGatheringDone.store(false);
    session->shouldExit.store(false);
}

// Cleanup session
static void cleanupSession(WhepSession* session)
{
    session->shouldExit.store(true);

    // Wait for video thread to stop
    if (session->videoThread.joinable()) {
        session->videoThread.join();
    }

    // Wait for audio thread to stop
    if (session->audioThread.joinable()) {
        session->audioThread.join();
    }

    if (session->pPeerConnection != NULL) {
        freePeerConnection(&session->pPeerConnection);
    }

    if (session->pVideoFrameBuffer != NULL) {
        MEMFREE(session->pVideoFrameBuffer);
        session->pVideoFrameBuffer = NULL;
    }

    if (session->pAudioFrameBuffer != NULL) {
        MEMFREE(session->pAudioFrameBuffer);
        session->pAudioFrameBuffer = NULL;
    }
}

int main(int argc, char* argv[])
{
    int port = 8080;
    // const char* stunServer = "stun:stun.l.google.com:19302";
    const char* stunServer = "";
    bool enableAudio = false;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc) {
            stunServer = argv[++i];
        } else if (strcmp(argv[i], "--audio") == 0) {
            enableAudio = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--port PORT] [--stun STUN_URL] [--audio]\n", argv[0]);
            printf("  --port PORT     HTTP server port (default: 8080)\n");
            printf("  --stun STUN_URL STUN server URL (default: stun:stun.l.google.com:19302)\n");
            printf("  --audio         Enable audio streaming (default: disabled)\n");
            return 0;
        }
    }

    // Initialize KVS WebRTC SDK
    printf("[WHEP] Initializing WebRTC SDK...\n");
    STATUS status = initKvsWebRtc();
    if (STATUS_FAILED(status)) {
        printf("[WHEP] Failed to initialize WebRTC SDK: 0x%08x\n", status);
        return 1;
    }

    // Initialize session
    initSession(&g_session, stunServer, enableAudio);

    // Create HTTP server
    httplib::Server svr;
    g_session.pServer = &svr;

    // Setup routes
    setupRoutes(svr, &g_session);

    // Start listening
    printf("[WHEP] WHEP Server listening on http://0.0.0.0:%d\n", port);
    printf("[WHEP] Using STUN server: %s\n", stunServer);
    printf("[WHEP] Audio streaming: %s\n", enableAudio ? "enabled" : "disabled");
    printf("[WHEP] Open browser to http://localhost:%d and click Start\n", port);

    svr.listen("0.0.0.0", port);

    // Cleanup
    printf("[WHEP] Shutting down...\n");
    cleanupSession(&g_session);
    deinitKvsWebRtc();

    printf("[WHEP] Done\n");
    return 0;
}
