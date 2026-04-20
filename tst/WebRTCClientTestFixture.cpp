#include "WebRTCClientTestFixture.h"
#include "src/source/Rtp/Codecs/RtpH264Payloader.h"

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
    :
#ifdef ENABLE_SIGNALING
      mSignalingClientHandle(INVALID_SIGNALING_CLIENT_HANDLE_VALUE),
#endif
      mAccessKey(NULL), mSecretKey(NULL), mSessionToken(NULL), mRegion(NULL), mCaCertPath(NULL), mAccessKeyIdSet(FALSE)
{
    // Initialize the endianness of the library
    initializeEndianness();

    SRAND(12345);

    mStreamingRotationPeriod = TEST_STREAMING_TOKEN_DURATION;
}

void WebRtcClientTestBase::SetUp()
{
    DLOGI("\nSetting up test: %s\n", GetTestName());
    noNewThreads = FALSE;

    SET_INSTRUMENTED_ALLOCATORS();

    mLogLevel = LOG_LEVEL_DEBUG;

    PCHAR logLevelStr = GETENV(DEBUG_LOG_LEVEL_ENV_VAR);
    if (logLevelStr != NULL) {
        ASSERT_EQ(STATUS_SUCCESS, STRTOUI32(logLevelStr, NULL, 10, &mLogLevel));
    }

    SET_LOGGER_LOG_LEVEL(mLogLevel);

    if (STATUS_SUCCESS != initKvsWebRtc()) {
        DLOGE("Test initKvsWebRtc FAILED!!!!");
    }

#ifdef ENABLE_SIGNALING
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
#endif
}

void WebRtcClientTestBase::TearDown()
{
    DLOGI("\nTearing down test: %s\n", GetTestName());

    deinitKvsWebRtc();
#ifdef ENABLE_KVS_THREADPOOL
    // Need this sleep for threads in threadpool to close
    THREAD_SLEEP(400 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
#endif

#ifdef ENABLE_SIGNALING
    freeStaticCredentialProvider(&mTestCredentialProvider);
#endif

    EXPECT_EQ(STATUS_SUCCESS, RESET_INSTRUMENTED_ALLOCATORS());
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

    auto onICECandidateHdlr = [](UINT64 customData, PCHAR candidateStr) -> void {
        PPeerContainer container = (PPeerContainer) customData;
        if (candidateStr != NULL) {
            container->client->lock.lock();
            if (!container->client->noNewThreads) {
                container->client->threads.push_back(std::thread(
                    [container](std::string candidate) {
                        RtcIceCandidateInit iceCandidate;
                        EXPECT_EQ(STATUS_SUCCESS,
                                  deserializeRtcIceCandidateInit((PCHAR) candidate.c_str(), STRLEN(candidate.c_str()), &iceCandidate));
                        EXPECT_EQ(STATUS_SUCCESS, addIceCandidate((PRtcPeerConnection) container->pc, iceCandidate.candidate));
                    },
                    std::string(candidateStr)));
            }
            container->client->lock.unlock();
        }
    };

    auto onICECandidateHdlrDone = [](UINT64 customData, PCHAR candidateStr) -> void {
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

    for (auto i = 0; i <= 10 && ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_CONNECTED]) != 2 &&
         ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_FAILED]) == 0;
         i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

    this->lock.lock();
    // join all threads before leaving
    for (auto& th : this->threads)
        th.join();

    this->threads.clear();
    this->noNewThreads = TRUE;
    this->lock.unlock();

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) 0, onICECandidateHdlrDone));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) 0, onICECandidateHdlrDone));

    return ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_CONNECTED]) == 2;
}

