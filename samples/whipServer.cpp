/**
 * Simple WHIP Server - HTTP-based WebRTC ingestion server (RFC 9725)
 * Receives H264 video from browser clients using WebRTC
 */

#include "httplib.h"

extern "C" {
#include "SamplesCommon.h"
}

#include <atomic>
#include <string>
#include <fstream>
#include <sstream>
#include <random>

// Session state for single connection
struct WhipSession {
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
    httplib::Server* pServer;
    char stunServer[256];
    bool enableAudio;
    std::string sessionId;
    UINT64 totalVideoFrames;
    UINT64 totalAudioFrames;
    // Bitrate tracking
    UINT64 videoBytesReceived;
    UINT64 audioBytesReceived;
    UINT64 lastBitrateReportTime;
    UINT64 lastVideoBytesReceived;
    UINT64 lastAudioBytesReceived;
};

static WhipSession g_session;

static const char* connectionStateToString(RTC_PEER_CONNECTION_STATE state) {
    switch (state) {
        case RTC_PEER_CONNECTION_STATE_NONE: return "NONE";
        case RTC_PEER_CONNECTION_STATE_NEW: return "NEW";
        case RTC_PEER_CONNECTION_STATE_CONNECTING: return "CONNECTING";
        case RTC_PEER_CONNECTION_STATE_CONNECTED: return "CONNECTED";
        case RTC_PEER_CONNECTION_STATE_DISCONNECTED: return "DISCONNECTED";
        case RTC_PEER_CONNECTION_STATE_FAILED: return "FAILED";
        case RTC_PEER_CONNECTION_STATE_CLOSED: return "CLOSED";
        default: return "UNKNOWN";
    }
}

// Generate a random session ID
static std::string generateSessionId() {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    std::string result;
    for (int i = 0; i < 16; i++) {
        result += charset[dis(gen)];
    }
    return result;
}

// Report received bitrate (called periodically)
static void reportReceivedBitrate(WhipSession* session, UINT64 now) {
    UINT64 elapsed = now - session->lastBitrateReportTime;
    if (elapsed >= HUNDREDS_OF_NANOS_IN_A_SECOND) {
        UINT64 videoBytesDelta = session->videoBytesReceived - session->lastVideoBytesReceived;
        UINT64 audioBytesDelta = session->audioBytesReceived - session->lastAudioBytesReceived;

        DOUBLE elapsedSec = (DOUBLE)elapsed / HUNDREDS_OF_NANOS_IN_A_SECOND;
        DOUBLE videoBitrate = (DOUBLE)(videoBytesDelta * 8) / elapsedSec;
        DOUBLE audioBitrate = (DOUBLE)(audioBytesDelta * 8) / elapsedSec;
        DOUBLE totalBitrate = videoBitrate + audioBitrate;

        printf("[WHIP] Received bitrate: %.2f kbps (video: %.2f kbps, audio: %.2f kbps)\n",
               totalBitrate / 1000.0, videoBitrate / 1000.0, audioBitrate / 1000.0);

        session->lastBitrateReportTime = now;
        session->lastVideoBytesReceived = session->videoBytesReceived;
        session->lastAudioBytesReceived = session->audioBytesReceived;
    }
}

// Video frame callback - called when video frame received from browser
static VOID onVideoFrame(UINT64 customData, PFrame pFrame) {
    WhipSession* session = (WhipSession*)customData;
    session->totalVideoFrames++;
    session->videoBytesReceived += pFrame->size;

    UINT64 now = GETTIME();
    reportReceivedBitrate(session, now);

    // printf("[WHIP] Video frame #%llu PTS: %llu, size: %u, flags: %u\n",
    //        (unsigned long long)session->totalVideoFrames,
    //        (unsigned long long)pFrame->presentationTs,
    //        pFrame->size,
    //        pFrame->flags);
}

// Audio frame callback - called when audio frame received from browser
static VOID onAudioFrame(UINT64 customData, PFrame pFrame) {
    WhipSession* session = (WhipSession*)customData;
    session->totalAudioFrames++;
    session->audioBytesReceived += pFrame->size;

    UINT64 now = GETTIME();
    reportReceivedBitrate(session, now);

    // printf("[WHIP] Audio frame #%llu PTS: %llu, size: %u\n",
    //        (unsigned long long)session->totalAudioFrames,
    //        (unsigned long long)pFrame->presentationTs,
    //        pFrame->size);
}

