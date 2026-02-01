#include "WebRTCClientTestFixture.h"

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

//
// Global memory allocation counter
//
UINT64 gTotalWebRtcClientMemoryUsage = 0;

//
// Global memory counter lock
//
MUTEX gTotalWebRtcClientMemoryMutex;

// Forward declaration
static void my_set_threadname(const char *name);

STATUS createRtpPacketWithSeqNum(UINT16 seqNum, PRtpPacket* ppRtpPacket)
{
    STATUS retStatus = STATUS_SUCCESS;
    BYTE payload[10];
    PRtpPacket pRtpPacket = NULL;

    CHK_STATUS(createRtpPacket(2, FALSE, FALSE, 0, FALSE, 96, seqNum, 100, 0x1234ABCD, NULL, 0, 0, NULL, payload, 10, &pRtpPacket));
    *ppRtpPacket = pRtpPacket;

    CHK_STATUS(createBytesFromRtpPacket(pRtpPacket, NULL, &pRtpPacket->rawPacketLength));
    CHK(NULL != (pRtpPacket->pRawPacket = (PBYTE) MEMALLOC(pRtpPacket->rawPacketLength)), STATUS_NOT_ENOUGH_MEMORY);
    CHK_STATUS(createBytesFromRtpPacket(pRtpPacket, pRtpPacket->pRawPacket, &pRtpPacket->rawPacketLength));

CleanUp:
    return retStatus;
}

WebRtcClientTestBase::WebRtcClientTestBase()
    : mSignalingClientHandle(INVALID_SIGNALING_CLIENT_HANDLE_VALUE), mAccessKey(NULL), mSecretKey(NULL), mSessionToken(NULL), mRegion(NULL),
      mCaCertPath(NULL), mAccessKeyIdSet(FALSE)
{
    // Initialize the endianness of the library
    initializeEndianness();

    SRAND(12345);

    mStreamingRotationPeriod = TEST_STREAMING_TOKEN_DURATION;
}

static MUTEX noopCreateMutex(BOOL) {
    return 42;
}

static VOID noopLockMutex(MUTEX) {
}

static VOID noopUnlockMutex(MUTEX) {
}

static BOOL noopTryLockMutex(MUTEX) {
    return TRUE;
}

static VOID noopFreeMutex(MUTEX) {
}

// Thread-aware logging for debugging threading issues
static UINT64 getThreadId() {
    return (UINT64)GETTID();
}

static const char* getThreadName() {
    static thread_local char threadName[32] = {0};
    if (threadName[0] == '\0') {
        pthread_getname_np(pthread_self(), threadName, sizeof(threadName));
        if (threadName[0] == '\0') {
            snprintf(threadName, sizeof(threadName), "tid-%lu", getThreadId());
        }
    }
    return threadName;
}
static uint32_t startTimeForLogging = 0;
static VOID myLogPrint(UINT32 level, PCHAR tag, PCHAR fmt, ...)
{
    CHAR logFmtString[MAX_LOG_FORMAT_LENGTH + 1];
    CHAR finalLogStrBuffer[4096];
    UINT32 logLevel = GET_LOGGER_LOG_LEVEL();

    if (level >= logLevel) {
        uint32_t elapsedMsec = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - startTimeForLogging;

        // Add timestamp, thread info, level, and tag
        SNPRINTF(logFmtString, ARRAY_SIZE(logFmtString), "%u [%s] %s %-5s %s%s\n",
                    elapsedMsec,
                 getThreadName(),
                 tag,
                 level == LOG_LEVEL_VERBOSE ? "VERB" :
                 level == LOG_LEVEL_DEBUG ? "DEBUG" :
                 level == LOG_LEVEL_INFO ? "INFO" :
                 level == LOG_LEVEL_WARN ? "WARN" :
                 level == LOG_LEVEL_ERROR ? "ERROR" :
                 level == LOG_LEVEL_FATAL ? "FATAL" :
                 level == LOG_LEVEL_SILENT ? "SILENT" :
                 level == LOG_LEVEL_PROFILE ? "PROFILE" : "?????",
                 fmt,
                 EMPTY_STRING);

        va_list valist;
        va_start(valist, fmt);
        vsnprintf(finalLogStrBuffer, ARRAY_SIZE(finalLogStrBuffer), logFmtString, valist);
        va_end(valist);

        fputs(finalLogStrBuffer, stdout);
        fflush(stdout);
    }
}