// Non-trickle variant of connectTwoPeers. See header for parameter semantics.
bool WebRtcClientTestBase::connectTwoPeersNoTrickle(PRtcPeerConnection offerPc, PRtcPeerConnection answerPc, bool assumeOfferGathered,
                                                    bool assumeAnswerGathered)
{
    RtcSessionDescriptionInit sdp;
    SIZE_T offerDoneGather = 0, answerDoneGather = 0;
    UINT64 timeout;

    auto onDoneGatherHdlr = [](UINT64 customData, PCHAR candidateStr) -> void {
        if (candidateStr == NULL) {
            ATOMIC_STORE((PSIZE_T) customData, 1);
        }
    };
    auto onDoneHdlrFinal = [](UINT64 customData, PCHAR candidateStr) -> void {
        UNUSED_PARAM(customData);
        UNUSED_PARAM(candidateStr);
    };

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) &offerDoneGather, onDoneGatherHdlr));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) &answerDoneGather, onDoneGatherHdlr));

    auto onStateChangeHdlr = [](UINT64 customData, RTC_PEER_CONNECTION_STATE newState) -> void {
        ATOMIC_INCREMENT((PSIZE_T) customData + newState);
    };
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnConnectionStateChange(offerPc, (UINT64) this->stateChangeCount, onStateChangeHdlr));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnConnectionStateChange(answerPc, (UINT64) this->stateChangeCount, onStateChangeHdlr));

    // Produce the offer SDP BEFORE gathering starts. populateSessionDescription iterates valid local candidates,
    // and host candidates become VALID synchronously inside iceAgentInitHostCandidate; calling createOffer after
    // setLocalDescription would bake them in regardless of whether we then call
    // peerConnectionGetCurrentLocalDescription. Capturing the offer before gathering gives us a guaranteed-bare
    // offer SDP that we can optionally refresh with candidates after gathering completes.
    MEMSET(&sdp, 0x00, SIZEOF(RtcSessionDescriptionInit));
    EXPECT_EQ(STATUS_SUCCESS, createOffer(offerPc, &sdp));

    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(answerPc, &sdp));

    // Wait only for sides that are supposed to publish candidates in their SDP.
    timeout = GETTIME() + KVS_ICE_GATHER_REFLEXIVE_AND_RELAYED_CANDIDATE_TIMEOUT + 2 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    while (GETTIME() < timeout && ((!assumeOfferGathered && ATOMIC_LOAD(&offerDoneGather) == 0) ||
                                   (!assumeAnswerGathered && ATOMIC_LOAD(&answerDoneGather) == 0))) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }
    if (!assumeOfferGathered) {
        EXPECT_GT(ATOMIC_LOAD(&offerDoneGather), 0u);
    }
    if (!assumeAnswerGathered) {
        EXPECT_GT(ATOMIC_LOAD(&answerDoneGather), 0u);
    }

    // If we waited for the offer side, refresh sdp with its post-gathering description (now has a=candidate
    // lines). Otherwise leave sdp as the bare offer captured before gathering started.
    if (!assumeOfferGathered) {
        EXPECT_EQ(STATUS_SUCCESS, peerConnectionGetCurrentLocalDescription(offerPc, &sdp));
    }
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(answerPc, &sdp));

    EXPECT_EQ(STATUS_SUCCESS, createAnswer(answerPc, &sdp));
    if (!assumeAnswerGathered) {
        EXPECT_EQ(STATUS_SUCCESS, peerConnectionGetCurrentLocalDescription(answerPc, &sdp));
    }
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(offerPc, &sdp));

    for (auto i = 0; i <= 10 && ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_CONNECTED]) != 2 &&
         ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_FAILED]) == 0;
         i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) 0, onDoneHdlrFinal));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) 0, onDoneHdlrFinal));

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
#ifdef ENABLE_SIGNALING
    UINT32 i, j, iceConfigCount, uriCount;
    PIceConfigInfo pIceConfigInfo;

    // Assume signaling client is already created
    EXPECT_EQ(STATUS_SUCCESS, signalingClientGetIceConfigInfoCount(mSignalingClientHandle, &iceConfigCount));

    // Set the  STUN server
    SNPRINTF(pRtcConfiguration->iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL, TEST_DEFAULT_REGION,
             TEST_DEFAULT_STUN_URL_POSTFIX);

    for (uriCount = 0, i = 0; i < iceConfigCount; i++) {
        EXPECT_EQ(STATUS_SUCCESS, signalingClientGetIceConfigInfo(mSignalingClientHandle, i, &pIceConfigInfo));
        for (j = 0; j < pIceConfigInfo->uriCount; j++) {
            STRNCPY(pRtcConfiguration->iceServers[uriCount + 1].urls, pIceConfigInfo->uris[j], MAX_ICE_CONFIG_URI_LEN);
            STRNCPY(pRtcConfiguration->iceServers[uriCount + 1].credential, pIceConfigInfo->password, MAX_ICE_CONFIG_CREDENTIAL_LEN);
            STRNCPY(pRtcConfiguration->iceServers[uriCount + 1].username, pIceConfigInfo->userName, MAX_ICE_CONFIG_USER_NAME_LEN);

            uriCount++;
        }
    }
#endif /* ENABLE_SIGNALING */
}