// ICE candidate callback
static VOID onIceCandidateCallback(UINT64 customData, PCHAR candidateJson) {
    WhipSession* session = (WhipSession*)customData;

    if (candidateJson == NULL) {
        printf("[WHIP] ICE gathering complete\n");
        session->iceGatheringDone.store(true);
    } else {
        printf("[WHIP] ICE candidate: %.80s...\n", candidateJson);
    }
}

// Connection state change callback
static VOID onConnectionStateChangeCallback(UINT64 customData, RTC_PEER_CONNECTION_STATE newState) {
    WhipSession* session = (WhipSession*)customData;

    printf("[WHIP] Connection state: %s\n", connectionStateToString(newState));
    session->connectionState = newState;

    switch (newState) {
        case RTC_PEER_CONNECTION_STATE_CONNECTED:
            printf("[WHIP] Ready to receive media from browser\n");
            // Initialize bitrate tracking
            session->lastBitrateReportTime = GETTIME();
            session->videoBytesReceived = 0;
            session->audioBytesReceived = 0;
            session->lastVideoBytesReceived = 0;
            session->lastAudioBytesReceived = 0;
            break;

        case RTC_PEER_CONNECTION_STATE_DISCONNECTED:
        case RTC_PEER_CONNECTION_STATE_FAILED:
        case RTC_PEER_CONNECTION_STATE_CLOSED:
            printf("[WHIP] Connection ended\n");
            printf("[WHIP] Total video frames received: %llu\n", (unsigned long long)session->totalVideoFrames);
            printf("[WHIP] Total audio frames received: %llu\n", (unsigned long long)session->totalAudioFrames);
            break;

        default:
            break;
    }
}

