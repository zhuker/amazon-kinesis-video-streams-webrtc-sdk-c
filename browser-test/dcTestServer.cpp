/**
 * DataChannel Test Server - HTTP-based WebRTC server for browser interop testing
 * Pure data-channel server (no media). Used by Puppeteer test runner.
 */

#include "httplib.h"

extern "C" {
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
}

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>

#define MAX_TEST_CHANNELS   16
#define MAX_ECHO_BUFFER     (512 * 1024)

struct ChannelStats {
    std::string name;
    int messagesReceived;
    int messagesSent;
    int bytesReceived;
    bool opened;
};

struct TestSession {
    RtcConfiguration rtcConfig;
    PRtcPeerConnection pPeerConnection;
    RTC_PEER_CONNECTION_STATE connectionState;
    std::atomic<bool> iceGatheringDone;
    RtcSessionDescriptionInit answerSdp;

    // Server-created channels
    RtcDataChannel serverChannelStorage[MAX_TEST_CHANNELS];
    PRtcDataChannel serverChannels[MAX_TEST_CHANNELS];
    int serverChannelCount;

    // Current test name
    std::string currentTest;

    // Stats
    std::vector<ChannelStats> channelStats;
    std::mutex statsMutex;

    // Server ref for shutdown
    httplib::Server* pServer;

    // Static file directory
    std::string staticDir;

    // Port
    int port;
};

// ---------- Helpers ----------

static ChannelStats* findOrCreateStats(TestSession* session, const std::string& name) {
    std::lock_guard<std::mutex> lock(session->statsMutex);
    for (auto& s : session->channelStats) {
        if (s.name == name) return &s;
    }
    session->channelStats.push_back({name, 0, 0, 0, false});
    return &session->channelStats.back();
}

static std::string readFileContent(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string content(sz, '\0');
    fread(&content[0], 1, sz, f);
    fclose(f);
    return content;
}

// ---------- Callbacks ----------

static VOID onIceCandidate(UINT64 customData, PCHAR candidateJson) {
    TestSession* session = (TestSession*)customData;
    if (candidateJson == NULL) {
        printf("[DC-TEST] ICE gathering complete\n");
        session->iceGatheringDone.store(true);
    }
}

static VOID onConnectionStateChange(UINT64 customData, RTC_PEER_CONNECTION_STATE newState) {
    TestSession* session = (TestSession*)customData;
    printf("[DC-TEST] Connection state: %d\n", (int)newState);
    session->connectionState = newState;
}

// Echo message callback - echoes back whatever it receives
static VOID onDataChannelMessageEcho(UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMessage, UINT32 messageLen) {
    TestSession* session = (TestSession*)customData;
    ChannelStats* stats = findOrCreateStats(session, pDataChannel->name);
    stats->messagesReceived++;
    stats->bytesReceived += messageLen;

    // Handle empty messages: SDK requires non-NULL pMessage, so use dummy byte for 0-length
    BYTE dummy = 0;
    PBYTE sendPtr = (messageLen == 0) ? &dummy : pMessage;

    STATUS status = dataChannelSend(pDataChannel, isBinary, sendPtr, messageLen);
    if (STATUS_SUCCEEDED(status)) {
        stats->messagesSent++;
    } else {
        printf("[DC-TEST] Echo send failed: 0x%08x (len=%u)\n", status, messageLen);
    }
}

// Burst callback - server sends 50 numbered messages when browser channel opens
static VOID onDataChannelMessageBurst(UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMessage, UINT32 messageLen) {
    (void)isBinary;
    (void)pMessage;
    TestSession* session = (TestSession*)customData;
    ChannelStats* stats = findOrCreateStats(session, pDataChannel->name);
    stats->messagesReceived++;
    stats->bytesReceived += messageLen;

    // When we get the first message ("start-burst"), send 50 messages
    if (messageLen >= 5) {
        CHAR buf[64];
        for (int i = 0; i < 50; i++) {
            int len = snprintf(buf, sizeof(buf), "server-msg-%d", i);
            STATUS status = dataChannelSend(pDataChannel, FALSE, (PBYTE)buf, (UINT32)len);
            if (STATUS_SUCCEEDED(status)) {
                stats->messagesSent++;
            }
        }
    }
}