void WebRtcClientTestBase::SetUp()
{
    globalCreateThread = crashingThreadCreate;
    globalJoinThread = crashingThreadJoin;
    globalCreateMutex = noopCreateMutex;
    globalLockMutex = noopLockMutex;
    globalUnlockMutex = noopUnlockMutex;
    globalTryLockMutex = noopTryLockMutex;
    globalFreeMutex = noopFreeMutex;

    startTimeForLogging = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    globalCustomLogPrintFn = myLogPrint;
    mLogLevel = LOG_LEVEL_DEBUG;  // Initialize log level first
    SET_LOGGER_LOG_LEVEL(mLogLevel);
    DLOGI("Custom thread-aware log function installed");
    DLOGI("\nSetting up test: %s\n", GetTestName());
    mReadyFrameIndex = 0;
    mDroppedFrameIndex = 0;
    mExpectedFrameCount = 0;
    mExpectedDroppedFrameCount = 0;
    noNewThreads = FALSE;

    SET_INSTRUMENTED_ALLOCATORS();

    PCHAR logLevelStr = GETENV(DEBUG_LOG_LEVEL_ENV_VAR);
    if (logLevelStr != NULL) {
        ASSERT_EQ(STATUS_SUCCESS, STRTOUI32(logLevelStr, NULL, 10, &mLogLevel));
    }

    if (STATUS_SUCCESS != initKvsWebRtc()) {
        DLOGE("Test initKvsWebRtc FAILED!!!!");
    }

    if (NULL != (mAccessKey = getenv(ACCESS_KEY_ENV_VAR))) {
        mAccessKeyIdSet = TRUE;
    }

    mSecretKey = getenv(SECRET_KEY_ENV_VAR);
    mSessionToken = getenv(SESSION_TOKEN_ENV_VAR);

    if (NULL == (mRegion = getenv(DEFAULT_REGION_ENV_VAR))) {
        mRegion = TEST_DEFAULT_REGION;
    }

    if (NULL == (mCaCertPath = getenv(CACERT_PATH_ENV_VAR))) {
        mCaCertPath = (PCHAR) DEFAULT_KVS_CACERT_PATH;
    }

    if (mAccessKey) {
        ASSERT_EQ(STATUS_SUCCESS,
                  createStaticCredentialProvider(mAccessKey, 0, mSecretKey, 0, mSessionToken, 0, MAX_UINT64, &mTestCredentialProvider));
    } else {
        mTestCredentialProvider = nullptr;
    }

    // Prepare the test channel name by prefixing with test channel name
    // and generating random chars replacing a potentially bad characters with '.'
    STRCPY(mChannelName, TEST_SIGNALING_CHANNEL_NAME);
    UINT32 testNameLen = STRLEN(TEST_SIGNALING_CHANNEL_NAME);
    const UINT32 randSize = 16;

    PCHAR pCur = &mChannelName[testNameLen];

    for (UINT32 i = 0; i < randSize; i++) {
        *pCur++ = SIGNALING_VALID_NAME_CHARS[RAND() % (ARRAY_SIZE(SIGNALING_VALID_NAME_CHARS) - 1)];
    }

    *pCur = '\0';

#ifdef USE_LIBUV
    // Start UV loop for all tests
    uvAsyncJobReady.data = this;
    uv_async_init(uv_default_loop(), &uvAsyncJobReady, uvAsyncJobReadyCb);
    uvLoopRunning.store(true);
    uvlooper = std::thread([this]() {
        my_set_threadname("uv");
        uvLoopThreadId.store((uint64_t)GETTID());
        DLOGI("Starting uv_run on loop %p, alive=%d, tid=%lu", uv_default_loop(), uv_loop_alive(uv_default_loop()), uvLoopThreadId.load());
        int ret = uv_run(uv_default_loop(), UV_RUN_DEFAULT);
        DLOGI("uv_run returned %d", ret);
        uvLoopRunning.store(false);
    });
#endif
}