// Handle POST /whip/endpoint (RFC 9725 compliant)
static void handleWhipOffer(WhipSession* session, const httplib::Request& req, httplib::Response& res) {
    STATUS status = STATUS_SUCCESS;

    // Validate Content-Type per RFC 9725
    std::string contentType = req.get_header_value("Content-Type");
    if (contentType.find("application/sdp") == std::string::npos) {
        printf("[WHIP] Invalid Content-Type: %s (expected application/sdp)\n", contentType.c_str());
        res.status = 415;
        res.set_content("{\"error\": \"Content-Type must be application/sdp\"}", "application/json");
        return;
    }

    printf("[WHIP] Received SDP offer (%zu bytes)\n", req.body.length());

    // Check if already connected
    if (session->pPeerConnection != NULL) {
        res.status = 409;
        res.set_content("{\"error\": \"Already connected\"}", "application/json");
        return;
    }

    // Parse SDP offer (raw SDP, not JSON)
    RtcSessionDescriptionInit offerSdp;
    MEMSET(&offerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
    offerSdp.type = SDP_TYPE_OFFER;

    if (req.body.length() >= MAX_SESSION_DESCRIPTION_INIT_SDP_LEN) {
        res.status = 400;
        res.set_content("{\"error\": \"SDP too large\"}", "application/json");
        return;
    }
    STRNCPY(offerSdp.sdp, req.body.c_str(), MAX_SESSION_DESCRIPTION_INIT_SDP_LEN - 1);
    printf("%s", offerSdp.sdp);

    // Create peer connection
    status = createPeerConnection(&session->rtcConfig, &session->pPeerConnection);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to create peer connection: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to create peer connection\"}", "application/json");
        return;
    }

    // Setup callbacks
    status = peerConnectionOnIceCandidate(session->pPeerConnection, (UINT64)session, onIceCandidateCallback);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to set ICE callback: 0x%08x\n", status);
    }

    status = peerConnectionOnConnectionStateChange(session->pPeerConnection, (UINT64)session, onConnectionStateChangeCallback);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to set connection state callback: 0x%08x\n", status);
    }

    // Add H264 video codec support
    status = addSupportedCodec(session->pPeerConnection, RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to add video codec: 0x%08x\n", status);
    }

    // Add Opus audio codec if enabled
    if (session->enableAudio) {
        status = addSupportedCodec(session->pPeerConnection, RTC_CODEC_OPUS);
        if (STATUS_FAILED(status)) {
            printf("[WHIP] Failed to add audio codec: 0x%08x\n", status);
        }
    }

    // Setup VIDEO transceiver - RECVONLY (we receive from browser)
    MEMSET(&session->videoTrack, 0, SIZEOF(RtcMediaStreamTrack));
    session->videoTrack.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    session->videoTrack.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
    STRCPY(session->videoTrack.streamId, "whip-stream");
    STRCPY(session->videoTrack.trackId, "video0");

    RtcRtpTransceiverInit videoTransceiverInit;
    MEMSET(&videoTransceiverInit, 0, SIZEOF(RtcRtpTransceiverInit));
    videoTransceiverInit.direction = RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY;

    status = addTransceiver(session->pPeerConnection, &session->videoTrack, &videoTransceiverInit, &session->pVideoTransceiver);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to add video transceiver: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to add video transceiver\"}", "application/json");
        return;
    }

    // Register frame callback for video
    status = transceiverOnFrame(session->pVideoTransceiver, (UINT64)session, onVideoFrame);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to register video frame callback: 0x%08x\n", status);
    }

    // Setup AUDIO transceiver if enabled
    if (session->enableAudio) {
        MEMSET(&session->audioTrack, 0, SIZEOF(RtcMediaStreamTrack));
        session->audioTrack.codec = RTC_CODEC_OPUS;
        session->audioTrack.kind = MEDIA_STREAM_TRACK_KIND_AUDIO;
        STRCPY(session->audioTrack.streamId, "whip-stream");
        STRCPY(session->audioTrack.trackId, "audio0");

        RtcRtpTransceiverInit audioTransceiverInit;
        MEMSET(&audioTransceiverInit, 0, SIZEOF(RtcRtpTransceiverInit));
        audioTransceiverInit.direction = RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY;

        status = addTransceiver(session->pPeerConnection, &session->audioTrack, &audioTransceiverInit, &session->pAudioTransceiver);
        if (STATUS_FAILED(status)) {
            printf("[WHIP] Failed to add audio transceiver: 0x%08x\n", status);
            res.status = 500;
            res.set_content("{\"error\": \"Failed to add audio transceiver\"}", "application/json");
            return;
        }

        // Register frame callback for audio
        status = transceiverOnFrame(session->pAudioTransceiver, (UINT64)session, onAudioFrame);
        if (STATUS_FAILED(status)) {
            printf("[WHIP] Failed to register audio frame callback: 0x%08x\n", status);
        }
    }

    // Set remote description (the offer from browser)
    status = setRemoteDescription(session->pPeerConnection, &offerSdp);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to set remote description: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to set remote description\"}", "application/json");
        return;
    }

    // Create and set local description (answer)
    MEMSET(&session->answerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
    status = setLocalDescription(session->pPeerConnection, &session->answerSdp);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to set local description: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to set local description\"}", "application/json");
        return;
    }

    // Wait for ICE gathering to complete
    printf("[WHIP] Waiting for ICE gathering...\n");
    UINT64 timeout = GETTIME() + (10 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    while (!session->iceGatheringDone.load() && GETTIME() < timeout) {
        THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    if (!session->iceGatheringDone.load()) {
        printf("[WHIP] ICE gathering timeout\n");
        res.status = 504;
        res.set_content("{\"error\": \"ICE gathering timeout\"}", "application/json");
        return;
    }

    // Create final answer
    status = createAnswer(session->pPeerConnection, &session->answerSdp);
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to create answer: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to create answer\"}", "application/json");
        return;
    }

    // Generate unique session ID for WHIP session URL
    session->sessionId = generateSessionId();

    // RFC 9725: Return 201 Created with:
    // - Content-Type: application/sdp
    // - Location header pointing to WHIP session URL
    printf("[WHIP] Session created: %s\n", session->sessionId.c_str());
    printf("[WHIP] Sending SDP answer (%zu bytes)\n", strlen(session->answerSdp.sdp));
    printf("%s\n", session->answerSdp.sdp);

    res.status = 201;
    res.set_header("Location", "/whip/session/" + session->sessionId);
    res.set_header("ETag", "\"" + session->sessionId + "\"");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Expose-Headers", "Location, ETag");
    res.set_content(session->answerSdp.sdp, "application/sdp");
}

// Handle DELETE /whip/session/{id}
static void handleWhipDelete(WhipSession* session, const std::string& sessionId, httplib::Response& res) {
    if (session->sessionId != sessionId) {
        res.status = 404;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content("{\"error\": \"Session not found\"}", "application/json");
        return;
    }

    printf("[WHIP] Terminating session: %s\n", sessionId.c_str());

    // Signal shutdown
    session->shouldExit.store(true);

    // Close peer connection
    if (session->pPeerConnection != NULL) {
        freePeerConnection(&session->pPeerConnection);
        session->pPeerConnection = NULL;
    }

    printf("[WHIP] Total video frames received: %llu\n", (unsigned long long)session->totalVideoFrames);
    printf("[WHIP] Total audio frames received: %llu\n", (unsigned long long)session->totalAudioFrames);

    // Reset session for next connection
    session->sessionId.clear();
    session->iceGatheringDone.store(false);
    session->shouldExit.store(false);
    session->totalVideoFrames = 0;
    session->totalAudioFrames = 0;
    session->videoBytesReceived = 0;
    session->audioBytesReceived = 0;
    session->lastBitrateReportTime = 0;
    session->lastVideoBytesReceived = 0;
    session->lastAudioBytesReceived = 0;

    res.status = 200;
    res.set_header("Access-Control-Allow-Origin", "*");
}