// Data channel open callback for server-created channels
static VOID onServerChannelOpen(UINT64 customData, PRtcDataChannel pDataChannel) {
    TestSession* session = (TestSession*)customData;
    printf("[DC-TEST] Server channel opened: '%s'\n", pDataChannel->name);

    ChannelStats* stats = findOrCreateStats(session, pDataChannel->name);
    stats->opened = true;

    if (session->currentTest == "server-creates-dc") {
        const char* msg = "hello from server";
        dataChannelSend(pDataChannel, FALSE, (PBYTE)msg, (UINT32)strlen(msg));
        stats->messagesSent++;
    } else if (session->currentTest == "server-sends-binary") {
        // Send 1024-byte pattern
        BYTE pattern[1024];
        for (int i = 0; i < 1024; i++) pattern[i] = (BYTE)(i % 256);
        dataChannelSend(pDataChannel, TRUE, pattern, 1024);
        stats->messagesSent++;
    } else if (session->currentTest == "burst") {
        // Send 50 numbered messages with small delays to allow SCTP flow control
        CHAR buf[64];
        for (int i = 0; i < 50; i++) {
            int len = snprintf(buf, sizeof(buf), "server-burst-%d", i);
            STATUS status = dataChannelSend(pDataChannel, FALSE, (PBYTE)buf, (UINT32)len);
            if (STATUS_SUCCEEDED(status)) {
                stats->messagesSent++;
            } else {
                printf("[DC-TEST] Burst send %d failed: 0x%08x\n", i, status);
            }
            // Small delay every 10 messages to allow SACKs to flow
            if (i > 0 && i % 10 == 0) {
                THREAD_SLEEP(10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
            }
        }
    }
}

// Callback for when browser creates a data channel (remote channel notification)
static VOID onDataChannel(UINT64 customData, PRtcDataChannel pDataChannel) {
    TestSession* session = (TestSession*)customData;
    printf("[DC-TEST] Remote DataChannel opened: '%s'\n", pDataChannel->name);

    ChannelStats* stats = findOrCreateStats(session, pDataChannel->name);
    stats->opened = true;

    // Always use echo for browser-created channels
    dataChannelOnMessage(pDataChannel, customData, onDataChannelMessageEcho);
}

// ---------- Test configuration ----------

static void configureForTest(TestSession* session, const std::string& testName) {
    session->currentTest = testName;
    session->serverChannelCount = 0;
    PRtcPeerConnection pc = session->pPeerConnection;

    auto createChannel = [&](const char* name, PRtcDataChannelInit pInit) {
        int idx = session->serverChannelCount;
        if (idx >= MAX_TEST_CHANNELS) return;
        PRtcDataChannel pChannel = NULL;
        STATUS status = createDataChannel(pc, (PCHAR)name, pInit, &pChannel);
        if (STATUS_SUCCEEDED(status)) {
            session->serverChannels[idx] = pChannel;
            dataChannelOnOpen(pChannel, (UINT64)session, onServerChannelOpen);
            dataChannelOnMessage(pChannel, (UINT64)session, onDataChannelMessageEcho);
            session->serverChannelCount++;
        } else {
            printf("[DC-TEST] createDataChannel '%s' failed: 0x%08x\n", name, status);
        }
    };

    if (testName == "server-creates-dc") {
        createChannel("server-channel", NULL);
    }
    else if (testName == "server-creates-unordered") {
        RtcDataChannelInit init;
        MEMSET(&init, 0, SIZEOF(RtcDataChannelInit));
        init.ordered = FALSE;
        NULLABLE_SET_EMPTY(init.maxPacketLifeTime);
        NULLABLE_SET_EMPTY(init.maxRetransmits);
        createChannel("unordered-srv", &init);
    }
    else if (testName == "server-creates-maxretransmits") {
        RtcDataChannelInit init;
        MEMSET(&init, 0, SIZEOF(RtcDataChannelInit));
        init.ordered = TRUE;
        NULLABLE_SET_VALUE(init.maxRetransmits, 3);
        NULLABLE_SET_EMPTY(init.maxPacketLifeTime);
        createChannel("maxretransmit-srv", &init);
    }
    else if (testName == "server-creates-maxlifetime") {
        RtcDataChannelInit init;
        MEMSET(&init, 0, SIZEOF(RtcDataChannelInit));
        init.ordered = TRUE;
        NULLABLE_SET_EMPTY(init.maxRetransmits);
        NULLABLE_SET_VALUE(init.maxPacketLifeTime, 1000);
        createChannel("maxlifetime-srv", &init);
    }
    else if (testName == "server-creates-multi") {
        for (int i = 0; i < 5; i++) {
            CHAR name[32];
            snprintf(name, sizeof(name), "srv-%d", i);
            createChannel(name, NULL);
        }
    }
    else if (testName == "bidirectional") {
        createChannel("server-ch", NULL);
    }
    else if (testName == "server-sends-binary") {
        createChannel("binary-srv", NULL);
    }
    else if (testName == "burst") {
        createChannel("burst-srv", NULL);
    }
    // else: echo (default) or large-echo - no server channels, just accept browser's
}

// ---------- HTTP handlers ----------

static void handleOffer(TestSession* session, const httplib::Request& req, httplib::Response& res) {
    // Parse test parameter
    std::string testName = "echo";
    if (req.has_param("test")) {
        testName = req.get_param_value("test");
    }

    printf("[DC-TEST] Received offer, test='%s' (%zu bytes)\n", testName.c_str(), req.body.length());

    // Auto-reset if a previous connection is still active (race between
    // browser disconnect and the next test's offer)
    if (session->pPeerConnection != NULL) {
        printf("[DC-TEST] Auto-resetting stale session before new offer\n");
        freePeerConnection(&session->pPeerConnection);
        session->pPeerConnection = NULL;
        session->iceGatheringDone.store(false);
        session->connectionState = RTC_PEER_CONNECTION_STATE_NONE;
        session->currentTest.clear();
        session->serverChannelCount = 0;
        {
            std::lock_guard<std::mutex> lock(session->statsMutex);
            session->channelStats.clear();
        }
    }

    // Parse SDP offer
    RtcSessionDescriptionInit offerSdp;
    MEMSET(&offerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
    offerSdp.type = SDP_TYPE_OFFER;

    CHAR* offerJson = (CHAR*)MEMALLOC(req.body.length() + 1);
    if (offerJson == NULL) {
        res.status = 500;
        res.set_content("{\"error\": \"Memory allocation failed\"}", "application/json");
        return;
    }
    MEMCPY(offerJson, req.body.c_str(), req.body.length());
    offerJson[req.body.length()] = '\0';

    STATUS status = deserializeSessionDescriptionInit(offerJson, (UINT32)req.body.length(), &offerSdp);
    MEMFREE(offerJson);
    if (STATUS_FAILED(status)) {
        printf("[DC-TEST] Failed to parse offer: 0x%08x\n", status);
        res.status = 400;
        res.set_content("{\"error\": \"Invalid SDP\"}", "application/json");
        return;
    }

    // Create peer connection
    status = createPeerConnection(&session->rtcConfig, &session->pPeerConnection);
    if (STATUS_FAILED(status)) {
        printf("[DC-TEST] createPeerConnection failed: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to create peer connection\"}", "application/json");
        return;
    }

    // Setup callbacks
    peerConnectionOnIceCandidate(session->pPeerConnection, (UINT64)session, onIceCandidate);
    peerConnectionOnConnectionStateChange(session->pPeerConnection, (UINT64)session, onConnectionStateChange);
    peerConnectionOnDataChannel(session->pPeerConnection, (UINT64)session, onDataChannel);

    // Configure test-specific channels BEFORE signaling
    configureForTest(session, testName);

    // Set remote description (offer)
    status = setRemoteDescription(session->pPeerConnection, &offerSdp);
    if (STATUS_FAILED(status)) {
        printf("[DC-TEST] setRemoteDescription failed: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to set remote description\"}", "application/json");
        freePeerConnection(&session->pPeerConnection);
        return;
    }

    // Create answer
    MEMSET(&session->answerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
    status = setLocalDescription(session->pPeerConnection, &session->answerSdp);
    if (STATUS_FAILED(status)) {
        printf("[DC-TEST] setLocalDescription failed: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to set local description\"}", "application/json");
        freePeerConnection(&session->pPeerConnection);
        return;
    }

    // Wait for ICE gathering
    printf("[DC-TEST] Waiting for ICE gathering...\n");
    UINT64 timeout = GETTIME() + (10 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    while (!session->iceGatheringDone.load() && GETTIME() < timeout) {
        THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    if (!session->iceGatheringDone.load()) {
        printf("[DC-TEST] ICE gathering timeout\n");
        res.status = 504;
        res.set_content("{\"error\": \"ICE gathering timeout\"}", "application/json");
        freePeerConnection(&session->pPeerConnection);
        return;
    }

    // Create final answer
    status = createAnswer(session->pPeerConnection, &session->answerSdp);
    if (STATUS_FAILED(status)) {
        printf("[DC-TEST] createAnswer failed: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to create answer\"}", "application/json");
        freePeerConnection(&session->pPeerConnection);
        return;
    }

    // Serialize answer
    CHAR answerJson[MAX_SESSION_DESCRIPTION_INIT_SDP_LEN + 256];
    UINT32 jsonLen = SIZEOF(answerJson);
    status = serializeSessionDescriptionInit(&session->answerSdp, answerJson, &jsonLen);
    if (STATUS_FAILED(status)) {
        printf("[DC-TEST] serializeSessionDescriptionInit failed: 0x%08x\n", status);
        res.status = 500;
        res.set_content("{\"error\": \"Failed to serialize answer\"}", "application/json");
        freePeerConnection(&session->pPeerConnection);
        return;
    }

    printf("[DC-TEST] Sending answer (%u bytes)\n", jsonLen);
    res.set_content(answerJson, "application/json");
}

static void handleReset(TestSession* session, httplib::Response& res) {
    printf("[DC-TEST] Resetting session\n");

    if (session->pPeerConnection != NULL) {
        freePeerConnection(&session->pPeerConnection);
        session->pPeerConnection = NULL;
    }

    session->iceGatheringDone.store(false);
    session->connectionState = RTC_PEER_CONNECTION_STATE_NONE;
    session->currentTest.clear();
    session->serverChannelCount = 0;
    {
        std::lock_guard<std::mutex> lock(session->statsMutex);
        session->channelStats.clear();
    }

    res.set_content("{\"status\": \"ok\"}", "application/json");
}

static void handleResults(TestSession* session, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(session->statsMutex);

    std::string json = "{\"test\": \"" + session->currentTest + "\", \"channels\": [";
    for (size_t i = 0; i < session->channelStats.size(); i++) {
        auto& s = session->channelStats[i];
        if (i > 0) json += ",";
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"name\": \"%s\", \"messagesReceived\": %d, \"messagesSent\": %d, \"bytesReceived\": %d, \"opened\": %s}",
            s.name.c_str(), s.messagesReceived, s.messagesSent, s.bytesReceived,
            s.opened ? "true" : "false");
        json += buf;
    }
    json += "]}";
    res.set_content(json, "application/json");
}

static void setupRoutes(httplib::Server& svr, TestSession* session) {
    svr.Get("/", [session](const httplib::Request&, httplib::Response& res) {
        std::string content = readFileContent(session->staticDir + "/dc-test.html");
        if (content.empty()) {
            res.status = 404;
            res.set_content("dc-test.html not found", "text/plain");
        } else {
            res.set_content(content, "text/html");
        }
    });

    svr.Get("/dc-test.js", [session](const httplib::Request&, httplib::Response& res) {
        std::string content = readFileContent(session->staticDir + "/dc-test.js");
        if (content.empty()) {
            res.status = 404;
            res.set_content("dc-test.js not found", "text/plain");
        } else {
            res.set_content(content, "application/javascript");
        }
    });

    svr.Post("/offer", [session](const httplib::Request& req, httplib::Response& res) {
        handleOffer(session, req, res);
    });

    svr.Post("/reset", [session](const httplib::Request&, httplib::Response& res) {
        handleReset(session, res);
    });

    svr.Get("/results", [session](const httplib::Request&, httplib::Response& res) {
        handleResults(session, res);
    });
}

int main(int argc, char* argv[]) {
    int port = 9876;
    std::string staticDir = ".";

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--static-dir") == 0 && i + 1 < argc) {
            staticDir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--port PORT] [--static-dir DIR]\n", argv[0]);
            printf("  --port PORT         HTTP server port (default: 9876)\n");
            printf("  --static-dir DIR    Directory containing dc-test.html/js (default: .)\n");
            return 0;
        }
    }

    // Initialize SDK
    printf("[DC-TEST] Initializing WebRTC SDK...\n");
    STATUS status = initKvsWebRtc();
    if (STATUS_FAILED(status)) {
        printf("[DC-TEST] initKvsWebRtc failed: 0x%08x\n", status);
        return 1;
    }

    // Initialize session
    TestSession session = {};
    session.pPeerConnection = NULL;
    session.connectionState = RTC_PEER_CONNECTION_STATE_NONE;
    session.iceGatheringDone.store(false);
    session.serverChannelCount = 0;
    session.staticDir = staticDir;
    session.port = port;

    // ICE config - localhost only, short timeout
    MEMSET(&session.rtcConfig, 0, SIZEOF(RtcConfiguration));
    session.rtcConfig.kvsRtcConfiguration.iceLocalCandidateGatheringTimeout = 500 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    session.rtcConfig.kvsRtcConfiguration.iceCandidateNominationTimeout = 10 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    session.rtcConfig.kvsRtcConfiguration.iceConnectionCheckTimeout = 10 * HUNDREDS_OF_NANOS_IN_A_SECOND;

    // Create HTTP server
    httplib::Server svr;
    session.pServer = &svr;

    setupRoutes(svr, &session);

    printf("[DC-TEST] Server listening on http://127.0.0.1:%d\n", port);
    printf("[DC-TEST] Static files from: %s\n", staticDir.c_str());
    svr.listen("0.0.0.0", port);

    // Cleanup
    printf("[DC-TEST] Shutting down...\n");
    if (session.pPeerConnection != NULL) {
        freePeerConnection(&session.pPeerConnection);
    }
    deinitKvsWebRtc();

    printf("[DC-TEST] Done\n");
    return 0;
}