void WebRtcClientTestBase::TearDown()
{
    DLOGI("\nTearing down test: %s\n", GetTestName());
#ifdef USE_LIBUV
    // Close the async handle to allow the UV loop to exit
    if (uvlooper.joinable() && uvLoopRunning.load()) {
        // Schedule closing of the async handle from the UV loop thread
        // Don't use uv_stop() as it sets a flag that persists and affects next test
        runOnLoop([this]() {
            uv_close((uv_handle_t*)&uvAsyncJobReady, nullptr);
        });
        uvlooper.join();
    } else if (uvlooper.joinable()) {
        uvlooper.join();
    }
    uvLoopRunning.store(false);

    // Reset the default loop for the next test
    // Close the loop (releases resources) and reinitialize it
    uv_loop_t* loop = uv_default_loop();
    int closeResult = uv_loop_close(loop);
    if (closeResult != 0) {
        DLOGW("uv_loop_close returned %d - draining remaining handles", closeResult);
        // Run the loop until all handles are closed (close callbacks need to fire)
        uv_run(loop, UV_RUN_DEFAULT);
        closeResult = uv_loop_close(loop);
        if (closeResult != 0) {
            DLOGE("Failed to close loop after draining: %d", closeResult);
        }
    }
    // Reinitialize the default loop for the next test
    uv_loop_init(loop);
#else
    if (uvlooper.joinable()) {
        uvlooper.join();
    }
#endif

    deinitKvsWebRtc();
    // Need this sleep for threads in threadpool to close
    THREAD_SLEEP(400 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);

    freeStaticCredentialProvider(&mTestCredentialProvider);

    EXPECT_EQ(STATUS_SUCCESS, RESET_INSTRUMENTED_ALLOCATORS());
}

#ifdef USE_LIBUV
void WebRtcClientTestBase::uvAsyncJobReadyCb(uv_async_t* handle)
{
    auto* self = static_cast<WebRtcClientTestBase*>(handle->data);
    std::function<void()> job;
    while (true) {
        {
            std::lock_guard<std::mutex> guard(self->uvJobQueueMutex);
            if (self->uvJobQueue.empty()) {
                break;
            }
            job = std::move(self->uvJobQueue.front());
            self->uvJobQueue.pop();
        }
        job();
    }
}

void WebRtcClientTestBase::runOnLoop(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> guard(uvJobQueueMutex);
        uvJobQueue.push(std::move(job));
    }
    uv_async_send(&uvAsyncJobReady);
}
#endif

VOID WebRtcClientTestBase::initializeJitterBuffer(UINT32 expectedFrameCount, UINT32 expectedDroppedFrameCount, UINT32 rtpPacketCount)
{
    UINT32 i, timestamp;
    EXPECT_EQ(STATUS_SUCCESS,
              createJitterBuffer(testFrameReadyFunc, testFrameDroppedFunc, testDepayRtpFunc, DEFAULT_JITTER_BUFFER_MAX_LATENCY,
                                 TEST_JITTER_BUFFER_CLOCK_RATE, (UINT64) this, &mJitterBuffer));
    mExpectedFrameCount = expectedFrameCount;
    mFrame = NULL;
    if (expectedFrameCount > 0) {
        mPExpectedFrameArr = (PBYTE*) MEMALLOC(SIZEOF(PBYTE) * expectedFrameCount);
        mExpectedFrameSizeArr = (PUINT32) MEMALLOC(SIZEOF(UINT32) * expectedFrameCount);
    }
    mExpectedDroppedFrameCount = expectedDroppedFrameCount;
    if (expectedDroppedFrameCount > 0) {
        mExpectedDroppedFrameTimestampArr = (PUINT32) MEMALLOC(SIZEOF(UINT32) * expectedDroppedFrameCount);
    }

    mPRtpPackets = (PRtpPacket*) MEMALLOC(SIZEOF(PRtpPacket) * rtpPacketCount);
    mRtpPacketCount = rtpPacketCount;

    // Assume timestamp is on time unit ms for test
    for (i = 0, timestamp = 0; i < rtpPacketCount; i++, timestamp += 200) {
        EXPECT_EQ(STATUS_SUCCESS,
                  createRtpPacket(2, FALSE, FALSE, 0, FALSE, 96, i, timestamp, 0x1234ABCD, NULL, 0, 0, NULL, NULL, 0, mPRtpPackets + i));
    }
}