void WebRtcClientTestBase::initRtcConfiguration(PRtcConfiguration pRtcConfiguration)
{
    MEMSET(pRtcConfiguration, 0x00, SIZEOF(RtcConfiguration));
    pRtcConfiguration->kvsRtcConfiguration.iceLocalCandidateGatheringTimeout = KVS_CONVERT_TIMESCALE(1000, 1000, HUNDREDS_OF_NANOS_IN_A_SECOND);
    pRtcConfiguration->kvsRtcConfiguration.iceConnectionCheckTimeout = KVS_CONVERT_TIMESCALE(1000, 1000, HUNDREDS_OF_NANOS_IN_A_SECOND);
    pRtcConfiguration->kvsRtcConfiguration.iceCandidateNominationTimeout = KVS_CONVERT_TIMESCALE(2000, 1000, HUNDREDS_OF_NANOS_IN_A_SECOND);
    // Low-end Android devices we run tests on have small default socket
    // receive buffers, causing packet drops under burst traffic.
#ifdef __ANDROID__
    pRtcConfiguration->kvsRtcConfiguration.recvBufSize = 512 * 1024;
#endif
}

PCHAR WebRtcClientTestBase::GetTestName()
{
    return (PCHAR)::testing::UnitTest::GetInstance()->current_test_info()->test_case_name();
}

std::vector<TestFrame> WebRtcClientTestBase::loadFramesFromFolder(PCHAR folder, UINT32 count, RTC_CODEC codec, UINT32 timescale, UINT64 frameDuration,
                                                                  UINT32 firstIndex)
{
    constexpr UINT32 MAX_FRAME_SIZE = 500000;
    std::vector<BYTE> buffer(MAX_FRAME_SIZE);
    std::vector<TestFrame> frames;
    frames.reserve(count);

    DLOGI("Loading %u frames from %s", count, folder);
    for (UINT32 i = 0; i < count; i++) {
        UINT32 frameSize = MAX_FRAME_SIZE;
        EXPECT_EQ(STATUS_SUCCESS, readFrameData(buffer.data(), &frameSize, firstIndex + i, folder, codec));
        TestFrame tf;
        tf.data.assign(buffer.data(), buffer.data() + frameSize);
        tf.sendPts = (UINT64) i * frameDuration;
        tf.timescale = timescale;
        frames.push_back(std::move(tf));
    }
    return frames;
}

void WebRtcClientTestBase::expectTestFramesNalUnitsEqual(const TestFrame& expected, const TestFrame& actual, const char* context)
{
    constexpr UINT32 MAX_NALUS = 128;
    UINT32 expOffsets[MAX_NALUS], expLengths[MAX_NALUS];
    UINT32 actOffsets[MAX_NALUS], actLengths[MAX_NALUS];

    UINT32 expCount = extractNaluInfo((PBYTE) expected.data.data(), (UINT32) expected.data.size(), expOffsets, expLengths, MAX_NALUS);
    UINT32 actCount = extractNaluInfo((PBYTE) actual.data.data(), (UINT32) actual.data.size(), actOffsets, actLengths, MAX_NALUS);

    // The KVS H264 depayloader sometimes drops a leading AUD (NAL type 9) when
    // reassembling a frame. Tolerate it: if the expected side begins with an
    // AUD and the actual side does not, skip the leading AUD in the expected
    // side and compare the remaining NAL units.
    UINT32 expStart = 0;
    if (expCount >= 1 && actCount >= 1 && expLengths[0] >= 1 && actLengths[0] >= 1 && (expected.data[expOffsets[0]] & 0x1F) == H264_NALU_TYPE_AUD &&
        (actual.data[actOffsets[0]] & 0x1F) != H264_NALU_TYPE_AUD) {
        expStart = 1;
    }
    UINT32 expCountCmp = expCount - expStart;

    if (expCountCmp != actCount) {
        DLOGI("%s: NAL unit count mismatch: expected %u, actual %u", context, expCountCmp, actCount);
    }
    EXPECT_EQ(expCountCmp, actCount) << context << ": NAL count mismatch";

    for (UINT32 i = 0; i < MIN(expCountCmp, actCount); i++) {
        UINT32 e = i + expStart;
        EXPECT_EQ(expLengths[e], actLengths[i]) << context << ": NAL " << i << " length mismatch";
        if (expLengths[e] == actLengths[i]) {
            EXPECT_EQ(0, MEMCMP(expected.data.data() + expOffsets[e], actual.data.data() + actOffsets[i], expLengths[e]))
                << context << ": NAL " << i << " data mismatch";
        }
    }
}

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