// Read file content helper
static std::string readFileContent(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Setup HTTP routes
static void setupRoutes(httplib::Server& svr, WhipSession* session) {
    // Serve whipIndex.html at root
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::string content = readFileContent("./examples/whipIndex.html");
        if (content.empty()) {
            res.status = 404;
            res.set_content("   File not found", "text/plain");
        } else {
            res.set_content(content, "text/html");
        }
    });

    // Serve whipClient.js
    svr.Get("/whipClient.js", [](const httplib::Request&, httplib::Response& res) {
        std::string content = readFileContent("./examples/whipClient.js");
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
        if (strlen(session->stunServer) > 0) {
            SNPRINTF(json, SIZEOF(json), "[{\"urls\": \"%s\"}]", session->stunServer);
        } else {
            SNPRINTF(json, SIZEOF(json), "[]");
        }
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(json, "application/json");
    });

    // WHIP endpoint - POST /whip/endpoint (RFC 9725 Section 4.2)
    svr.Post("/whip/endpoint", [session](const httplib::Request& req, httplib::Response& res) {
        handleWhipOffer(session, req, res);
    });

    // WHIP session - DELETE /whip/session/{id} (RFC 9725 Section 3)
    svr.Delete(R"(/whip/session/([a-zA-Z0-9]+))", [session](const httplib::Request& req, httplib::Response& res) {
        std::string sessionId = req.matches[1];
        handleWhipDelete(session, sessionId, res);
    });

    // OPTIONS for CORS preflight (RFC 9725 Section 4.2)
    svr.Options("/whip/endpoint", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Accept-Post", "application/sdp");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 200;
    });

    svr.Options(R"(/whip/session/([a-zA-Z0-9]+))", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 200;
    });
}

// Initialize session
static void initSession(WhipSession* session, const char* stunServer, bool enableAudio) {
    MEMSET(session, 0, SIZEOF(WhipSession));

    // Configure ICE
    session->rtcConfig.kvsRtcConfiguration.iceLocalCandidateGatheringTimeout = 500 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    session->rtcConfig.kvsRtcConfiguration.iceCandidateNominationTimeout = 10 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    session->rtcConfig.kvsRtcConfiguration.iceConnectionCheckTimeout = 10 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    STRNCPY(session->rtcConfig.iceServers[0].urls, stunServer, MAX_ICE_CONFIG_URI_LEN);
    STRNCPY(session->stunServer, stunServer, SIZEOF(session->stunServer) - 1);

    // Audio config
    session->enableAudio = enableAudio;

    // Initialize atomics
    new (&session->iceGatheringDone) std::atomic<bool>(false);
    new (&session->shouldExit) std::atomic<bool>(false);
    new (&session->sessionId) std::string();
}

// Cleanup session
static void cleanupSession(WhipSession* session) {
    session->shouldExit.store(true);

    if (session->pPeerConnection != NULL) {
        freePeerConnection(&session->pPeerConnection);
    }

    session->sessionId.~basic_string();
    session->iceGatheringDone.~atomic();
    session->shouldExit.~atomic();
}

int main(int argc, char* argv[]) {
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
            printf("  --audio         Enable audio receiving (default: disabled)\n");
            return 0;
        }
    }

    // Initialize KVS WebRTC SDK
    printf("[WHIP] Initializing WebRTC SDK...\n");
    SET_LOGGER_LOG_LEVEL(LOG_LEVEL_INFO);
    STATUS status = initKvsWebRtc();
    if (STATUS_FAILED(status)) {
        printf("[WHIP] Failed to initialize WebRTC SDK: 0x%08x\n", status);
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
    printf("[WHIP] WHIP Server (RFC 9725) listening on http://0.0.0.0:%d\n", port);
    printf("[WHIP] Using STUN server: %s\n", stunServer);
    printf("[WHIP] Audio receiving: %s\n", enableAudio ? "enabled" : "disabled");
    printf("[WHIP] Open browser to http://localhost:%d and click Start Streaming\n", port);

    svr.listen("0.0.0.0", port);

    // Cleanup
    printf("[WHIP] Shutting down...\n");
    cleanupSession(&g_session);
    deinitKvsWebRtc();

    printf("[WHIP] Done\n");
    return 0;
}