VOID WebRtcClientTestBase::setPayloadToFree()
{
    UINT32 i;
    for (i = 0; i < mRtpPacketCount; i++) {
        mPRtpPackets[i]->pRawPacket = mPRtpPackets[i]->payload;
    }
}
static void my_set_threadname(const char *name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#endif
#if defined(__linux__)
    prctl(PR_SET_NAME, name);
#endif
#if defined(__FreeBSD__)
    pthread_set_name_np(pthread_self(), name);
#endif
}

VOID WebRtcClientTestBase::clearJitterBufferForTest()
{
    UINT32 i;
    EXPECT_EQ(STATUS_SUCCESS, freeJitterBuffer(&mJitterBuffer));
    if (mExpectedFrameCount > 0) {
        for (i = 0; i < mExpectedFrameCount; i++) {
            MEMFREE(mPExpectedFrameArr[i]);
        }
        MEMFREE(mPExpectedFrameArr);
        MEMFREE(mExpectedFrameSizeArr);
    }
    if (mExpectedDroppedFrameCount > 0) {
        MEMFREE(mExpectedDroppedFrameTimestampArr);
    }
    MEMFREE(mPRtpPackets);
    EXPECT_EQ(mExpectedFrameCount, mReadyFrameIndex);
    EXPECT_EQ(mExpectedDroppedFrameCount, mDroppedFrameIndex);
    if (mFrame != NULL) {
        MEMFREE(mFrame);
    }
}

// Connect two RtcPeerConnections, and wait for them to be connected
// in the given amount of time. Return false if they don't go to connected in
// the expected amounted of time
bool WebRtcClientTestBase::connectTwoPeers(PRtcPeerConnection offerPc, PRtcPeerConnection answerPc, PCHAR pOfferCertFingerprint,
                                           PCHAR pAnswerCertFingerprint)
{
    RtcSessionDescriptionInit sdp;
    PeerContainer offer;
    PeerContainer answer;
    this->noNewThreads = FALSE;

    auto onICECandidateHdlr = [this](UINT64 customData, PCHAR candidateStr) -> void {
        PPeerContainer container = (PPeerContainer)customData;
        if (candidateStr != NULL) {
            container->client->lock.lock();
            if(!container->client->noNewThreads) {
                container->client->threads.push_back(std::thread(
                    [container, this](std::string candidate) {
                        RtcIceCandidateInit iceCandidate;
                        EXPECT_EQ(STATUS_SUCCESS, deserializeRtcIceCandidateInit((PCHAR) candidate.c_str(), STRLEN(candidate.c_str()), &iceCandidate));
                        EXPECT_EQ(STATUS_SUCCESS, addIceCandidate((PRtcPeerConnection) container->pc, iceCandidate.candidate));
                    },
                    std::string(candidateStr)));
            }
            container->client->lock.unlock();
        }

    };

    RtcOnIceCandidate onICECandidateHdlrDone = [](UINT64 customData, PCHAR candidateStr) -> void {
        UNUSED_PARAM(customData);
        UNUSED_PARAM(candidateStr);
    };

    offer.pc = offerPc;
    offer.client = this;
    answer.pc = answerPc;
    answer.client = this;

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) &answer, onICECandidateHdlr));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) &offer, onICECandidateHdlr));

    auto onICEConnectionStateChangeHdlr = [](UINT64 customData, RTC_PEER_CONNECTION_STATE newState) -> void {
        ATOMIC_INCREMENT((PSIZE_T) customData + newState);
    };

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnConnectionStateChange(offerPc, (UINT64) this->stateChangeCount, onICEConnectionStateChangeHdlr));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnConnectionStateChange(answerPc, (UINT64) this->stateChangeCount, onICEConnectionStateChangeHdlr));

    EXPECT_EQ(STATUS_SUCCESS, createOffer(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(answerPc, &sdp));

    // Validate the cert fingerprint if we are asked to do so
    if (pOfferCertFingerprint != NULL) {
        EXPECT_NE((PCHAR) NULL, STRSTR(sdp.sdp, pOfferCertFingerprint));
    }

    EXPECT_EQ(STATUS_SUCCESS, createAnswer(answerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(answerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(offerPc, &sdp));

    if (pAnswerCertFingerprint != NULL) {
        EXPECT_NE((PCHAR) NULL, STRSTR(sdp.sdp, pAnswerCertFingerprint));
    }

    // Wait for both peers to reach CONNECTED state
    for (auto i = 0; i <= 100 && ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_CONNECTED]) != 2; i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
        if (ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_FAILED]) == 2) {
            break;
        }
    }
    this->noNewThreads = TRUE;

    this->lock.lock();
    //join all threads before leaving
    for (auto& th : this->threads) th.join();

    this->threads.clear();
    this->lock.unlock();

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) 0, onICECandidateHdlrDone));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) 0, onICECandidateHdlrDone));


    return ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_CONNECTED]) == 2;
}

// Create track and transceiver and adds to PeerConnection
void WebRtcClientTestBase::addTrackToPeerConnection(PRtcPeerConnection pRtcPeerConnection, PRtcMediaStreamTrack track,
                                                    PRtcRtpTransceiver* transceiver, RTC_CODEC codec, MEDIA_STREAM_TRACK_KIND kind)
{
    MEMSET(track, 0x00, SIZEOF(RtcMediaStreamTrack));

    EXPECT_EQ(STATUS_SUCCESS, addSupportedCodec(pRtcPeerConnection, codec));

    track->kind = kind;
    track->codec = codec;
    EXPECT_EQ(STATUS_SUCCESS, generateJSONSafeString(track->streamId, MAX_MEDIA_STREAM_ID_LEN));
    EXPECT_EQ(STATUS_SUCCESS, generateJSONSafeString(track->trackId, MAX_MEDIA_STREAM_ID_LEN));

    EXPECT_EQ(STATUS_SUCCESS, addTransceiver(pRtcPeerConnection, track, NULL, transceiver));
}

void WebRtcClientTestBase::getIceServers(PRtcConfiguration pRtcConfiguration)
{
    UINT32 i, j, iceConfigCount, uriCount;
    PIceConfigInfo pIceConfigInfo;

    // Assume signaling client is already created
    EXPECT_EQ(STATUS_SUCCESS, signalingClientGetIceConfigInfoCount(mSignalingClientHandle, &iceConfigCount));

    // Set the  STUN server
    SNPRINTF(pRtcConfiguration->iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL, TEST_DEFAULT_REGION, TEST_DEFAULT_STUN_URL_POSTFIX);

    for (uriCount = 0, i = 0; i < iceConfigCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, signalingClientGetIceConfigInfo(mSignalingClientHandle, i, &pIceConfigInfo));
        for (j = 0; j < pIceConfigInfo->uriCount; j++) {
            STRNCPY(pRtcConfiguration->iceServers[uriCount + 1].urls, pIceConfigInfo->uris[j], MAX_ICE_CONFIG_URI_LEN);
            STRNCPY(pRtcConfiguration->iceServers[uriCount + 1].credential, pIceConfigInfo->password, MAX_ICE_CONFIG_CREDENTIAL_LEN);
            STRNCPY(pRtcConfiguration->iceServers[uriCount + 1].username, pIceConfigInfo->userName, MAX_ICE_CONFIG_USER_NAME_LEN);

            uriCount++;
        }
    }
}

PCHAR WebRtcClientTestBase::GetTestName()
{
    return (PCHAR)::testing::UnitTest::GetInstance()->current_test_info()->test_case_name();
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
