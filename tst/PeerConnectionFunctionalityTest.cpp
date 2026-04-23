#include "WebRTCClientTestFixture.h"

#include <algorithm>

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

// Forward declaration for pacing test helper
struct PacingTestContext;

class PeerConnectionFunctionalityTest : public WebRtcClientTestBase {
  protected:
    void runPacingTest(const RtcPacerConfig& pacerConfig, PacingTestContext& context);
    void validatePacingResults(PacingTestContext& context, const std::vector<TwccPacketReport>& validReports, UINT64 targetBitrateBps,
                               DOUBLE pacingFactor);
};

// Assert that two PeerConnections can connect to each other and go to connected
TEST_F(PeerConnectionFunctionalityTest, connectTwoPeers)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

// End-to-end: a full ICE agent (offerer) connecting to a lite ICE agent (answerer) must reach CONNECTED,
// with full=controlling / lite=controlled per RFC 8445.
TEST_F(PeerConnectionFunctionalityTest, connectFullToLitePeer)
{
    RtcConfiguration fullCfg{}, liteCfg{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&fullCfg);
    initRtcConfiguration(&liteCfg);
    liteCfg.kvsRtcConfiguration.iceLiteMode = TRUE;

    EXPECT_EQ(createPeerConnection(&fullCfg, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&liteCfg, &answerPc), STATUS_SUCCESS);

    ASSERT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    PKvsPeerConnection pKvsOffer = (PKvsPeerConnection) offerPc;
    PKvsPeerConnection pKvsAnswer = (PKvsPeerConnection) answerPc;

    // Full agent observed the answerer's a=ice-lite and took the controlling role.
    EXPECT_FALSE(pKvsOffer->pIceAgent->isLiteAgent);
    EXPECT_TRUE(pKvsOffer->remoteIsIceLite);
    EXPECT_TRUE(pKvsOffer->pIceAgent->isControlling);

    // Lite agent is controlled and did not initiate any binding requests.
    EXPECT_TRUE(pKvsAnswer->pIceAgent->isLiteAgent);
    EXPECT_FALSE(pKvsAnswer->pIceAgent->isControlling);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);
    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

// Non-trickle exchange where the full (offerer) peer hands over its offer SDP before gathering completes, so
// the offer carries no a=candidate lines; the lite peer still gathers host candidates normally and publishes
// them in the answer SDP. Full learns lite's addresses and probes them. Lite, seeing an inbound binding request
// from an unknown source, learns the full peer's address via peer-reflexive discovery (RFC 8445 §7.3.1.3) and
// responds. Both peers must reach CONNECTED.
TEST_F(PeerConnectionFunctionalityTest, liteConnectsWhenFullEmitsNoCandidates)
{
    RtcConfiguration fullCfg{}, liteCfg{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&fullCfg);
    initRtcConfiguration(&liteCfg);
    liteCfg.kvsRtcConfiguration.iceLiteMode = TRUE;

    ASSERT_EQ(createPeerConnection(&fullCfg, &offerPc), STATUS_SUCCESS);
    ASSERT_EQ(createPeerConnection(&liteCfg, &answerPc), STATUS_SUCCESS);

    EXPECT_TRUE(connectTwoPeersNoTrickle(offerPc, answerPc, /*assumeOfferGathered=*/true));

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);
    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

// Proves that announcedIpAddress is applied at the PeerConnection layer (not just the ICE-agent unit): at least one
// IPv4 host candidate trickled out by the lite peer must carry the announced address in its SDP form. We assert on
// the trickled candidate strings rather than on whether the connection succeeds — the latter depends on host
// routing (e.g. an Android image with IPv6 loopback may connect over IPv6, where the announced IPv4 rewrite does
// not apply), which makes pass/fail platform-dependent.
TEST_F(PeerConnectionFunctionalityTest, announcedIpPropagatesIntoLiteTrickledCandidates)
{
    RtcConfiguration fullCfg{}, liteCfg{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&fullCfg);
    initRtcConfiguration(&liteCfg);
    liteCfg.kvsRtcConfiguration.iceLiteMode = TRUE;
    STRNCPY(liteCfg.kvsRtcConfiguration.announcedIpAddress, "203.0.113.10", KVS_IP_ADDRESS_STRING_BUFFER_LEN - 1);

    ASSERT_EQ(createPeerConnection(&fullCfg, &offerPc), STATUS_SUCCESS);
    ASSERT_EQ(createPeerConnection(&liteCfg, &answerPc), STATUS_SUCCESS);

    // Collect every candidate string lite trickles out.
    std::vector<std::string> liteCandidates;
    std::mutex liteCandidatesLock;
    struct Capture {
        std::vector<std::string>* candidates;
        std::mutex* lock;
    } capture{&liteCandidates, &liteCandidatesLock};

    auto captureHdlr = [](UINT64 customData, PCHAR candidateStr) -> void {
        if (candidateStr == NULL) {
            return;
        }
        Capture* cap = (Capture*) customData;
        cap->lock->lock();
        cap->candidates->emplace_back(candidateStr);
        cap->lock->unlock();
    };
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) &capture, captureHdlr));

    // Drive the offer/answer exchange manually; no need to actually connect.
    RtcSessionDescriptionInit sdp;
    MEMSET(&sdp, 0x00, SIZEOF(RtcSessionDescriptionInit));
    EXPECT_EQ(STATUS_SUCCESS, createOffer(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(answerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, createAnswer(answerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(answerPc, &sdp));

    // Give host gathering a moment to run and the trickle callback a moment to fire.
    UINT64 timeout = GETTIME() + 3 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    bool found = false;
    while (!found && GETTIME() < timeout) {
        THREAD_SLEEP(50 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        liteCandidatesLock.lock();
        for (const auto& c : liteCandidates) {
            if (c.find("203.0.113.10") != std::string::npos) {
                found = true;
                break;
            }
        }
        liteCandidatesLock.unlock();
    }
    EXPECT_TRUE(found);

    auto noopHdlr = [](UINT64, PCHAR) -> void {};
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, 0, noopHdlr));

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);
    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

TEST_F(PeerConnectionFunctionalityTest, connectTwoPeersWithDelay)
{
    RtcConfiguration configuration{};
    RtcSessionDescriptionInit sdp;
    SIZE_T connectedCount = 0;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    PeerContainer offer;
    PeerContainer answer;

    initRtcConfiguration(&configuration);
    // solves occassional failure in this test where the peers fail to connect because of the delay in ICE candidate nomination
    configuration.kvsRtcConfiguration.iceCandidateNominationTimeout = KVS_CONVERT_TIMESCALE(2000, 1000, HUNDREDS_OF_NANOS_IN_A_SECOND);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

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

    offer.pc = offerPc;
    offer.client = this;
    answer.pc = answerPc;
    answer.client = this;

    auto onICECandidateHdlrDone = [](UINT64 customData, PCHAR candidateStr) -> void {
        UNUSED_PARAM(customData);
        UNUSED_PARAM(candidateStr);
    };

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) &answer, onICECandidateHdlr));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) &offer, onICECandidateHdlr));

    auto onICEConnectionStateChangeHdlr = [](UINT64 customData, RTC_PEER_CONNECTION_STATE newState) -> void {
        if (newState == RTC_PEER_CONNECTION_STATE_CONNECTED) {
            ATOMIC_INCREMENT((PSIZE_T) customData);
        }
    };

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnConnectionStateChange(offerPc, (UINT64) &connectedCount, onICEConnectionStateChangeHdlr));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnConnectionStateChange(answerPc, (UINT64) &connectedCount, onICEConnectionStateChangeHdlr));

    EXPECT_EQ(STATUS_SUCCESS, createOffer(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(answerPc, &sdp));

    EXPECT_EQ(STATUS_SUCCESS, createAnswer(answerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(answerPc, &sdp));

    THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);

    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(offerPc, &sdp));

    for (auto i = 0; i <= 100 && ATOMIC_LOAD(&connectedCount) != 2; i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

    EXPECT_EQ(2, connectedCount);

    this->lock.lock();
    // join all threads before leaving
    for (auto& th : this->threads)
        th.join();

    this->threads.clear();
    this->noNewThreads = TRUE;
    this->lock.unlock();

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) 0, onICECandidateHdlrDone));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) 0, onICECandidateHdlrDone));

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

#ifdef KVS_USE_OPENSSL
TEST_F(PeerConnectionFunctionalityTest, connectTwoPeersWithPresetCerts)
{
    RtcConfiguration offerConfig{}, answerConfig{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    X509* pOfferCert = NULL;
    X509* pAnswerCert = NULL;
    EVP_PKEY* pOfferKey = NULL;
    EVP_PKEY* pAnswerKey = NULL;
    CHAR offerCertFingerprint[CERTIFICATE_FINGERPRINT_LENGTH];
    CHAR answerCertFingerprint[CERTIFICATE_FINGERPRINT_LENGTH];

    // Generate offer cert
    ASSERT_EQ(STATUS_SUCCESS, createCertificateAndKey(GENERATED_CERTIFICATE_BITS, true, &pOfferCert, &pOfferKey));
    ASSERT_EQ(STATUS_SUCCESS, dtlsCertificateFingerprint(pOfferCert, offerCertFingerprint));

    // Generate answer cert
    ASSERT_EQ(STATUS_SUCCESS, createCertificateAndKey(GENERATED_CERTIFICATE_BITS, true, &pAnswerCert, &pAnswerKey));
    ASSERT_EQ(STATUS_SUCCESS, dtlsCertificateFingerprint(pAnswerCert, answerCertFingerprint));

    initRtcConfiguration(&offerConfig);
    offerConfig.certificates[0].pCertificate = (PBYTE) pOfferCert;
    offerConfig.certificates[0].certificateSize = 0;
    offerConfig.certificates[0].pPrivateKey = (PBYTE) pOfferKey;
    offerConfig.certificates[0].privateKeySize = 0;

    initRtcConfiguration(&answerConfig);
    answerConfig.certificates[0].pCertificate = (PBYTE) pAnswerCert;
    answerConfig.certificates[0].certificateSize = 0;
    answerConfig.certificates[0].pPrivateKey = (PBYTE) pAnswerKey;
    answerConfig.certificates[0].privateKeySize = 0;

    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&offerConfig, &offerPc));
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&answerConfig, &answerPc));

    // Should be fine to free right after create peer connection
    freeCertificateAndKey(&pOfferCert, &pOfferKey);
    freeCertificateAndKey(&pAnswerCert, &pAnswerKey);

    EXPECT_EQ(TRUE, connectTwoPeers(offerPc, answerPc, offerCertFingerprint, answerCertFingerprint));

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}
#elif KVS_USE_MBEDTLS
TEST_F(PeerConnectionFunctionalityTest, connectTwoPeersWithPresetCerts)
{
    RtcConfiguration offerConfig{}, answerConfig{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    mbedtls_x509_crt offerCert;
    mbedtls_x509_crt answerCert;
    mbedtls_pk_context offerKey;
    mbedtls_pk_context answerKey;
    CHAR offerCertFingerprint[CERTIFICATE_FINGERPRINT_LENGTH];
    CHAR answerCertFingerprint[CERTIFICATE_FINGERPRINT_LENGTH];

    // Generate offer cert
    ASSERT_EQ(STATUS_SUCCESS, createCertificateAndKey(GENERATED_CERTIFICATE_BITS, true, &offerCert, &offerKey));
    ASSERT_EQ(STATUS_SUCCESS, dtlsCertificateFingerprint(&offerCert, offerCertFingerprint));

    // Generate answer cert
    ASSERT_EQ(STATUS_SUCCESS, createCertificateAndKey(GENERATED_CERTIFICATE_BITS, true, &answerCert, &answerKey));
    ASSERT_EQ(STATUS_SUCCESS, dtlsCertificateFingerprint(&answerCert, answerCertFingerprint));

    initRtcConfiguration(&offerConfig);
    offerConfig.certificates[0].pCertificate = (PBYTE) &offerCert;
    offerConfig.certificates[0].certificateSize = 0;
    offerConfig.certificates[0].pPrivateKey = (PBYTE) &offerKey;
    offerConfig.certificates[0].privateKeySize = 0;

    initRtcConfiguration(&answerConfig);
    answerConfig.certificates[0].pCertificate = (PBYTE) &answerCert;
    answerConfig.certificates[0].certificateSize = 0;
    answerConfig.certificates[0].pPrivateKey = (PBYTE) &answerKey;
    answerConfig.certificates[0].privateKeySize = 0;

    ASSERT_EQ(STATUS_SUCCESS, createPeerConnection(&offerConfig, &offerPc));
    ASSERT_EQ(STATUS_SUCCESS, createPeerConnection(&answerConfig, &answerPc));

    // Should be fine to free right after create peer connection
    freeCertificateAndKey(&offerCert, &offerKey);
    freeCertificateAndKey(&answerCert, &answerKey);

    ASSERT_EQ(TRUE, connectTwoPeers(offerPc, answerPc, offerCertFingerprint, answerCertFingerprint));

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}
#endif

// Assert that two PeerConnections with forced TURN can connect to each other and go to connected
TEST_F(PeerConnectionFunctionalityTest, DISABLED_connectTwoPeersForcedTURN)
{
    ASSERT_EQ(TRUE, mAccessKeyIdSet);

    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&configuration);
    configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_RELAY;

    initializeSignalingClient();
    getIceServers(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    deinitializeSignalingClient();
}

TEST_F(PeerConnectionFunctionalityTest, DISABLED_sendDataWithClosedSocketConnectionWithHostAndStun)
{
    ASSERT_EQ(TRUE, mAccessKeyIdSet);

    RtcMediaStreamTrack offerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver;
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    PKvsPeerConnection pOfferPcImpl;
    PIceAgent pIceAgent;
    PIceCandidate pLocalCandidate;
    PSocketConnection pSocketConnection;

    initRtcConfiguration(&configuration);
    SNPRINTF(configuration.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL, TEST_DEFAULT_REGION, TEST_DEFAULT_STUN_URL_POSTFIX);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    // addTrackToPeerConnection is necessary because we need to add a transceiver which will trigger the RTCP callback. The RTCP callback
    // will send application data. The expected behavior for the PeerConnection is to bail out when the socket connection that's being used
    // is already closed.
    //
    // In summary, the scenario looks like the following:
    //   1. Connect the two peers
    //   2. Add a transceiver, which will send RTCP feedback in a regular interval + some randomness
    //   3. Do fault injection to the ICE agent, simulate early closed connection
    //   4. Wait for the RTCP callback to fire, which will change the ICE agent status to STATUS_SOCKET_CONNECTION_CLOSED_ALREADY
    //   5. Wait for the ICE agent state regular polling to check the status and update the ICE agent state to FAILED
    //   6. When ICE agent state changes to FAILED, the PeerConnection will be notified and change its state to FAILED as well
    //   7. Verify that we the counter for RTC_PEER_CONNECTION_STATE_FAILED is not 0
    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    pOfferPcImpl = (PKvsPeerConnection) offerPc;
    pIceAgent = pOfferPcImpl->pIceAgent;
    MUTEX_LOCK(pIceAgent->lock);
    pLocalCandidate = pIceAgent->pDataSendingIceCandidatePair->local;

    if (pLocalCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED) {
        pSocketConnection = pLocalCandidate->pTurnConnection->pControlChannel;
    } else {
        pSocketConnection = pLocalCandidate->pSocketConnection;
    }
    EXPECT_EQ(STATUS_SUCCESS, socketConnectionClosed(pSocketConnection));
    MUTEX_UNLOCK(pIceAgent->lock);

    // The next poll should check the current ICE agent status and drives the ICE agent state machine to failed,
    // change the PeerConnection state to failed as well.
    //
    // We need to add 2 seconds because we need to first wait the RTCP callback to fire first after the fault injection.
    THREAD_SLEEP(KVS_ICE_STATE_READY_TIMER_POLLING_INTERVAL + 2 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    EXPECT_NE(0, ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_FAILED]));

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

TEST_F(PeerConnectionFunctionalityTest, DISABLED_sendDataWithClosedSocketConnectionWithForcedTurn)
{
    ASSERT_EQ(TRUE, mAccessKeyIdSet);

    RtcMediaStreamTrack offerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver;
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    PKvsPeerConnection pOfferPcImpl;
    PIceAgent pIceAgent;
    PIceCandidate pLocalCandidate;
    PSocketConnection pSocketConnection;

    initRtcConfiguration(&configuration);
    configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_RELAY;

    initializeSignalingClient();
    getIceServers(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    // addTrackToPeerConnection is necessary because we need to add a transceiver which will trigger the RTCP callback. The RTCP callback
    // will send application data. The expected behavior for the PeerConnection is to bail out when the socket connection that's being used
    // is already closed.
    //
    // In summary, the scenario looks like the following:
    //   1. Connect the two peers
    //   2. Add a transceiver, which will send RTCP feedback in a regular interval + some randomness
    //   3. Do fault injection to the ICE agent, simulate early closed connection
    //   4. Wait for the RTCP callback to fire, which will change the ICE agent status to STATUS_SOCKET_CONNECTION_CLOSED_ALREADY
    //   5. Wait for the ICE agent state regular polling to check the status and update the ICE agent state to FAILED
    //   6. When ICE agent state changes to FAILED, the PeerConnection will be notified and change its state to FAILED as well
    //   7. Verify that we the counter for RTC_PEER_CONNECTION_STATE_FAILED is not 0
    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    pOfferPcImpl = (PKvsPeerConnection) offerPc;
    pIceAgent = pOfferPcImpl->pIceAgent;
    MUTEX_LOCK(pIceAgent->lock);
    pLocalCandidate = pIceAgent->pDataSendingIceCandidatePair->local;

    if (pLocalCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED) {
        pSocketConnection = pLocalCandidate->pTurnConnection->pControlChannel;
    } else {
        pSocketConnection = pLocalCandidate->pSocketConnection;
    }
    EXPECT_EQ(STATUS_SUCCESS, socketConnectionClosed(pSocketConnection));
    MUTEX_UNLOCK(pIceAgent->lock);

    // The next poll should check the current ICE agent status and drives the ICE agent state machine to failed,
    // change the PeerConnection state to failed as well.
    //
    // We need to add 2 seconds because we need to first wait the RTCP callback to fire first after the fault injection.
    THREAD_SLEEP(KVS_ICE_STATE_READY_TIMER_POLLING_INTERVAL + 2 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    EXPECT_NE(0, ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_FAILED]));

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    deinitializeSignalingClient();
}

TEST_F(PeerConnectionFunctionalityTest, DISABLED_shutdownTurnDueToP2PFoundBeforeTurnEstablished)
{
    ASSERT_EQ(TRUE, mAccessKeyIdSet);

    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    PIceAgent pIceAgent = NULL;
    PDoubleListNode pCurNode = NULL;
    PIceCandidate pIceCandidate = NULL;

    initRtcConfiguration(&configuration);

    initializeSignalingClient();
    getIceServers(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    THREAD_SLEEP(5 * HUNDREDS_OF_NANOS_IN_A_SECOND);

    pIceAgent = ((PKvsPeerConnection) offerPc)->pIceAgent;
    MUTEX_LOCK(pIceAgent->lock);
    EXPECT_EQ(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode), STATUS_SUCCESS);
    while (pCurNode != NULL) {
        pIceCandidate = (PIceCandidate) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED) {
            EXPECT_TRUE(!ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->hasAllocation) ||
                        ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->stopTurnConnection));
        }
    }
    MUTEX_UNLOCK(pIceAgent->lock);

    pIceAgent = ((PKvsPeerConnection) answerPc)->pIceAgent;
    MUTEX_LOCK(pIceAgent->lock);
    EXPECT_EQ(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode), STATUS_SUCCESS);
    while (pCurNode != NULL) {
        pIceCandidate = (PIceCandidate) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED) {
            EXPECT_TRUE(!ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->hasAllocation) ||
                        ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->stopTurnConnection));
        }
    }
    MUTEX_UNLOCK(pIceAgent->lock);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    deinitializeSignalingClient();
}

TEST_F(PeerConnectionFunctionalityTest, DISABLED_shutdownTurnDueToP2PFoundAfterTurnEstablished)
{
    ASSERT_EQ(TRUE, mAccessKeyIdSet);

    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcSessionDescriptionInit sdp;
    SIZE_T offerPcDoneGatherCandidate = 0, answerPcDoneGatherCandidate = 0;
    UINT64 candidateGatherTimeout;
    PIceAgent pIceAgent = NULL;
    PDoubleListNode pCurNode = NULL;
    PIceCandidate pIceCandidate = NULL;

    initRtcConfiguration(&configuration);

    initializeSignalingClient();
    getIceServers(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    auto onICECandidateHdlr = [](UINT64 customData, PCHAR candidateStr) -> void {
        PSIZE_T pDoneGatherCandidate = (PSIZE_T) customData;
        if (candidateStr == NULL) {
            ATOMIC_STORE(pDoneGatherCandidate, 1);
        }
    };

    EXPECT_EQ(peerConnectionOnIceCandidate(offerPc, (UINT64) &offerPcDoneGatherCandidate, onICECandidateHdlr), STATUS_SUCCESS);
    EXPECT_EQ(peerConnectionOnIceCandidate(answerPc, (UINT64) &answerPcDoneGatherCandidate, onICECandidateHdlr), STATUS_SUCCESS);

    auto onICEConnectionStateChangeHdlr = [](UINT64 customData, RTC_PEER_CONNECTION_STATE newState) -> void {
        ATOMIC_INCREMENT((PSIZE_T) customData + newState);
    };

    EXPECT_EQ(peerConnectionOnConnectionStateChange(offerPc, (UINT64) this->stateChangeCount, onICEConnectionStateChangeHdlr), STATUS_SUCCESS);
    EXPECT_EQ(peerConnectionOnConnectionStateChange(answerPc, (UINT64) this->stateChangeCount, onICEConnectionStateChangeHdlr), STATUS_SUCCESS);

    // start gathering candidates
    EXPECT_EQ(setLocalDescription(offerPc, &sdp), STATUS_SUCCESS);
    EXPECT_EQ(setLocalDescription(answerPc, &sdp), STATUS_SUCCESS);

    // give time for turn allocation to be finished
    candidateGatherTimeout = GETTIME() + KVS_ICE_GATHER_REFLEXIVE_AND_RELAYED_CANDIDATE_TIMEOUT + 2 * HUNDREDS_OF_NANOS_IN_A_SECOND;
    while (!(ATOMIC_LOAD(&offerPcDoneGatherCandidate) > 0 && ATOMIC_LOAD(&answerPcDoneGatherCandidate) > 0) && GETTIME() < candidateGatherTimeout) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

    EXPECT_TRUE(ATOMIC_LOAD(&offerPcDoneGatherCandidate) > 0);
    EXPECT_TRUE(ATOMIC_LOAD(&answerPcDoneGatherCandidate) > 0);

    EXPECT_EQ(createOffer(offerPc, &sdp), STATUS_SUCCESS);
    EXPECT_EQ(peerConnectionGetCurrentLocalDescription(offerPc, &sdp), STATUS_SUCCESS);
    EXPECT_EQ(setRemoteDescription(answerPc, &sdp), STATUS_SUCCESS);

    EXPECT_EQ(createAnswer(answerPc, &sdp), STATUS_SUCCESS);
    EXPECT_EQ(peerConnectionGetCurrentLocalDescription(answerPc, &sdp), STATUS_SUCCESS);
    EXPECT_EQ(setRemoteDescription(offerPc, &sdp), STATUS_SUCCESS);

    for (auto i = 0; i <= 100 && ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_CONNECTED]) != 2; i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

    EXPECT_TRUE(ATOMIC_LOAD(&this->stateChangeCount[RTC_PEER_CONNECTION_STATE_CONNECTED]) == 2);

    // give time for turn allocated to be freed
    THREAD_SLEEP(5 * HUNDREDS_OF_NANOS_IN_A_SECOND);

    pIceAgent = ((PKvsPeerConnection) offerPc)->pIceAgent;
    MUTEX_LOCK(pIceAgent->lock);
    EXPECT_EQ(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode), STATUS_SUCCESS);
    while (pCurNode != NULL) {
        pIceCandidate = (PIceCandidate) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED) {
            EXPECT_TRUE(!ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->hasAllocation) ||
                        ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->stopTurnConnection));
        }
    }
    MUTEX_UNLOCK(pIceAgent->lock);

    pIceAgent = ((PKvsPeerConnection) answerPc)->pIceAgent;
    MUTEX_LOCK(pIceAgent->lock);
    EXPECT_EQ(doubleListGetHeadNode(pIceAgent->localCandidates, &pCurNode), STATUS_SUCCESS);
    while (pCurNode != NULL) {
        pIceCandidate = (PIceCandidate) pCurNode->data;
        pCurNode = pCurNode->pNext;

        if (pIceCandidate->iceCandidateType == ICE_CANDIDATE_TYPE_RELAYED) {
            EXPECT_TRUE(!ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->hasAllocation) ||
                        ATOMIC_LOAD_BOOL(&pIceCandidate->pTurnConnection->stopTurnConnection));
        }
    }
    MUTEX_UNLOCK(pIceAgent->lock);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    deinitializeSignalingClient();
}

// Assert that two PeerConnections with host and stun candidate can go to connected
TEST_F(PeerConnectionFunctionalityTest, connectTwoPeersWithHostAndStun)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&configuration);

    // Set the  STUN server
    SNPRINTF(configuration.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL, TEST_DEFAULT_REGION, TEST_DEFAULT_STUN_URL_POSTFIX);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

// Assert that two PeerConnections can connect and then terminate one of them, the other one will eventually report disconnection
TEST_F(PeerConnectionFunctionalityTest, connectTwoPeersThenDisconnectTest)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    UINT32 i;
    constexpr UINT64 disconnectionTimeout = 2 * HUNDREDS_OF_NANOS_IN_A_SECOND;

    initRtcConfiguration(&configuration);
    configuration.kvsRtcConfiguration.iceDisconnectionTimeout = disconnectionTimeout;

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    // free offerPc so it wont send anymore keep alives and answerPc will detect disconnection
    freePeerConnection(&offerPc);

    THREAD_SLEEP(disconnectionTimeout);

    for (i = 0; i < 10; ++i) {
        if (ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_DISCONNECTED]) > 0) {
            break;
        }

        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

    EXPECT_TRUE(ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_DISCONNECTED]) > 0);

    freePeerConnection(&answerPc);
}

// Assert that PeerConnection will go to failed state when no turn server was given in turn only mode.
TEST_F(PeerConnectionFunctionalityTest, connectTwoPeersExpectFailureBecauseNoCandidatePair)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&configuration);
    configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_RELAY;

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), FALSE);

    // Both peers should have already transitioned to failed state by the time connectTwoPeers returns.
    // Poll briefly in case of scheduling delays.
    for (UINT32 i = 0; i < 10; ++i) {
        if (ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_FAILED]) == 2) {
            break;
        }
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }
    EXPECT_TRUE(ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_FAILED]) == 2);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

TEST_F(PeerConnectionFunctionalityTest, noLostFramesAfterConnected)
{
    struct Context {
        MUTEX mutex;
        ATOMIC_BOOL done;
        CVAR cvar;
    };

    RtcConfiguration configuration{};
    Context context;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver;
    RtcSessionDescriptionInit sdp;
    struct NoLostFramesContext {
        ATOMIC_BOOL seenFirstFrame;
        UINT64 receivedDecodingTs;
    };
    NoLostFramesContext frameCtx;
    ATOMIC_STORE_BOOL(&frameCtx.seenFirstFrame, FALSE);
    frameCtx.receivedDecodingTs = 0;
    Frame videoFrame;

    PeerContainer offer;
    PeerContainer answer;

    initRtcConfiguration(&configuration);
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));

    videoFrame.frameData = (PBYTE) MEMALLOC(1);
    videoFrame.size = 1;
    videoFrame.presentationTs = HUNDREDS_OF_NANOS_IN_A_SECOND;

    context.mutex = MUTEX_CREATE(FALSE);
    ASSERT_NE(context.mutex, INVALID_MUTEX_VALUE);
    context.cvar = CVAR_CREATE();
    ASSERT_NE(context.cvar, INVALID_CVAR_VALUE);
    ATOMIC_STORE_BOOL(&context.done, FALSE);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);

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

    offer.pc = offerPc;
    offer.client = this;
    answer.pc = answerPc;
    answer.client = this;

    auto onICECandidateHdlrDone = [](UINT64 customData, PCHAR candidateStr) -> void {
        UNUSED_PARAM(customData);
        UNUSED_PARAM(candidateStr);
    };

    auto onFrameHandler = [](UINT64 customData, PFrame pFrame) -> void {
        NoLostFramesContext* ctx = (NoLostFramesContext*) customData;
        if (pFrame->frameData[0] == 1) {
            ctx->receivedDecodingTs = pFrame->decodingTs;
            ATOMIC_STORE_BOOL(&ctx->seenFirstFrame, 1);
        }
    };
    EXPECT_EQ(transceiverOnFrame(answerVideoTransceiver, (UINT64) &frameCtx, onFrameHandler), STATUS_SUCCESS);

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) &answer, onICECandidateHdlr));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) &offer, onICECandidateHdlr));

    auto onICEConnectionStateChangeHdlr = [](UINT64 customData, RTC_PEER_CONNECTION_STATE newState) -> void {
        Context* pContext = (Context*) customData;

        if (newState == RTC_PEER_CONNECTION_STATE_CONNECTED) {
            ATOMIC_STORE_BOOL(&pContext->done, TRUE);
            CVAR_SIGNAL(pContext->cvar);
        }
    };

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnConnectionStateChange(offerPc, (UINT64) &context, onICEConnectionStateChangeHdlr));

    EXPECT_EQ(STATUS_SUCCESS, createOffer(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(offerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(answerPc, &sdp));

    EXPECT_EQ(STATUS_SUCCESS, createAnswer(answerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setLocalDescription(answerPc, &sdp));
    EXPECT_EQ(STATUS_SUCCESS, setRemoteDescription(offerPc, &sdp));

    MUTEX_LOCK(context.mutex);
    while (!ATOMIC_LOAD_BOOL(&context.done)) {
        CVAR_WAIT(context.cvar, context.mutex, INFINITE_TIME_VALUE);
    }
    MUTEX_UNLOCK(context.mutex);

    for (BYTE i = 1; i <= 3; i++) {
        videoFrame.frameData[0] = i;
        EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        videoFrame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 25);
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND / 25);
    }

    for (auto i = 0; i <= 1000 && !ATOMIC_LOAD_BOOL(&frameCtx.seenFirstFrame); i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    this->lock.lock();
    for (auto& th : this->threads)
        th.join();

    this->threads.clear();
    this->noNewThreads = TRUE;
    this->lock.unlock();

    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(offerPc, (UINT64) 0, onICECandidateHdlrDone));
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnIceCandidate(answerPc, (UINT64) 0, onICECandidateHdlrDone));

    MEMFREE(videoFrame.frameData);
    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    CVAR_FREE(context.cvar);
    MUTEX_FREE(context.mutex);

    EXPECT_EQ(ATOMIC_LOAD_BOOL(&frameCtx.seenFirstFrame), TRUE);

    // Verify timestamp conversion: first frame was sent with presentationTs = HUNDREDS_OF_NANOS_IN_A_SECOND
    // With correct conversion, received decodingTs should match (not be ~90x too large)
    EXPECT_EQ(frameCtx.receivedDecodingTs, HUNDREDS_OF_NANOS_IN_A_SECOND);
}

// Assert that two PeerConnections can connect and then send media until the receiver gets both audio/video
TEST_F(PeerConnectionFunctionalityTest, exchangeMedia)
{
    auto const frameBufferSize = 200000;

    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack, offerAudioTrack, answerAudioTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver, offerAudioTransceiver, answerAudioTransceiver;
    struct ExchangeMediaFrameContext {
        SIZE_T seenVideo;
        UINT64 receivedDecodingTs;
        UINT64 receivedPresentationTs;
    };
    ExchangeMediaFrameContext frameCtx;
    MEMSET(&frameCtx, 0, SIZEOF(frameCtx));
    Frame videoFrame;

    initRtcConfiguration(&configuration);
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));

    videoFrame.frameData = (PBYTE) MEMALLOC(frameBufferSize);
    videoFrame.size = TEST_VIDEO_FRAME_SIZE;
    MEMSET(videoFrame.frameData, 0x11, videoFrame.size);
    videoFrame.presentationTs = HUNDREDS_OF_NANOS_IN_A_SECOND;

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(offerPc, &offerAudioTrack, &offerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerAudioTrack, &answerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);

    auto onFrameHandler = [](UINT64 customData, PFrame pFrame) -> void {
        ExchangeMediaFrameContext* ctx = (ExchangeMediaFrameContext*) customData;
        ctx->receivedDecodingTs = pFrame->decodingTs;
        ctx->receivedPresentationTs = pFrame->presentationTs;
        ATOMIC_INCREMENT((PSIZE_T) &ctx->seenVideo);
    };
    EXPECT_EQ(transceiverOnFrame(answerVideoTransceiver, (UINT64) &frameCtx, onFrameHandler), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    // send 2 frames, receiver should see at least 1 frames
    for (int i = 0; i < 2; i++) {
        EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        videoFrame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 25);
        THREAD_SLEEP(40 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    DLOGI("framesSent");
    // Wait for receiver to see at least 1 frame
    // exact number of frames depends on timing
    for (auto i = 0; i <= 1000 && ATOMIC_LOAD(&frameCtx.seenVideo) < 2; i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }
    DLOGI("framesReceived %zu", ATOMIC_LOAD(&frameCtx.seenVideo));

    MEMFREE(videoFrame.frameData);
    RtcOutboundRtpStreamStats stats{};
    EXPECT_EQ(STATUS_SUCCESS, getRtpOutboundStats(offerPc, offerVideoTransceiver, &stats));
    EXPECT_EQ(206, stats.sent.packetsSent);
#ifdef KVS_USE_MBEDTLS
    EXPECT_EQ(248026, stats.sent.bytesSent);
#else
    EXPECT_EQ(246790, stats.sent.bytesSent);
#endif
    EXPECT_EQ(2, stats.framesSent);
    // Each RTP packet header: 12 bytes base + 8 bytes TWCC extension = 20 bytes
    EXPECT_EQ(4120, stats.headerBytesSent);
    EXPECT_LT(0, stats.lastPacketSentTimestamp);

    RtcInboundRtpStreamStats answerStats{};
    EXPECT_EQ(STATUS_SUCCESS, getRtpInboundStats(answerPc, answerVideoTransceiver, &answerStats));

    EXPECT_LE(1, ATOMIC_LOAD(&frameCtx.seenVideo));
    EXPECT_LE(1, answerStats.framesReceived);
    EXPECT_LT(103, answerStats.received.packetsReceived);
    EXPECT_LT(120000, answerStats.bytesReceived);
    EXPECT_LT(1234, answerStats.headerBytesReceived);
    EXPECT_LT(0, answerStats.lastPacketReceivedTimestamp);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    // Verify timestamp conversion: received timestamps should be in the correct range
    // Sent timestamps start at HUNDREDS_OF_NANOS_IN_A_SECOND (1s) with 25fps increments
    // With the old bug (treating RTP clock rate units as ms), they'd be ~90x too large
    EXPECT_GE(frameCtx.receivedDecodingTs, HUNDREDS_OF_NANOS_IN_A_SECOND);
    EXPECT_LE(frameCtx.receivedDecodingTs, (UINT64) 50 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    EXPECT_EQ(frameCtx.receivedDecodingTs, frameCtx.receivedPresentationTs);
}

// Same test as exchangeMedia, but assert that if one side is RSA DTLS and Key Extraction works
TEST_F(PeerConnectionFunctionalityTest, exchangeMediaRSA)
{
    auto const frameBufferSize = 200000;

    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack, offerAudioTrack, answerAudioTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver, offerAudioTransceiver, answerAudioTransceiver;
    SIZE_T seenVideo = 0;
    Frame videoFrame;

    initRtcConfiguration(&configuration);
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));

    videoFrame.frameData = (PBYTE) MEMALLOC(frameBufferSize);
    videoFrame.size = TEST_VIDEO_FRAME_SIZE;
    MEMSET(videoFrame.frameData, 0x11, videoFrame.size);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    configuration.kvsRtcConfiguration.generateRSACertificate = TRUE;
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(offerPc, &offerAudioTrack, &offerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerAudioTrack, &answerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);

    auto onFrameHandler = [](UINT64 customData, PFrame pFrame) -> void {
        UNUSED_PARAM(pFrame);
        ATOMIC_STORE((PSIZE_T) customData, 1);
    };
    EXPECT_EQ(transceiverOnFrame(answerVideoTransceiver, (UINT64) &seenVideo, onFrameHandler), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    for (auto i = 0; i <= 1000 && ATOMIC_LOAD(&seenVideo) != 1; i++) {
        EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        videoFrame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 25);

        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    MEMFREE(videoFrame.frameData);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    EXPECT_EQ(ATOMIC_LOAD(&seenVideo), 1);
}

TEST_F(PeerConnectionFunctionalityTest, iceRestartTest)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    EXPECT_EQ(restartIce(offerPc), STATUS_SUCCESS);

    /* reset state change count */
    MEMSET(&stateChangeCount, 0x00, SIZEOF(stateChangeCount));

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

TEST_F(PeerConnectionFunctionalityTest, DISABLED_iceRestartTestForcedTurn)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    ASSERT_EQ(TRUE, mAccessKeyIdSet);

    initRtcConfiguration(&configuration);
    configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_RELAY;

    initializeSignalingClient();
    getIceServers(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    EXPECT_EQ(restartIce(offerPc), STATUS_SUCCESS);

    /* reset state change count */
    MEMSET(&stateChangeCount, 0x00, SIZEOF(stateChangeCount));

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    deinitializeSignalingClient();
}

TEST_F(PeerConnectionFunctionalityTest, peerConnectionOfferCloseConnection)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    initRtcConfiguration(&configuration);

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    closePeerConnection(offerPc);
    EXPECT_EQ(ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_CLOSED]), 2);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}

TEST_F(PeerConnectionFunctionalityTest, peerConnectionAnswerCloseConnection)
{
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    initRtcConfiguration(&configuration);

    initializeSignalingClient();

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    closePeerConnection(answerPc);
    EXPECT_EQ(ATOMIC_LOAD(&stateChangeCount[RTC_PEER_CONNECTION_STATE_CLOSED]), 2);
    closePeerConnection(offerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    deinitializeSignalingClient();
}

TEST_F(PeerConnectionFunctionalityTest, DISABLED_exchangeMediaThroughTurnRandomStop)
{
    ASSERT_EQ(TRUE, mAccessKeyIdSet);

    initializeSignalingClient();

    auto repeatedStreamingRandomStop = [this](int iteration, int maxStreamingDurationMs, int minStreamingDurationMs, bool expectSeenVideo) -> void {
        auto const frameBufferSize = 200000;
        Frame videoFrame;
        PRtcPeerConnection offerPc = NULL, answerPc = NULL;
        RtcMediaStreamTrack offerVideoTrack, answerVideoTrack, offerAudioTrack, answerAudioTrack;
        PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver, offerAudioTransceiver, answerAudioTransceiver;
        ATOMIC_BOOL offerSeenVideo = 0, answerSeenVideo = 0, offerStopVideo = 0, answerStopVideo = 0;
        UINT64 streamingTimeMs;
        RtcConfiguration configuration{};

        MEMSET(&videoFrame, 0x00, SIZEOF(Frame));
        videoFrame.frameData = (PBYTE) MEMALLOC(frameBufferSize);
        videoFrame.size = TEST_VIDEO_FRAME_SIZE;
        MEMSET(videoFrame.frameData, 0x11, videoFrame.size);

        for (int i = 0; i < iteration; ++i) {
            initRtcConfiguration(&configuration);
            configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_RELAY;
            getIceServers(&configuration);

            EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
            EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

            addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
            addTrackToPeerConnection(offerPc, &offerAudioTrack, &offerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);
            addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
            addTrackToPeerConnection(answerPc, &answerAudioTrack, &answerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);

            auto onFrameHandler = [](UINT64 customData, PFrame pFrame) -> void {
                UNUSED_PARAM(pFrame);
                ATOMIC_STORE_BOOL((PSIZE_T) customData, TRUE);
            };
            EXPECT_EQ(transceiverOnFrame(offerVideoTransceiver, (UINT64) &offerSeenVideo, onFrameHandler), STATUS_SUCCESS);
            EXPECT_EQ(transceiverOnFrame(answerVideoTransceiver, (UINT64) &answerSeenVideo, onFrameHandler), STATUS_SUCCESS);

            MEMSET(stateChangeCount, 0x00, SIZEOF(stateChangeCount));
            EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

            streamingTimeMs = (UINT64) (RAND() % (maxStreamingDurationMs - minStreamingDurationMs)) + minStreamingDurationMs;
            DLOGI("Stop streaming after %u milliseconds.", streamingTimeMs);

            auto sendVideoWorker = [](PRtcRtpTransceiver pRtcRtpTransceiver, Frame frame, PSIZE_T pTerminationFlag) -> void {
                while (!ATOMIC_LOAD_BOOL(pTerminationFlag)) {
                    EXPECT_EQ(writeFrame(pRtcRtpTransceiver, &frame), STATUS_SUCCESS);
                    // frame was copied by value
                    frame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 25);

                    THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
                }
            };

            std::thread offerSendVideoWorker(sendVideoWorker, offerVideoTransceiver, videoFrame, &offerStopVideo);
            std::thread answerSendVideoWorker(sendVideoWorker, answerVideoTransceiver, videoFrame, &answerStopVideo);

            std::this_thread::sleep_for(std::chrono::milliseconds(streamingTimeMs));

            ATOMIC_STORE_BOOL(&offerStopVideo, TRUE);
            offerSendVideoWorker.join();
            freePeerConnection(&offerPc);

            ATOMIC_STORE_BOOL(&answerStopVideo, TRUE);
            answerSendVideoWorker.join();
            freePeerConnection(&answerPc);

            if (expectSeenVideo) {
                EXPECT_EQ(ATOMIC_LOAD_BOOL(&offerSeenVideo), TRUE);
                EXPECT_EQ(ATOMIC_LOAD_BOOL(&answerSeenVideo), TRUE);
            }
        }

        MEMFREE(videoFrame.frameData);
    };

    // Repeated steaming and stop at random times to catch potential deadlocks involving iceAgent and TurnConnection
    repeatedStreamingRandomStop(30, 5000, 1000, TRUE);
    repeatedStreamingRandomStop(30, 1000, 500, FALSE);

    deinitializeSignalingClient();
}

// Check that even when multiple successful candidate pairs are found, only one dtls negotiation takes place
// and that it is on the same candidate throughout the connection.
TEST_F(PeerConnectionFunctionalityTest, multipleCandidateSuccessOneDTLSCheck)
{
    RtcConfiguration configuration;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    // This test can succeed if the highest priority candidate pair happens to be the first one
    // to be nominated, even if the DTLS is broken. To be sure that this issue is fixed we want to
    // run the test 10 times and have it never break once in that cycle.
    for (auto i = 0; i < 10; i++) {
        offerPc = NULL;
        answerPc = NULL;
        initRtcConfiguration(&configuration);

        EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
        EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

        // create a callback that can check values at every state of the ice agent state machine
        auto masterOnIceConnectionStateChangeTest = [](UINT64 customData, UINT64 connectionState) -> void {
            static PIceCandidatePair pSendingPair;
            PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
            // still use normal callback
            onIceConnectionStateChange(customData, connectionState);
            switch (connectionState) {
                case ICE_AGENT_STATE_CHECK_CONNECTION:
                    // sleep(1);
                    break;
                case ICE_AGENT_STATE_CONNECTED:
                    if (pKvsPeerConnection->pIceAgent->pDataSendingIceCandidatePair != NULL) {
                        pSendingPair = pKvsPeerConnection->pIceAgent->pDataSendingIceCandidatePair;
                    }
                    break;
                case ICE_AGENT_STATE_READY:
                    if (pSendingPair != NULL) {
                        EXPECT_EQ(pSendingPair, pKvsPeerConnection->pIceAgent->pDataSendingIceCandidatePair);
                        pSendingPair = NULL;
                    }
                    break;
                default:
                    break;
            }
        };

        auto viewerOnIceConnectionStateChangeTest = [](UINT64 customData, UINT64 connectionState) -> void {
            PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
            PIceAgent pIceAgent = pKvsPeerConnection->pIceAgent;
            PDoubleListNode pCurNode = NULL;
            PIceCandidatePair pIceCandidatePair;
            BOOL locked = FALSE;
            // still use normal callback
            onIceConnectionStateChange(customData, connectionState);
            switch (connectionState) {
                case ICE_AGENT_STATE_CONNECTED:
                    // send 'USE_CANDIDATE' for every ice candidate pair
                    MUTEX_LOCK(pIceAgent->lock);
                    locked = TRUE;
                    doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode);
                    while (pCurNode != NULL) {
                        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
                        pCurNode = pCurNode->pNext;

                        pIceCandidatePair->nominated = TRUE;
                    }
                    if (locked) {
                        MUTEX_UNLOCK(pIceAgent->lock);
                    }

                    break;
                default:
                    break;
            }
        };

        // overwrite normal callback
        ((PKvsPeerConnection) answerPc)->pIceAgent->iceAgentCallbacks.connectionStateChangedFn = masterOnIceConnectionStateChangeTest;
        ((PKvsPeerConnection) offerPc)->pIceAgent->iceAgentCallbacks.connectionStateChangedFn = viewerOnIceConnectionStateChangeTest;

        EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

        closePeerConnection(offerPc);
        closePeerConnection(answerPc);

        freePeerConnection(&offerPc);
        freePeerConnection(&answerPc);
        MEMSET(this->stateChangeCount, 0, SIZEOF(SIZE_T) * RTC_PEER_CONNECTION_TOTAL_STATE_COUNT);
        if (::testing::Test::HasFailure()) {
            break;
        }
    }
}

// Check that even when multiple successful candidate pairs are found, only one dtls negotiation takes place
// and that it is on the same candidate throughout the connection. This time setting the viewer to use
// aggressive nomination
TEST_F(PeerConnectionFunctionalityTest, aggressiveNominationDTLSRaceConditionCheck)
{
    RtcConfiguration configuration;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;

    // This test can succeed if the highest priority candidate pair happens to be the first one
    // to be nominated, even if the DTLS is broken. To be sure that this issue is fixed we want to
    // run the test 10 times and have it never break once in that cycle.
    for (auto i = 0; i < 10; i++) {
        offerPc = NULL;
        answerPc = NULL;
        initRtcConfiguration(&configuration);

        EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
        EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

        // create a callback that can check values at every state of the ice agent state machine
        auto masterOnIceConnectionStateChangeTest = [](UINT64 customData, UINT64 connectionState) -> void {
            static PIceCandidatePair pSendingPair;
            PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
            // still use normal callback
            onIceConnectionStateChange(customData, connectionState);
            switch (connectionState) {
                case ICE_AGENT_STATE_CHECK_CONNECTION:
                    // sleep(1);
                    break;
                case ICE_AGENT_STATE_CONNECTED:
                    if (pKvsPeerConnection->pIceAgent->pDataSendingIceCandidatePair != NULL) {
                        pSendingPair = pKvsPeerConnection->pIceAgent->pDataSendingIceCandidatePair;
                    }
                    break;
                case ICE_AGENT_STATE_READY:
                    if (pSendingPair != NULL) {
                        EXPECT_EQ(pSendingPair, pKvsPeerConnection->pIceAgent->pDataSendingIceCandidatePair);
                        pSendingPair = NULL;
                    }
                    break;
                default:
                    break;
            }
        };

        auto viewerOnIceConnectionStateChangeTest = [](UINT64 customData, UINT64 connectionState) -> void {
            static BOOL setUseCandidate = FALSE;
            PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
            PIceAgent pIceAgent = pKvsPeerConnection->pIceAgent;
            PDoubleListNode pCurNode = NULL;
            PIceCandidatePair pIceCandidatePair;
            BOOL locked = FALSE;
            // still use normal callback
            onIceConnectionStateChange(customData, connectionState);
            switch (connectionState) {
                case ICE_AGENT_STATE_CHECK_CONNECTION:
                    MUTEX_LOCK(pIceAgent->lock);
                    locked = TRUE;
                    if (!setUseCandidate) {
                        setUseCandidate = TRUE;
                        appendStunFlagAttribute(pIceAgent->pBindingRequest, STUN_ATTRIBUTE_TYPE_USE_CANDIDATE);
                    }
                    doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode);
                    while (pCurNode != NULL) {
                        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
                        pCurNode = pCurNode->pNext;

                        pIceCandidatePair->nominated = TRUE;
                        iceCandidatePairCheckConnection(pIceAgent->pBindingRequest, pIceAgent, pIceCandidatePair);
                    }
                    if (locked) {
                        MUTEX_UNLOCK(pIceAgent->lock);
                    }
                    break;
                case ICE_AGENT_STATE_CONNECTED:
                    // send 'USE_CANDIDATE' for every ice candidate pair
                    setUseCandidate = FALSE;
                    MUTEX_LOCK(pIceAgent->lock);
                    locked = TRUE;
                    doubleListGetHeadNode(pIceAgent->iceCandidatePairs, &pCurNode);
                    while (pCurNode != NULL) {
                        pIceCandidatePair = (PIceCandidatePair) pCurNode->data;
                        pCurNode = pCurNode->pNext;

                        pIceCandidatePair->nominated = TRUE;
                    }
                    if (locked) {
                        MUTEX_UNLOCK(pIceAgent->lock);
                    }

                    break;
                default:
                    break;
            }
        };

        // overwrite normal callback
        ((PKvsPeerConnection) answerPc)->pIceAgent->iceAgentCallbacks.connectionStateChangedFn = masterOnIceConnectionStateChangeTest;
        ((PKvsPeerConnection) offerPc)->pIceAgent->iceAgentCallbacks.connectionStateChangedFn = viewerOnIceConnectionStateChangeTest;

        EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

        closePeerConnection(offerPc);
        closePeerConnection(answerPc);

        freePeerConnection(&offerPc);
        freePeerConnection(&answerPc);
        MEMSET(this->stateChangeCount, 0, SIZEOF(SIZE_T) * RTC_PEER_CONNECTION_TOTAL_STATE_COUNT);
        if (::testing::Test::HasFailure()) {
            break;
        }
    }
}

// Test PLI (Picture Loss Indication) functionality
// Sender sends I-frames (filled with 'I') and P-frames (filled with 'P')
// When receiver gets a P-frame, it sends a PLI request
// Sender responds to PLI by sending a special i-frame (filled with lowercase 'i')
// Test passes when receiver gets the lowercase 'i' frame
TEST_F(PeerConnectionFunctionalityTest, pliRequestTriggersKeyFrame)
{
    // Frame size for test frames
    auto const frameBufferSize = 42;

    // Frame type markers
    const BYTE IFRAME_MARKER = 'I';     // Regular I-frame
    const BYTE PFRAME_MARKER = 'P';     // P-frame
    const BYTE PLI_IFRAME_MARKER = 'i'; // I-frame sent in response to PLI

    // Context structure for sharing state between callbacks
    struct PliTestContext {
        PRtcRtpTransceiver pSenderTransceiver;
        ATOMIC_BOOL pliReceived;
        ATOMIC_BOOL pliResponseFrameReceived;
        ATOMIC_BOOL pFrameReceived;
        ATOMIC_BOOL testComplete;
    };

    RtcConfiguration configuration;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver;
    Frame videoFrame;
    PliTestContext context;

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));
    MEMSET(&context, 0x00, SIZEOF(PliTestContext));

    // Allocate frame buffer
    videoFrame.frameData = (PBYTE) MEMALLOC(frameBufferSize);
    videoFrame.size = frameBufferSize;
    ASSERT_NE(videoFrame.frameData, nullptr);

    // Initialize atomic flags
    ATOMIC_STORE_BOOL(&context.pliReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.pliResponseFrameReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.pFrameReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.testComplete, FALSE);

    // Create peer connections
    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    // Add video tracks to both peers
    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);

    context.pSenderTransceiver = offerVideoTransceiver;

    // Callback for when sender (offer) receives PLI from receiver
    auto onPictureLossHandler = [](UINT64 customData) -> void {
        PliTestContext* pContext = (PliTestContext*) customData;
        ATOMIC_STORE_BOOL(&pContext->pliReceived, TRUE);
        DLOGD("PLI received by sender");
    };

    // Register PLI callback on sender's transceiver
    EXPECT_EQ(transceiverOnPictureLoss(offerVideoTransceiver, (UINT64) &context, onPictureLossHandler), STATUS_SUCCESS);

    // Callback for when receiver (answer) gets a frame
    // If it's a P-frame, send PLI; if it's lowercase 'i', the test passes
    auto onFrameHandler = [](UINT64 customData, PFrame pFrame) -> void {
        PliTestContext* pContext = (PliTestContext*) customData;

        if (pFrame == NULL || pFrame->frameData == NULL || pFrame->size == 0) {
            return;
        }

        BYTE frameMarker = pFrame->frameData[0];

        if (frameMarker == 'P' && !ATOMIC_LOAD_BOOL(&pContext->pFrameReceived)) {
            // First P-frame received, send PLI
            ATOMIC_STORE_BOOL(&pContext->pFrameReceived, TRUE);
            DLOGD("P-frame received, sending PLI");
            // Note: We'll send PLI from the main test loop after detecting pFrameReceived
        } else if (frameMarker == 'i') {
            // Received the I-frame sent in response to PLI
            ATOMIC_STORE_BOOL(&pContext->pliResponseFrameReceived, TRUE);
            ATOMIC_STORE_BOOL(&pContext->testComplete, TRUE);
            DLOGD("PLI response I-frame received (lowercase 'i')");
        }
    };

    // Register frame callback on receiver's transceiver
    EXPECT_EQ(transceiverOnFrame(answerVideoTransceiver, (UINT64) &context, onFrameHandler), STATUS_SUCCESS);

    // Connect the two peers
    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    // Send frames: I, P, P... until we get PLI response
    BOOL sentPliResponseFrame = FALSE;
    BOOL pliSent = FALSE;

    for (auto i = 0; i < 200 && !ATOMIC_LOAD_BOOL(&context.testComplete); i++) {
        // Check if we received PLI and need to send response I-frame
        if (ATOMIC_LOAD_BOOL(&context.pliReceived) && !sentPliResponseFrame) {
            // Send I-frame in response to PLI (lowercase 'i')
            MEMSET(videoFrame.frameData, PLI_IFRAME_MARKER, videoFrame.size);
            videoFrame.flags = FRAME_FLAG_KEY_FRAME;
            EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
            sentPliResponseFrame = TRUE;
            DLOGD("Sent PLI response I-frame (lowercase 'i')");
        } else if (ATOMIC_LOAD_BOOL(&context.pFrameReceived) && !pliSent) {
            // Receiver got P-frame, now send PLI from receiver
            EXPECT_EQ(transceiverSendPli(answerVideoTransceiver), STATUS_SUCCESS);
            pliSent = TRUE;
            DLOGD("PLI sent from receiver");
        } else if (i == 0) {
            // First frame: send I-frame
            MEMSET(videoFrame.frameData, IFRAME_MARKER, videoFrame.size);
            videoFrame.flags = FRAME_FLAG_KEY_FRAME;
            EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
            DLOGD("Sent I-frame");
        } else if (!ATOMIC_LOAD_BOOL(&context.pFrameReceived)) {
            // Send P-frames until receiver gets one
            MEMSET(videoFrame.frameData, PFRAME_MARKER, videoFrame.size);
            videoFrame.flags = FRAME_FLAG_NONE;
            EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        }

        videoFrame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 25);
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND * 10);
    }

    // Cleanup
    MEMFREE(videoFrame.frameData);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    // Verify test succeeded
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.pFrameReceived)) << "Receiver should have received a P-frame";
    EXPECT_TRUE(pliSent) << "PLI should have been sent";
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.pliReceived)) << "Sender should have received PLI";
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.pliResponseFrameReceived)) << "Receiver should have received the I-frame sent in response to PLI";
}

// Test FIR (Full Intra Request) functionality - similar to PLI test
// Sender sends I-frames (filled with 'I') and P-frames (filled with 'P')
// When receiver gets a P-frame, it sends a FIR request
// Sender responds to FIR by sending a special i-frame (filled with lowercase 'i')
// Test passes when receiver gets the lowercase 'i' frame
TEST_F(PeerConnectionFunctionalityTest, firRequestTriggersKeyFrame)
{
    // Frame size for test frames
    auto const frameBufferSize = 42;

    // Frame type markers
    const BYTE IFRAME_MARKER = 'I';     // Regular I-frame
    const BYTE PFRAME_MARKER = 'P';     // P-frame
    const BYTE FIR_IFRAME_MARKER = 'i'; // I-frame sent in response to FIR

    // Context structure for sharing state between callbacks
    struct FirTestContext {
        PRtcRtpTransceiver pSenderTransceiver;
        ATOMIC_BOOL firReceived;
        ATOMIC_BOOL firResponseFrameReceived;
        ATOMIC_BOOL pFrameReceived;
        ATOMIC_BOOL testComplete;
    };

    RtcConfiguration configuration;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver;
    Frame videoFrame;
    FirTestContext context;

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));
    MEMSET(&context, 0x00, SIZEOF(FirTestContext));

    // Allocate frame buffer
    videoFrame.frameData = (PBYTE) MEMALLOC(frameBufferSize);
    videoFrame.size = frameBufferSize;
    ASSERT_NE(videoFrame.frameData, nullptr);

    // Initialize atomic flags
    ATOMIC_STORE_BOOL(&context.firReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.firResponseFrameReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.pFrameReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.testComplete, FALSE);

    // Create peer connections
    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    // Add video tracks to both peers
    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);

    context.pSenderTransceiver = offerVideoTransceiver;

    // Callback for when sender (offer) receives FIR from receiver (uses same onPictureLoss callback)
    auto onPictureLossHandler = [](UINT64 customData) -> void {
        FirTestContext* pContext = (FirTestContext*) customData;
        ATOMIC_STORE_BOOL(&pContext->firReceived, TRUE);
        DLOGD("FIR received by sender (via onPictureLoss callback)");
    };

    // Register PLI/FIR callback on sender's transceiver
    EXPECT_EQ(transceiverOnPictureLoss(offerVideoTransceiver, (UINT64) &context, onPictureLossHandler), STATUS_SUCCESS);

    // Callback for when receiver (answer) gets a frame
    auto onFrameHandler = [](UINT64 customData, PFrame pFrame) -> void {
        FirTestContext* pContext = (FirTestContext*) customData;

        if (pFrame == NULL || pFrame->frameData == NULL || pFrame->size == 0) {
            return;
        }

        BYTE frameMarker = pFrame->frameData[0];

        if (frameMarker == 'P' && !ATOMIC_LOAD_BOOL(&pContext->pFrameReceived)) {
            // First P-frame received, will trigger FIR from main loop
            ATOMIC_STORE_BOOL(&pContext->pFrameReceived, TRUE);
            DLOGD("P-frame received, will send FIR");
        } else if (frameMarker == 'i') {
            // Received the I-frame sent in response to FIR
            ATOMIC_STORE_BOOL(&pContext->firResponseFrameReceived, TRUE);
            ATOMIC_STORE_BOOL(&pContext->testComplete, TRUE);
            DLOGD("FIR response I-frame received (lowercase 'i')");
        }
    };

    // Register frame callback on receiver's transceiver
    EXPECT_EQ(transceiverOnFrame(answerVideoTransceiver, (UINT64) &context, onFrameHandler), STATUS_SUCCESS);

    // Connect the two peers
    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    // Send frames: I, P, P... until we get FIR response
    BOOL sentFirResponseFrame = FALSE;
    BOOL firSent = FALSE;

    for (auto i = 0; i < 200 && !ATOMIC_LOAD_BOOL(&context.testComplete); i++) {
        // Check if we received FIR and need to send response I-frame
        if (ATOMIC_LOAD_BOOL(&context.firReceived) && !sentFirResponseFrame) {
            // Send I-frame in response to FIR (lowercase 'i')
            MEMSET(videoFrame.frameData, FIR_IFRAME_MARKER, videoFrame.size);
            videoFrame.flags = FRAME_FLAG_KEY_FRAME;
            EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
            sentFirResponseFrame = TRUE;
            DLOGD("Sent FIR response I-frame (lowercase 'i')");
        } else if (ATOMIC_LOAD_BOOL(&context.pFrameReceived) && !firSent) {
            // Receiver got P-frame, now send FIR from receiver
            EXPECT_EQ(transceiverSendFir(answerVideoTransceiver), STATUS_SUCCESS);
            firSent = TRUE;
            DLOGD("FIR sent from receiver");
        } else if (i == 0) {
            // First frame: send I-frame
            MEMSET(videoFrame.frameData, IFRAME_MARKER, videoFrame.size);
            videoFrame.flags = FRAME_FLAG_KEY_FRAME;
            EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
            DLOGD("Sent I-frame");
        } else if (!ATOMIC_LOAD_BOOL(&context.pFrameReceived)) {
            // Send P-frames until receiver gets one
            MEMSET(videoFrame.frameData, PFRAME_MARKER, videoFrame.size);
            videoFrame.flags = FRAME_FLAG_NONE;
            EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        }

        videoFrame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 25);
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND * 10);
    }

    // Cleanup
    MEMFREE(videoFrame.frameData);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    // Verify test succeeded
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.pFrameReceived)) << "Receiver should have received a P-frame";
    EXPECT_TRUE(firSent) << "FIR should have been sent";
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.firReceived)) << "Sender should have received FIR";
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.firResponseFrameReceived)) << "Receiver should have received the I-frame sent in response to FIR";
}

// Test TWCC (Transport-Wide Congestion Control) feedback functionality
// Sender sends video frames to receiver
// Receiver generates TWCC feedback for incoming RTP packets
// When sender receives TWCC feedback, RtcOnSenderBandwidthEstimation callback is fired
TEST_F(PeerConnectionFunctionalityTest, twccFeedbackTriggersBandwidthEstimation)
{
    // Frame size for test frames
    auto const frameBufferSize = 1200;

    // Context structure for sharing state between callbacks
    struct TwccTestContext {
        ATOMIC_BOOL bandwidthEstimationReceived;
        ATOMIC_BOOL frameReceived;
        ATOMIC_BOOL testComplete;
        SIZE_T callbackCount;
        UINT32 totalTxBytes;
        UINT32 totalRxBytes;
        UINT32 totalTxPackets;
        UINT32 totalRxPackets;
    };

    RtcConfiguration configuration;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver;
    Frame videoFrame;
    TwccTestContext context;

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));
    MEMSET(&context, 0x00, SIZEOF(TwccTestContext));

    // Allocate frame buffer
    videoFrame.frameData = (PBYTE) MEMALLOC(frameBufferSize);
    videoFrame.size = frameBufferSize;
    ASSERT_NE(videoFrame.frameData, nullptr);
    MEMSET(videoFrame.frameData, 0xAB, videoFrame.size);

    // Initialize atomic flags
    ATOMIC_STORE_BOOL(&context.bandwidthEstimationReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.frameReceived, FALSE);
    ATOMIC_STORE_BOOL(&context.testComplete, FALSE);
    context.callbackCount = 0;

    // Create peer connections
    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    // Manually enable TWCC extension on both peers
    // In real scenarios, this is negotiated via SDP when connecting to browsers
    // For peer-to-peer testing between SDK instances, we need to set it manually
    PKvsPeerConnection pOfferKvs = (PKvsPeerConnection) offerPc;
    PKvsPeerConnection pAnswerKvs = (PKvsPeerConnection) answerPc;
    pOfferKvs->twccExtId = 1; // Standard TWCC extension ID
    pAnswerKvs->twccExtId = 1;

    // Add video tracks to both peers (VP8 codec supports TWCC)
    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);

    // Callback for when sender receives TWCC feedback and computes bandwidth estimation
    auto onBandwidthEstimationHandler = [](UINT64 customData, UINT32 txBytes, UINT32 rxBytes, UINT32 txPackets, UINT32 rxPackets,
                                           UINT64 duration) -> void {
        UNUSED_PARAM(duration);
        TwccTestContext* pContext = (TwccTestContext*) customData;

        pContext->callbackCount++;
        pContext->totalTxBytes += txBytes;
        pContext->totalRxBytes += rxBytes;
        pContext->totalTxPackets += txPackets;
        pContext->totalRxPackets += rxPackets;

        ATOMIC_STORE_BOOL(&pContext->bandwidthEstimationReceived, TRUE);
        DLOGD("Bandwidth estimation callback: txBytes=%u rxBytes=%u txPackets=%u rxPackets=%u", txBytes, rxBytes, txPackets, rxPackets);

        // Mark test complete if we've received enough callbacks
        if (pContext->callbackCount >= 2) {
            ATOMIC_STORE_BOOL(&pContext->testComplete, TRUE);
        }
    };

    // Register bandwidth estimation callback on sender (offer peer)
    EXPECT_EQ(peerConnectionOnSenderBandwidthEstimation(offerPc, (UINT64) &context, onBandwidthEstimationHandler), STATUS_SUCCESS);

    // Callback for when receiver gets a frame
    auto onFrameHandler = [](UINT64 customData, PFrame pFrame) -> void {
        UNUSED_PARAM(pFrame);
        TwccTestContext* pContext = (TwccTestContext*) customData;
        ATOMIC_STORE_BOOL(&pContext->frameReceived, TRUE);
    };

    // Register frame callback on receiver's transceiver
    EXPECT_EQ(transceiverOnFrame(answerVideoTransceiver, (UINT64) &context, onFrameHandler), STATUS_SUCCESS);

    // Connect the two peers
    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    // Verify TWCC extension ID is set
    DLOGD("Offer TWCC ext ID: %u, Answer TWCC ext ID: %u", pOfferKvs->twccExtId, pAnswerKvs->twccExtId);

    // Send video frames from offer to answer
    // TWCC feedback is generated every ~100ms, so we need to send for a while
    for (auto i = 0; i < 300 && !ATOMIC_LOAD_BOOL(&context.testComplete); i++) {
        // Send video frame
        videoFrame.flags = (i % 30 == 0) ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
        EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        videoFrame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 30); // 30 fps

        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND * 33); // ~30fps timing
    }

    // Cleanup
    MEMFREE(videoFrame.frameData);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    // Verify test succeeded
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.frameReceived)) << "Receiver should have received video frames";
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.bandwidthEstimationReceived))
        << "Sender should have received TWCC feedback and bandwidth estimation callback should have been fired";
    EXPECT_GT(context.callbackCount, 0) << "Bandwidth estimation callback should have been called at least once";
    EXPECT_GT(context.totalTxPackets, 0) << "Should have reported transmitted packets";
    EXPECT_GT(context.totalRxPackets, 0) << "Should have reported received packets";

    DLOGD("TWCC test completed: %zu callbacks, txPackets=%u rxPackets=%u", context.callbackCount, context.totalTxPackets, context.totalRxPackets);
}

// Test TWCC feedback generation on the receiver side
// This verifies the receiver correctly generates TWCC feedback packets
// with proper incremental deltas (not absolute timestamps)
// Bug history: Chrome reported 300kbps stable bitrate because:
// 1. Delta calculation was absolute instead of incremental
// 2. 24-bit reference time wraparound caused delta overflow
// 3. First packet with relative time=0 was treated as "no packet found"
TEST_F(PeerConnectionFunctionalityTest, twccReceiverGeneratesFeedback)
{
    auto const frameBufferSize = 1200;

    struct TwccReceiverTestContext {
        ATOMIC_BOOL feedbackSent;
        SIZE_T feedbackCount;
        ATOMIC_BOOL testComplete;
        UINT32 totalTxPackets;
        UINT32 totalRxPackets;
        SIZE_T validDurationCount;
        SIZE_T invalidDurationCount;
    };

    RtcConfiguration configuration;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver;
    Frame videoFrame;
    TwccReceiverTestContext context;

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));
    MEMSET(&context, 0x00, SIZEOF(TwccReceiverTestContext));

    videoFrame.frameData = (PBYTE) MEMALLOC(frameBufferSize);
    videoFrame.size = frameBufferSize;
    ASSERT_NE(videoFrame.frameData, nullptr);
    MEMSET(videoFrame.frameData, 0xAB, videoFrame.size);

    ATOMIC_STORE_BOOL(&context.feedbackSent, FALSE);
    ATOMIC_STORE_BOOL(&context.testComplete, FALSE);
    context.feedbackCount = 0;
    context.totalTxPackets = 0;
    context.totalRxPackets = 0;
    context.validDurationCount = 0;
    context.invalidDurationCount = 0;

    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    // Enable TWCC on both peers
    PKvsPeerConnection pOfferKvs = (PKvsPeerConnection) offerPc;
    PKvsPeerConnection pAnswerKvs = (PKvsPeerConnection) answerPc;
    pOfferKvs->twccExtId = 1;
    pAnswerKvs->twccExtId = 1;

    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver, RTC_CODEC_VP8, MEDIA_STREAM_TRACK_KIND_VIDEO);

    // Bandwidth estimation callback on SENDER (offer) proves receiver sent feedback
    auto onBandwidthEstimationHandler = [](UINT64 customData, UINT32 txBytes, UINT32 rxBytes, UINT32 txPackets, UINT32 rxPackets,
                                           UINT64 duration) -> void {
        UNUSED_PARAM(txBytes);
        UNUSED_PARAM(rxBytes);
        TwccReceiverTestContext* pContext = (TwccReceiverTestContext*) customData;

        pContext->feedbackCount++;
        pContext->totalTxPackets += txPackets;
        pContext->totalRxPackets += rxPackets;
        ATOMIC_STORE_BOOL(&pContext->feedbackSent, TRUE);

        // Verify we're getting reasonable packet counts (not zero due to delta overflow)
        // This would fail with the old bugs where packets were marked as "lost"
        EXPECT_GT(rxPackets, 0) << "rxPackets should be > 0 (not marked as lost due to delta overflow)";

        // Verify duration is reasonable (not negative or huge due to delta overflow)
        // Duration is in 100ns units, should be positive and less than 10 seconds for each feedback
        INT64 signedDuration = (INT64) duration;
        if (signedDuration >= 0 && signedDuration <= 10 * HUNDREDS_OF_NANOS_IN_A_SECOND) {
            pContext->validDurationCount++;
        } else {
            pContext->invalidDurationCount++;
            DLOGW("Invalid TWCC duration: %lld (possible delta overflow)", (long long) signedDuration);
        }

        // Verify receive ratio is reasonable (> 50% of packets should be marked received)
        // With delta overflow bugs, most packets would be marked as "lost"
        if (txPackets > 0) {
            UINT32 receivePercent = (rxPackets * 100) / txPackets;
            EXPECT_GE(receivePercent, 50) << "Receive ratio should be >= 50% (was " << receivePercent
                                          << "%), low ratio indicates delta calculation bugs";
        }

        // After several feedbacks, mark complete
        if (pContext->feedbackCount >= 3) {
            ATOMIC_STORE_BOOL(&pContext->testComplete, TRUE);
        }
    };

    EXPECT_EQ(peerConnectionOnSenderBandwidthEstimation(offerPc, (UINT64) &context, onBandwidthEstimationHandler), STATUS_SUCCESS);

    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    // Verify TWCC receiver manager is initialized on answer peer
    EXPECT_NE(pAnswerKvs->pTwccReceiverManager, nullptr) << "TWCC receiver manager should be initialized";

    // Send frames - receiver (answer) should generate TWCC feedback
    for (auto i = 0; i < 300 && !ATOMIC_LOAD_BOOL(&context.testComplete); i++) {
        videoFrame.flags = (i % 30 == 0) ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
        EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        videoFrame.presentationTs += (HUNDREDS_OF_NANOS_IN_A_SECOND / 30);
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND * 33);
    }

    MEMFREE(videoFrame.frameData);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    // Verify receiver generated feedback
    EXPECT_TRUE(ATOMIC_LOAD_BOOL(&context.feedbackSent)) << "Receiver should have sent TWCC feedback";
    EXPECT_GE(context.feedbackCount, 3) << "Should have received multiple TWCC feedbacks";
    EXPECT_GT(context.totalRxPackets, 0) << "Should have reported received packets (not all marked lost due to delta bugs)";

    // Verify duration checks actually ran and ALL passed
    // Using total ensures we don't pass by doing nothing (0 == 0 would be meaningless)
    SIZE_T totalDurationChecks = context.validDurationCount + context.invalidDurationCount;
    EXPECT_GT(totalDurationChecks, 0) << "Should have performed duration checks (test did nothing if 0)";
    EXPECT_EQ(context.validDurationCount, totalDurationChecks) << "All duration checks should pass (had " << context.invalidDurationCount
                                                               << " invalid out of " << totalDurationChecks << ", indicates delta overflow bugs)";

    // Final receive ratio check - should be high if deltas are correct
    EXPECT_GT(context.totalTxPackets, 0) << "Should have transmitted packets";
    if (context.totalTxPackets > 0) {
        UINT32 overallReceivePercent = (context.totalRxPackets * 100) / context.totalTxPackets;
        EXPECT_GE(overallReceivePercent, 80) << "Overall receive ratio should be >= 80% (was " << overallReceivePercent
                                             << "%), low ratio indicates TWCC delta calculation bugs";
    }

    DLOGD("TWCC receiver test completed: %zu feedbacks, txPackets=%u rxPackets=%u ratio=%u%% validDurations=%zu invalidDurations=%zu",
          context.feedbackCount, context.totalTxPackets, context.totalRxPackets,
          context.totalTxPackets > 0 ? (context.totalRxPackets * 100) / context.totalTxPackets : 0, context.validDurationCount,
          context.invalidDurationCount);
}

// Context for collecting TWCC packet reports (no mutex - peer connection closed before analysis)
struct PacingTestContext {
    std::vector<TwccPacketReport> allReports;
    SIZE_T feedbackCount;
    SIZE_T totalPackets;
    SIZE_T receivedPackets;

    // Get valid reports sorted by send time for analysis
    std::vector<TwccPacketReport> getValidSortedReports() const
    {
        std::vector<TwccPacketReport> validReports;
        for (const auto& report : allReports) {
            if (report.sendTimeKvs > 0 && report.received) {
                validReports.push_back(report);
            }
        }
        std::sort(validReports.begin(), validReports.end(),
                  [](const TwccPacketReport& a, const TwccPacketReport& b) { return a.sendTimeKvs < b.sendTimeKvs; });
        return validReports;
    }

    // Computed stats after validation
    DOUBLE totalDurationSec;
    UINT64 totalBytes;
    DOUBLE actualBitrateBps;
    DOUBLE maxWindowBitrateBps;
    DOUBLE burstRatio;
    DOUBLE receiveRatio;
};

// Helper function for pacing tests
// Sets up peer connections with given pacer config, sends video frames, closes connections,
// populates context with collected TWCC reports for analysis
void PeerConnectionFunctionalityTest::runPacingTest(const RtcPacerConfig& pacerConfig, PacingTestContext& context)
{
    const UINT32 NUM_FRAMES_TO_SEND = 60;
    const UINT64 FRAME_DURATION_KVS = HUNDREDS_OF_NANOS_IN_A_SECOND / 60;
    RtcConfiguration configuration;
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver;
    Frame videoFrame;

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));
    MEMSET(&videoFrame, 0x00, SIZEOF(Frame));
    context.feedbackCount = 0;
    context.totalPackets = 0;
    context.receivedPackets = 0;
    context.allReports.clear();

    // Pre-load all bbbH264 frames with 60 fps PTS. sendPts is populated by the
    // loader (i * FRAME_DURATION_KVS), so the per-frame write loop can pull
    // presentationTs directly from the TestFrame.
    std::vector<TestFrame> bbbFrames = loadFramesFromFolder((PCHAR) "../samples/bbbH264", NUM_FRAMES_TO_SEND,
                                                            RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE,
                                                            HUNDREDS_OF_NANOS_IN_A_SECOND, FRAME_DURATION_KVS);

    // Create peer connections
    EXPECT_EQ(createPeerConnection(&configuration, &offerPc), STATUS_SUCCESS);
    EXPECT_EQ(createPeerConnection(&configuration, &answerPc), STATUS_SUCCESS);

    // Enable TWCC on both peers
    PKvsPeerConnection pOfferKvs = (PKvsPeerConnection) offerPc;
    PKvsPeerConnection pAnswerKvs = (PKvsPeerConnection) answerPc;
    pOfferKvs->twccExtId = 1;
    pAnswerKvs->twccExtId = 1;

    // Add video tracks (H264 codec)
    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver,
                             RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver,
                             RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE, MEDIA_STREAM_TRACK_KIND_VIDEO);

    // Enable pacing with the provided config (including maxQueueTimeKvs if set)
    EXPECT_EQ(peerConnectionEnablePacing(offerPc, const_cast<PRtcPacerConfig>(&pacerConfig)), STATUS_SUCCESS);

    // Callback for TWCC packet reports
    auto onTwccPacketReportHandler = [](UINT64 customData, PTwccPacketReport pReports, UINT32 reportCount, UINT64 rtt) -> void {
        UNUSED_PARAM(rtt);
        PacingTestContext* ctx = (PacingTestContext*) customData;

        for (UINT32 i = 0; i < reportCount; i++) {
            ctx->allReports.push_back(pReports[i]);
            ctx->totalPackets++;
            if (pReports[i].received) {
                ctx->receivedPackets++;
            }
        }
        ctx->feedbackCount++;
    };

    PacingTestContext* pContext = &context;
    EXPECT_EQ(peerConnectionOnTwccPacketReport(offerPc, (UINT64) pContext, onTwccPacketReportHandler), STATUS_SUCCESS);

    // Connect peers
    EXPECT_EQ(connectTwoPeers(offerPc, answerPc), TRUE);

    // Set pacer bitrate after connection
    EXPECT_EQ(peerConnectionSetPacerBitrate(offerPc, pacerConfig.initialBitrateBps), STATUS_SUCCESS);

    // Send video frames
    SIZE_T totalBytesSent = 0;
    SIZE_T iFrameCount = 0;

    for (UINT32 frameNum = 0; frameNum < NUM_FRAMES_TO_SEND; frameNum++) {
        TestFrame& f = bbbFrames[frameNum];
        videoFrame.frameData = f.data.data();
        videoFrame.size = (UINT32) f.data.size();
        videoFrame.presentationTs = f.sendPts;
        videoFrame.decodingTs = f.sendPts;

        BOOL isKeyFrame = (frameNum == 0 || f.data.size() > 50000);
        videoFrame.flags = isKeyFrame ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
        if (isKeyFrame) {
            iFrameCount++;
        }

        EXPECT_EQ(writeFrame(offerVideoTransceiver, &videoFrame), STATUS_SUCCESS);
        totalBytesSent += f.data.size();
        THREAD_SLEEP(FRAME_DURATION_KVS);
    }

    // Wait for remaining TWCC feedback
    THREAD_SLEEP(500 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);

    DLOGD("Sent %u frames, %zu total bytes, %zu I-frames", NUM_FRAMES_TO_SEND, totalBytesSent, iFrameCount);

    // Close peer connections before analysis - guarantees context is stable
    closePeerConnection(offerPc);
    closePeerConnection(answerPc);

    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    // Basic validation
    EXPECT_GT(pContext->feedbackCount, 0) << "Should have received TWCC feedback";
    EXPECT_GT(pContext->allReports.size(), 100) << "Should have reports for many packets";
    EXPECT_GT(iFrameCount, 0) << "Should have sent at least one I-frame";
}

// Common pacing validation: computes stats and runs shared assertions
void PeerConnectionFunctionalityTest::validatePacingResults(PacingTestContext& context, const std::vector<TwccPacketReport>& validReports,
                                                            UINT64 targetBitrateBps, DOUBLE pacingFactor)
{
    ASSERT_GT(validReports.size(), 2) << "Need at least 3 valid reports for analysis";

    DLOGD("Received %zu TWCC feedbacks with %zu valid packet reports", context.feedbackCount, validReports.size());

    // Calculate overall transmission stats
    UINT64 firstSendTime = validReports.front().sendTimeKvs;
    UINT64 lastSendTime = validReports.back().sendTimeKvs;
    UINT64 totalDurationKvs = lastSendTime - firstSendTime;
    context.totalDurationSec = (DOUBLE) totalDurationKvs / HUNDREDS_OF_NANOS_IN_A_SECOND;

    context.totalBytes = 0;
    for (const auto& report : validReports) {
        context.totalBytes += report.packetSize;
    }

    context.actualBitrateBps = (context.totalBytes * 8.0) / context.totalDurationSec;

    DLOGD("Transmission stats: %zu packets, %llu bytes over %.2fs = %.0f bps (target: %llu bps)", validReports.size(),
          (unsigned long long) context.totalBytes, context.totalDurationSec, context.actualBitrateBps, (unsigned long long) targetBitrateBps);

    // Measure throughput in sliding 50ms windows
    const UINT64 WINDOW_SIZE_KVS = 50 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    UINT64 maxWindowBytes = 0;

    for (size_t i = 0; i < validReports.size(); i++) {
        UINT64 windowStart = validReports[i].sendTimeKvs;
        UINT64 windowEnd = windowStart + WINDOW_SIZE_KVS;
        UINT64 windowBytes = 0;

        for (size_t j = i; j < validReports.size() && validReports[j].sendTimeKvs < windowEnd; j++) {
            windowBytes += validReports[j].packetSize;
        }
        if (windowBytes > maxWindowBytes) {
            maxWindowBytes = windowBytes;
        }
    }

    context.maxWindowBitrateBps = (maxWindowBytes * 8.0) / 0.050;
    context.burstRatio = context.maxWindowBitrateBps / targetBitrateBps;

    DLOGD("Window analysis (50ms): maxWindowBytes=%llu maxWindowBitrate=%.0f bps burstRatio=%.2fx", (unsigned long long) maxWindowBytes,
          context.maxWindowBitrateBps, context.burstRatio);

    // Common assertions
    EXPECT_LT(context.actualBitrateBps, targetBitrateBps * pacingFactor) << "Overall bitrate should be < " << pacingFactor << "x target with pacing";

    ASSERT_GT(context.totalPackets, 0);
    context.receiveRatio = (DOUBLE) context.receivedPackets / context.totalPackets;
    DLOGD("Receive ratio: %.1f%% (%zu/%zu)", context.receiveRatio * 100.0, context.receivedPackets, context.totalPackets);
    EXPECT_GT(context.receiveRatio, 0.90) << "Receive ratio should be > 90%";
}

// Test bitrate-based pacing
// Verifies that packets are spread over time according to target bitrate
TEST_F(PeerConnectionFunctionalityTest, pacingBitrate)
{
    const UINT64 TARGET_BITRATE_BPS = 5000000; // 5 Mbps

    RtcPacerConfig pacerConfig;
    pacerConfig.initialBitrateBps = TARGET_BITRATE_BPS;
    pacerConfig.maxQueueSize = 1000;
    pacerConfig.maxQueueBytes = 4 * 1024 * 1024;
    pacerConfig.pacingFactor = 2.5;
    pacerConfig.maxQueueTimeKvs = 0; // No frame deadline, use bitrate-based pacing only

    PacingTestContext context;
    runPacingTest(pacerConfig, context);
    auto validReports = context.getValidSortedReports();

    // Common validation: transmission stats, receive ratio
    validatePacingResults(context, validReports, TARGET_BITRATE_BPS, pacerConfig.pacingFactor);

    // Bitrate-only pacing should have bounded burst ratio
    EXPECT_LT(context.burstRatio, pacerConfig.pacingFactor * 1.5)
        << "Max burst ratio should be bounded with pacing, got " << context.burstRatio << "x";

    DLOGD("Bitrate pacing test completed successfully");
}

// Test frame deadline pacing
// Verifies that large frames are sent within the deadline
TEST_F(PeerConnectionFunctionalityTest, pacingFrameDeadline)
{
    const UINT64 TARGET_BITRATE_BPS = 5000000; // 5 Mbps
    const UINT32 FRAME_DEADLINE_MS = 16;       // 16ms for 60fps
    const UINT64 FRAME_DEADLINE_KVS = FRAME_DEADLINE_MS * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

    RtcPacerConfig pacerConfig;
    pacerConfig.initialBitrateBps = TARGET_BITRATE_BPS;
    pacerConfig.maxQueueSize = 1000;
    pacerConfig.maxQueueBytes = 4 * 1024 * 1024;
    pacerConfig.pacingFactor = 2.5;
    pacerConfig.maxQueueTimeKvs = FRAME_DEADLINE_KVS;

    PacingTestContext context;
    runPacingTest(pacerConfig, context);
    auto validReports = context.getValidSortedReports();

    // Common validation: transmission stats, receive ratio
    // Note: burst ratio is NOT checked here since frame deadline pacing intentionally allows bursts
    validatePacingResults(context, validReports, TARGET_BITRATE_BPS, pacerConfig.pacingFactor);

    // Frame-specific analysis: group packets into frames based on send time gaps
    const UINT64 FRAME_GAP_THRESHOLD_KVS = 5 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    std::vector<std::vector<TwccPacketReport>> frameGroups;
    std::vector<TwccPacketReport> currentGroup;

    for (size_t i = 0; i < validReports.size(); i++) {
        if (currentGroup.empty()) {
            currentGroup.push_back(validReports[i]);
        } else {
            UINT64 gap = validReports[i].sendTimeKvs - currentGroup.back().sendTimeKvs;
            if (gap < FRAME_GAP_THRESHOLD_KVS) {
                currentGroup.push_back(validReports[i]);
            } else {
                frameGroups.push_back(currentGroup);
                currentGroup.clear();
                currentGroup.push_back(validReports[i]);
            }
        }
    }
    if (!currentGroup.empty()) {
        frameGroups.push_back(currentGroup);
    }

    DLOGD("Identified %zu frame groups", frameGroups.size());

    // Analyze large frames (> 10KB) for deadline compliance
    SIZE_T largeFrameCount = 0;
    SIZE_T deadlineMetCount = 0;
    DOUBLE maxFrameDurationMs = 0;
    UINT64 maxFrameBytes = 0;

    for (const auto& group : frameGroups) {
        UINT64 groupBytes = 0;
        for (const auto& pkt : group) {
            groupBytes += pkt.packetSize;
        }

        if (groupBytes < 10000) {
            continue;
        }

        largeFrameCount++;

        UINT64 firstSend = group.front().sendTimeKvs;
        UINT64 lastSend = group.back().sendTimeKvs;
        DOUBLE durationMs = (DOUBLE) (lastSend - firstSend) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

        if (durationMs > maxFrameDurationMs) {
            maxFrameDurationMs = durationMs;
            maxFrameBytes = groupBytes;
        }

        // Allow 2x deadline for timing jitter
        if (durationMs <= FRAME_DEADLINE_MS * 2) {
            deadlineMetCount++;
        }
    }

    DLOGD("Frame deadline analysis: %zu large frames, %zu met deadline (%.1f%%), max=%.1fms for %llu bytes", largeFrameCount, deadlineMetCount,
          largeFrameCount > 0 ? (100.0 * deadlineMetCount / largeFrameCount) : 0.0, maxFrameDurationMs, (unsigned long long) maxFrameBytes);

    // Frame-specific assertions
    ASSERT_GT(largeFrameCount, 0) << "Should have at least one large frame";

    DOUBLE deadlineRatio = (DOUBLE) deadlineMetCount / largeFrameCount;
    EXPECT_GT(deadlineRatio, 0.70) << "At least 70% of large frames should meet deadline, got " << (deadlineRatio * 100.0) << "%";
#ifdef __ANDROID__
    static constexpr auto frameDeadlineMultiplier = 6;
#else
    static constexpr auto frameDeadlineMultiplier = 3;
#endif
    EXPECT_LT(maxFrameDurationMs, FRAME_DEADLINE_MS * frameDeadlineMultiplier)
        << "Max frame duration should be < " << (FRAME_DEADLINE_MS * frameDeadlineMultiplier) << "ms, got " << maxFrameDurationMs << "ms";

    DLOGD("Frame deadline pacing test completed successfully");
}

// Send 100 real girH264 frames (a multi-NAL stream that exercises STAP-A packetization)
// end-to-end through two loopback PeerConnections. Assert that exactly 100 frames are
// delivered to the receiver and that every frame's NAL units match the original input.
TEST_F(PeerConnectionFunctionalityTest, girH264StapARoundTrip)
{
    constexpr UINT32 NUM_FRAMES = 100;

    // Pre-read all input frames. NAL comparison happens on the receive side
    // via the shared expectTestFramesNalUnitsEqual helper at comparison time.
    std::vector<TestFrame> inputFrames = loadFramesFromFolder((PCHAR) "../samples/girH264", NUM_FRAMES);

    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver;

    initRtcConfiguration(&configuration);
    ASSERT_EQ(STATUS_SUCCESS, createPeerConnection(&configuration, &offerPc));
    ASSERT_EQ(STATUS_SUCCESS, createPeerConnection(&configuration, &answerPc));

    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver,
                             RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver,
                             RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE, MEDIA_STREAM_TRACK_KIND_VIDEO);

    struct RxContext {
        const std::vector<TestFrame>* inputFrames;
        SIZE_T receivedCount; // incremented atomically; used as sequential frame index
    };
    RxContext rxCtx;
    MEMSET(&rxCtx, 0, SIZEOF(rxCtx));
    rxCtx.inputFrames = &inputFrames;

    // Non-capturing lambda: state flows through customData.
    // Jitter buffer guarantees in-order delivery; ATOMIC_INCREMENT returns the
    // pre-increment value, so idx is the zero-based frame index.
    EXPECT_EQ(STATUS_SUCCESS, transceiverOnFrame(answerVideoTransceiver, (UINT64) &rxCtx, [](UINT64 customData, PFrame pFrame) -> void {
                  RxContext* ctx = (RxContext*) customData;
                  SIZE_T idx = ATOMIC_INCREMENT((PSIZE_T) &ctx->receivedCount);
                  DLOGI("%zu received frame %lld %d", idx, pFrame->presentationTs, pFrame->size);
                  if (idx >= ctx->inputFrames->size()) {
                      return;
                  }
                  TestFrame received;
                  received.data.assign(pFrame->frameData, pFrame->frameData + pFrame->size);
                  std::string ctxStr = "girH264 frame " + std::to_string(idx);
                  expectTestFramesNalUnitsEqual((*ctx->inputFrames)[(UINT32) idx], received, ctxStr.c_str());
              }));

    EXPECT_EQ(TRUE, connectTwoPeers(offerPc, answerPc));

    // Send NUM_FRAMES+1 frames. The extra frame (a repeat of frame 0) carries a new
    // timestamp that causes the jitter buffer to flush and deliver frame NUM_FRAMES.
    Frame txFrame;
    MEMSET(&txFrame, 0x00, SIZEOF(Frame));
    txFrame.presentationTs = HUNDREDS_OF_NANOS_IN_A_SECOND;
    for (UINT32 i = 0; i < NUM_FRAMES; i++) {
        TestFrame& src = inputFrames[i];
        txFrame.frameData = src.data.data();
        txFrame.size = (UINT32) src.data.size();
        EXPECT_EQ(STATUS_SUCCESS, writeFrame(offerVideoTransceiver, &txFrame));
        txFrame.presentationTs += HUNDREDS_OF_NANOS_IN_A_SECOND / 30;
        DLOGI("%u sent frame %lld %d", i, txFrame.presentationTs, txFrame.size);
        THREAD_SLEEP(5 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    // Wait up to 2 s for all NUM_FRAMES to arrive.
    for (INT32 i = 0; i < 2000 && ATOMIC_LOAD(&rxCtx.receivedCount) < NUM_FRAMES; i++) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }
    size_t totalSize = 0;
    for (const auto& x : inputFrames) {
        totalSize += x.data.size();
    }
    const UINT16 mtu = ((PKvsPeerConnection) offerPc)->MTU;
    auto perfectWorldNumberOfPackets = totalSize / mtu;
    DLOGI("perfectWorldNumberOfPackets %lld", perfectWorldNumberOfPackets);

    size_t idealNumberOfPackets = 0;
    for (const auto& x : inputFrames) {
        idealNumberOfPackets += (x.data.size() + mtu - 1) / mtu;
    }
    DLOGI("idealNumberOfPackets %lld", idealNumberOfPackets);

    RtcOutboundRtpStreamStats stats{};
    EXPECT_EQ(STATUS_SUCCESS, getRtpOutboundStats(offerPc, offerVideoTransceiver, &stats));
    DLOGI("packetsSent %lld", stats.sent.packetsSent);

    RtcInboundRtpStreamStats statsRx{};
    EXPECT_EQ(STATUS_SUCCESS, getRtpInboundStats(answerPc, answerVideoTransceiver, &statsRx));
    DLOGI("packetsReceived %lld", statsRx.received.packetsReceived);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);
    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    EXPECT_EQ((SIZE_T) NUM_FRAMES, ATOMIC_LOAD(&rxCtx.receivedCount)) << "Expected exactly " << NUM_FRAMES << " frames delivered end-to-end";
    // Per-frame NAL count / content mismatches are reported directly by
    // expectTestFramesNalUnitsEqual in the receive callback via EXPECT_EQ.

    EXPECT_EQ(stats.sent.packetsSent, statsRx.received.packetsReceived);
    EXPECT_LE(stats.sent.packetsSent, perfectWorldNumberOfPackets * 2);
}

// Full-cycle test: peer A sends H.264 video and Opus audio; peer B sends
// binary data-channel messages back to peer A. Verifies that all video frames,
// audio frames, and data-channel messages are delivered correctly.
TEST_F(PeerConnectionFunctionalityTest, fullCycleVideoAudioDataChannel)
{
    constexpr UINT32 NUM_VIDEO_FRAMES = 100;
    constexpr UINT32 NUM_AUDIO_FRAMES = 100;
    constexpr UINT32 NUM_DC_MESSAGES = 120;
    constexpr UINT32 DC_MSG_SIZE = 32;

    // --- Pre-read H.264 video frames (25 fps in hundreds-of-nanos matches the
    //     test's existing PTS convention, so default loader args suffice). ---
    std::vector<TestFrame> videoInputFrames = loadFramesFromFolder((PCHAR) "../samples/girH264", NUM_VIDEO_FRAMES);

    // --- Pre-read Opus audio frames (20 ms per frame, 0-based file index) ---
    constexpr UINT64 OPUS_FRAME_DURATION = 20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    std::vector<TestFrame> audioInputFrames = loadFramesFromFolder((PCHAR) "../samples/opusSampleFrames", NUM_AUDIO_FRAMES, RTC_CODEC_OPUS,
                                                                   HUNDREDS_OF_NANOS_IN_A_SECOND, OPUS_FRAME_DURATION, /*firstIndex=*/0);

    // --- Create peer connections with video + audio tracks ---
    RtcConfiguration configuration{};
    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    RtcMediaStreamTrack offerVideoTrack, answerVideoTrack, offerAudioTrack, answerAudioTrack;
    PRtcRtpTransceiver offerVideoTransceiver, answerVideoTransceiver, offerAudioTransceiver, answerAudioTransceiver;

    initRtcConfiguration(&configuration);
    ASSERT_EQ(STATUS_SUCCESS, createPeerConnection(&configuration, &offerPc));
    ASSERT_EQ(STATUS_SUCCESS, createPeerConnection(&configuration, &answerPc));

    addTrackToPeerConnection(offerPc, &offerVideoTrack, &offerVideoTransceiver,
                             RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(offerPc, &offerAudioTrack, &offerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);
    addTrackToPeerConnection(answerPc, &answerVideoTrack, &answerVideoTransceiver,
                             RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE, MEDIA_STREAM_TRACK_KIND_VIDEO);
    addTrackToPeerConnection(answerPc, &answerAudioTrack, &answerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);

    // --- Received frame storage ---
    struct RxContext {
        std::vector<TestFrame> frames;
        std::mutex mtx;
        std::atomic<size_t> count{0};
    };

    // --- Set up frame receivers on answer peer ---
    auto onFrameReceived = [](UINT64 customData, PFrame pFrame) -> void {
        RxContext* ctx = (RxContext*) customData;
        TestFrame rf;
        rf.data.assign(pFrame->frameData, pFrame->frameData + pFrame->size);
        rf.receivePts = pFrame->presentationTs;
        rf.receiveTime = GETTIME();
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->frames.push_back(std::move(rf));
        ctx->count++;
    };

    RxContext videoRx;
    EXPECT_EQ(STATUS_SUCCESS, transceiverOnFrame(answerVideoTransceiver, (UINT64) &videoRx, onFrameReceived));
    RxContext audioRx;
    EXPECT_EQ(STATUS_SUCCESS, transceiverOnFrame(answerAudioTransceiver, (UINT64) &audioRx, onFrameReceived));

    // --- Set up data channel: offer creates, answer receives and sends messages back ---
    PRtcDataChannel pOfferDataChannel = nullptr;
    RtcDataChannelInit dcInit{};
    dcInit.ordered = FALSE;
    dcInit.maxRetransmits.isNull = FALSE;
    dcInit.maxRetransmits.value = 0;
    ASSERT_EQ(STATUS_SUCCESS, createDataChannel(offerPc, (PCHAR) "data", &dcInit, &pOfferDataChannel));

    RxContext dcRx;
    auto dcOnMessageCallback = [](UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMsg, UINT32 pMsgLen) {
        UNUSED_PARAM(pDataChannel);
        if (!isBinary || pMsgLen != DC_MSG_SIZE)
            return;
        RxContext* ctx = (RxContext*) customData;
        TestFrame rf;
        rf.data.assign(pMsg, pMsg + pMsgLen);
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->frames.push_back(std::move(rf));
        ctx->count++;
    };
    EXPECT_EQ(STATUS_SUCCESS, dataChannelOnMessage(pOfferDataChannel, (UINT64) &dcRx, dcOnMessageCallback));

    SIZE_T answerRemoteDc = 0;
    auto onDataChannelCallback = [](UINT64 customData, PRtcDataChannel pRtcDataChannel) {
        ATOMIC_STORE((PSIZE_T) customData, reinterpret_cast<UINT64>(pRtcDataChannel));
    };
    EXPECT_EQ(STATUS_SUCCESS, peerConnectionOnDataChannel(answerPc, (UINT64) &answerRemoteDc, onDataChannelCallback));

    // --- Connect ---
    EXPECT_EQ(TRUE, connectTwoPeers(offerPc, answerPc));

    // Wait for the SCTP data channel to arrive on answer peer
    for (INT32 i = 0; i < 100 && ATOMIC_LOAD(&answerRemoteDc) == 0; i++) {
        THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }
    PRtcDataChannel pAnswerRemoteDc = (PRtcDataChannel) ATOMIC_LOAD(&answerRemoteDc);
    ASSERT_TRUE(pAnswerRemoteDc != nullptr);

    // --- Answer peer sends DC messages back to offer peer ---
    std::thread dcSenderThread([&]() {
        BYTE dcMsgPayload[DC_MSG_SIZE];
        for (UINT32 i = 0; i < NUM_DC_MESSAGES; i++) {
            MEMSET(dcMsgPayload, (BYTE) (i & 0xFF), DC_MSG_SIZE);
            dataChannelSend(pAnswerRemoteDc, TRUE, dcMsgPayload, DC_MSG_SIZE);
            THREAD_SLEEP(8 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND); // ~120 msg/sec
        }
    });

    // --- Offer peer sends video frames ---
    std::thread videoSenderThread([&]() {
        Frame txFrame{};
        for (UINT32 i = 0; i < NUM_VIDEO_FRAMES; i++) {
            txFrame.frameData = videoInputFrames[i].data.data();
            txFrame.size = (UINT32) videoInputFrames[i].data.size();
            txFrame.presentationTs = videoInputFrames[i].sendPts;
            videoInputFrames[i].sendTime = GETTIME();
            EXPECT_EQ(STATUS_SUCCESS, writeFrame(offerVideoTransceiver, &txFrame));
            DLOGI("video frame %u sent, pts=%llu size=%u", i, txFrame.presentationTs, txFrame.size);
            THREAD_SLEEP(5 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }
    });

    // --- Offer peer sends audio frames ---
    std::thread audioSenderThread([&]() {
        Frame txFrame{};
        for (UINT32 i = 0; i < NUM_AUDIO_FRAMES; i++) {
            txFrame.frameData = audioInputFrames[i].data.data();
            txFrame.size = (UINT32) audioInputFrames[i].data.size();
            txFrame.presentationTs = audioInputFrames[i].sendPts;
            audioInputFrames[i].sendTime = GETTIME();
            EXPECT_EQ(STATUS_SUCCESS, writeFrame(offerAudioTransceiver, &txFrame));
            DLOGI("audio frame %u sent, pts=%llu size=%u", i, txFrame.presentationTs, txFrame.size);
            THREAD_SLEEP(5 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }
    });

    dcSenderThread.join();
    videoSenderThread.join();
    audioSenderThread.join();

    // --- Drain: wait for all frames and messages to arrive ---
    for (INT32 i = 0; i < 50 && (dcRx.count < NUM_DC_MESSAGES || videoRx.count < NUM_VIDEO_FRAMES || audioRx.count < NUM_AUDIO_FRAMES); i++) {
        THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    // --- Verify RTP stats on the offer (sender) side ---
    {
        RtcStats rtcMetrics{};

        // Outbound video stats
        rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_OUTBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerVideoTransceiver, &rtcMetrics));
        auto& videoOut = rtcMetrics.rtcStatsObject.outboundRtpStreamStats;

        // Each video frame produces at least ceil(frameSize / MTU) packets
        UINT64 minVideoPackets = 0;
        UINT64 totalNaluBytes = 0;
        for (UINT32 i = 0; i < NUM_VIDEO_FRAMES; i++) {
            minVideoPackets += (videoInputFrames[i].data.size() + DEFAULT_MTU_SIZE_BYTES - 1) / DEFAULT_MTU_SIZE_BYTES;
            UINT32 naluOffsets[128], naluLengths[128];
            UINT32 naluCount =
                extractNaluInfo((PBYTE) videoInputFrames[i].data.data(), (UINT32) videoInputFrames[i].data.size(), naluOffsets, naluLengths, 128);
            for (UINT32 j = 0; j < naluCount; j++) {
                totalNaluBytes += naluLengths[j];
            }
        }
        EXPECT_GE(videoOut.sent.packetsSent, minVideoPackets)
            << "Video packetsSent " << videoOut.sent.packetsSent << " below minimum " << minVideoPackets;
        EXPECT_GE(videoOut.sent.bytesSent, totalNaluBytes)
            << "Video bytesSent " << videoOut.sent.bytesSent << " below total NALU payload " << totalNaluBytes;

        // Outbound audio stats: one Opus frame = one RTP packet
        rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_OUTBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerAudioTransceiver, &rtcMetrics));
        auto& audioOut = rtcMetrics.rtcStatsObject.outboundRtpStreamStats;
        EXPECT_EQ(audioOut.sent.packetsSent, (UINT64) NUM_AUDIO_FRAMES)
            << "Audio packetsSent " << audioOut.sent.packetsSent << " expected " << NUM_AUDIO_FRAMES;
        UINT64 totalAudioBytes = 0;
        for (UINT32 i = 0; i < NUM_AUDIO_FRAMES; i++) {
            totalAudioBytes += audioInputFrames[i].data.size();
        }
        EXPECT_GE(audioOut.sent.bytesSent, totalAudioBytes)
            << "Audio bytesSent " << audioOut.sent.bytesSent << " below total payload " << totalAudioBytes;

        // Data channel stats on offer (receiver of DC messages)
        rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_DATA_CHANNEL;
        rtcMetrics.rtcStatsObject.rtcDataChannelStats.dataChannelIdentifier = pOfferDataChannel->id;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, NULL, &rtcMetrics));
        auto& dcRxStats = rtcMetrics.rtcStatsObject.rtcDataChannelStats;
        EXPECT_EQ(dcRxStats.messagesReceived, NUM_DC_MESSAGES)
            << "DC messagesReceived " << dcRxStats.messagesReceived << " expected " << NUM_DC_MESSAGES;
        EXPECT_EQ(dcRxStats.bytesReceived, (UINT64) NUM_DC_MESSAGES * DC_MSG_SIZE);
    }

    // --- Verify inbound RTP stats on the answer (receiver) side ---
    {
        RtcStats rtcMetrics{};

        // Inbound video stats
        rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_INBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(answerPc, answerVideoTransceiver, &rtcMetrics));
        auto& videoIn = rtcMetrics.rtcStatsObject.inboundRtpStreamStats;
        EXPECT_GE(videoIn.received.packetsReceived, (UINT64) NUM_VIDEO_FRAMES)
            << "Inbound video packetsReceived " << videoIn.received.packetsReceived << " below frame count";
        EXPECT_EQ(videoIn.received.packetsLost, 0) << "Inbound video packetsLost " << videoIn.received.packetsLost;

        // Inbound audio stats
        rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_INBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(answerPc, answerAudioTransceiver, &rtcMetrics));
        auto& audioIn = rtcMetrics.rtcStatsObject.inboundRtpStreamStats;
        EXPECT_EQ(audioIn.received.packetsReceived, (UINT64) NUM_AUDIO_FRAMES)
            << "Inbound audio packetsReceived " << audioIn.received.packetsReceived << " expected " << NUM_AUDIO_FRAMES;
        EXPECT_EQ(audioIn.received.packetsLost, 0) << "Inbound audio packetsLost " << audioIn.received.packetsLost;

        // Data channel stats on answer (sender of DC messages)
        rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_DATA_CHANNEL;
        rtcMetrics.rtcStatsObject.rtcDataChannelStats.dataChannelIdentifier = pAnswerRemoteDc->id;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(answerPc, NULL, &rtcMetrics));
        auto& dcTxStats = rtcMetrics.rtcStatsObject.rtcDataChannelStats;
        EXPECT_EQ(dcTxStats.messagesSent, NUM_DC_MESSAGES) << "DC messagesSent " << dcTxStats.messagesSent << " expected " << NUM_DC_MESSAGES;
        EXPECT_EQ(dcTxStats.bytesSent, (UINT64) NUM_DC_MESSAGES * DC_MSG_SIZE);
    }

    // --- Verify remote inbound RTP stats on the offer (from RTCP RR) ---
    {
        RtcStats rtcMetrics{};

        // Wait for at least one RTCP RR to arrive.
        for (INT32 i = 0; i < 50; i++) {
            rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_INBOUND_RTP;
            ASSERT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerVideoTransceiver, &rtcMetrics));
            if (rtcMetrics.rtcStatsObject.remoteInboundRtpStreamStats.reportsReceived > 0) {
                break;
            }
            THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }

        // Wait for the offer's Sender Report to fire.
        // SR needs RTCP_FIRST_REPORT_DELAY (3s) + 2.5s firstFrameWallClockTime guard.
        // Poll until the offer receives an RR with non-zero LSR (meaning the answer
        // received our SR), visible as roundTripTime > 0.
        for (INT32 i = 0; i < 80; i++) {
            rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_INBOUND_RTP;
            ASSERT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerVideoTransceiver, &rtcMetrics));
            if (rtcMetrics.rtcStatsObject.remoteInboundRtpStreamStats.roundTripTime > 0) {
                break;
            }
            THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }

        // Video remote inbound — compare against what we actually sent
        RtcStats outMetrics{};
        outMetrics.requestedTypeOfStats = RTC_STATS_TYPE_OUTBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerVideoTransceiver, &outMetrics));
        UINT64 videoPacketsSent = outMetrics.rtcStatsObject.outboundRtpStreamStats.sent.packetsSent;

        auto& videoRemote = rtcMetrics.rtcStatsObject.remoteInboundRtpStreamStats;
        EXPECT_GT(videoRemote.reportsReceived, (UINT64) 0) << "No RTCP RR received for video";
        EXPECT_GE(videoRemote.received.packetsReceived, videoPacketsSent)
            << "Remote inbound video packetsReceived " << videoRemote.received.packetsReceived << " expected ~" << videoPacketsSent;
        EXPECT_EQ(videoRemote.received.packetsLost, 0) << "Remote inbound video packetsLost " << videoRemote.received.packetsLost;
        EXPECT_GE(videoRemote.received.jitter, 0.0) << "Remote inbound video jitter is negative";
        // Localhost jitter should be sub-millisecond; a first-packet EWMA seed bug
        // produces ~47 000 s from a UINT32-saturated wire value.
        EXPECT_LT(videoRemote.received.jitter, 1.0) << "Remote inbound video jitter " << videoRemote.received.jitter << "s";
        EXPECT_EQ(videoRemote.fractionLost, 0.0) << "Remote inbound video fractionLost " << videoRemote.fractionLost;

        // Audio remote inbound
        outMetrics.requestedTypeOfStats = RTC_STATS_TYPE_OUTBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerAudioTransceiver, &outMetrics));
        UINT64 audioPacketsSent = outMetrics.rtcStatsObject.outboundRtpStreamStats.sent.packetsSent;

        rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_INBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerAudioTransceiver, &rtcMetrics));
        auto& audioRemote = rtcMetrics.rtcStatsObject.remoteInboundRtpStreamStats;
        EXPECT_GT(audioRemote.reportsReceived, (UINT64) 0) << "No RTCP RR received for audio";
        EXPECT_GE(audioRemote.received.packetsReceived, audioPacketsSent)
            << "Remote inbound audio packetsReceived " << audioRemote.received.packetsReceived << " expected ~" << audioPacketsSent;
        EXPECT_EQ(audioRemote.received.packetsLost, 0) << "Remote inbound audio packetsLost " << audioRemote.received.packetsLost;
        EXPECT_GE(audioRemote.received.jitter, 0.0) << "Remote inbound audio jitter is negative";
        EXPECT_LT(audioRemote.received.jitter, 1.0) << "Remote inbound audio jitter " << audioRemote.received.jitter << "s";
        EXPECT_EQ(audioRemote.fractionLost, 0.0) << "Remote inbound audio fractionLost " << audioRemote.fractionLost;
    }

    // --- Verify remote outbound RTP stats on the answer (from RTCP SR sent by offer) ---
    {
        RtcStats rtcMetrics{};

        // Get the offer's outbound stats for comparison — the SR carries these exact values
        RtcStats offerVideoOutMetrics{}, offerAudioOutMetrics{};
        offerVideoOutMetrics.requestedTypeOfStats = RTC_STATS_TYPE_OUTBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerVideoTransceiver, &offerVideoOutMetrics));
        offerAudioOutMetrics.requestedTypeOfStats = RTC_STATS_TYPE_OUTBOUND_RTP;
        EXPECT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(offerPc, offerAudioTransceiver, &offerAudioOutMetrics));

        // Wait for at least one RTCP SR with final packet counts to arrive on the answer side.
        // The SR copies outboundStats.sent.packetsSent; all frames were sent before the first SR fires
        // (SR starts 2.5s after first frame, frames finish in ~500ms), so the SR should carry final counts.
        for (INT32 i = 0; i < 50; i++) {
            rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_OUTBOUND_RTP;
            ASSERT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(answerPc, answerVideoTransceiver, &rtcMetrics));
            if (rtcMetrics.rtcStatsObject.remoteOutboundRtpStreamStats.sent.packetsSent ==
                offerVideoOutMetrics.rtcStatsObject.outboundRtpStreamStats.sent.packetsSent) {
                break;
            }
            THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }

        UINT64 nowMs = GETTIME() / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

        // Video remote outbound — packetsSent/bytesSent must match the offer's outbound stats exactly
        auto& videoRemoteOut = rtcMetrics.rtcStatsObject.remoteOutboundRtpStreamStats;
        auto& videoOfferOut = offerVideoOutMetrics.rtcStatsObject.outboundRtpStreamStats;
        EXPECT_GT(videoRemoteOut.reportsSent, (UINT64) 0) << "No RTCP SR received for video";
        EXPECT_EQ(videoRemoteOut.sent.packetsSent, videoOfferOut.sent.packetsSent)
            << "Remote outbound video packetsSent " << videoRemoteOut.sent.packetsSent << " != offer outbound " << videoOfferOut.sent.packetsSent;
        EXPECT_EQ(videoRemoteOut.sent.bytesSent, videoOfferOut.sent.bytesSent)
            << "Remote outbound video bytesSent " << videoRemoteOut.sent.bytesSent << " != offer outbound " << videoOfferOut.sent.bytesSent;
        EXPECT_EQ(videoRemoteOut.sent.rtpStream.ssrc, ((PKvsRtpTransceiver) offerVideoTransceiver)->sender.ssrc)
            << "Remote outbound video SSRC mismatch";
        // remoteTimestamp should be a Unix ms timestamp within 60s of now
        EXPECT_GT(videoRemoteOut.remoteTimestamp, nowMs - 60000) << "Remote outbound video remoteTimestamp too old";
        EXPECT_LE(videoRemoteOut.remoteTimestamp, nowMs + 1000) << "Remote outbound video remoteTimestamp in the future";

        // Audio remote outbound — wait for SR to arrive (separate timer from video)
        for (INT32 i = 0; i < 50; i++) {
            rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_OUTBOUND_RTP;
            ASSERT_EQ(STATUS_SUCCESS, rtcPeerConnectionGetMetrics(answerPc, answerAudioTransceiver, &rtcMetrics));
            if (rtcMetrics.rtcStatsObject.remoteOutboundRtpStreamStats.sent.packetsSent ==
                offerAudioOutMetrics.rtcStatsObject.outboundRtpStreamStats.sent.packetsSent) {
                break;
            }
            THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }
        auto& audioRemoteOut = rtcMetrics.rtcStatsObject.remoteOutboundRtpStreamStats;
        auto& audioOfferOut = offerAudioOutMetrics.rtcStatsObject.outboundRtpStreamStats;
        EXPECT_GT(audioRemoteOut.reportsSent, (UINT64) 0) << "No RTCP SR received for audio";
        EXPECT_EQ(audioRemoteOut.sent.packetsSent, audioOfferOut.sent.packetsSent)
            << "Remote outbound audio packetsSent " << audioRemoteOut.sent.packetsSent << " != offer outbound " << audioOfferOut.sent.packetsSent;
        EXPECT_EQ(audioRemoteOut.sent.bytesSent, audioOfferOut.sent.bytesSent)
            << "Remote outbound audio bytesSent " << audioRemoteOut.sent.bytesSent << " != offer outbound " << audioOfferOut.sent.bytesSent;
        EXPECT_EQ(audioRemoteOut.sent.rtpStream.ssrc, ((PKvsRtpTransceiver) offerAudioTransceiver)->sender.ssrc)
            << "Remote outbound audio SSRC mismatch";
        EXPECT_GT(audioRemoteOut.remoteTimestamp, nowMs - 60000) << "Remote outbound audio remoteTimestamp too old";
        EXPECT_LE(audioRemoteOut.remoteTimestamp, nowMs + 1000) << "Remote outbound audio remoteTimestamp in the future";
    }

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);
    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);

    // --- Verify data channel messages ---
    EXPECT_EQ((SIZE_T) NUM_DC_MESSAGES, dcRx.frames.size()) << "Expected " << NUM_DC_MESSAGES << " DC messages but got " << dcRx.frames.size();
    for (UINT32 i = 0; i < dcRx.frames.size() && i < NUM_DC_MESSAGES; i++) {
        const auto& msg = dcRx.frames[i];
        EXPECT_EQ((UINT32) DC_MSG_SIZE, (UINT32) msg.data.size()) << "DC message " << i << " size mismatch";
        BYTE expectedFill = (BYTE) (i & 0xFF);
        BYTE expectedPayload[DC_MSG_SIZE];
        MEMSET(expectedPayload, expectedFill, DC_MSG_SIZE);
        if (msg.data.size() == DC_MSG_SIZE) {
            EXPECT_EQ(0, MEMCMP(msg.data.data(), expectedPayload, DC_MSG_SIZE))
                << "DC message " << i << " content mismatch (expected fill 0x" << std::hex << (int) expectedFill << std::dec << ")";
        }
    }

    // --- Verify video frame count ---
    EXPECT_EQ((SIZE_T) NUM_VIDEO_FRAMES, videoRx.frames.size())
        << "Expected " << NUM_VIDEO_FRAMES << " video frames but got " << videoRx.frames.size();

    // --- Verify video frame content (NALU-level comparison) ---
    for (UINT32 i = 0; i < videoRx.frames.size() && i < NUM_VIDEO_FRAMES; i++) {
        const auto& rx = videoRx.frames[i];
        const auto& expected = videoInputFrames[i];
        EXPECT_EQ(expected.sendPts, rx.receivePts) << "Video frame " << i << " pts mismatch";
        UINT32 expectedOffsets[128], expectedLengths[128], rxOffsets[128], rxLengths[128];
        UINT32 expectedNaluCount =
            extractNaluInfo((PBYTE) expected.data.data(), (UINT32) expected.data.size(), expectedOffsets, expectedLengths, 128);
        UINT32 rxNaluCount = extractNaluInfo((PBYTE) rx.data.data(), (UINT32) rx.data.size(), rxOffsets, rxLengths, 128);
        EXPECT_EQ(expectedNaluCount, rxNaluCount) << "Video frame " << i << " NALU count mismatch";
        if (rxNaluCount == expectedNaluCount) {
            for (UINT32 j = 0; j < rxNaluCount; j++) {
                EXPECT_EQ(expectedLengths[j], rxLengths[j]) << "Video frame " << i << " NALU " << j << " length mismatch";
                if (rxLengths[j] == expectedLengths[j]) {
                    EXPECT_EQ(0, MEMCMP(rx.data.data() + rxOffsets[j], expected.data.data() + expectedOffsets[j], rxLengths[j]))
                        << "Video frame " << i << " NALU " << j << " content mismatch";
                }
            }
        }
    }

    // --- Verify audio frame count and content ---
    EXPECT_EQ((SIZE_T) NUM_AUDIO_FRAMES, audioRx.frames.size())
        << "Expected " << NUM_AUDIO_FRAMES << " audio frames but got " << audioRx.frames.size();
    for (UINT32 i = 0; i < audioRx.frames.size() && i < NUM_AUDIO_FRAMES; i++) {
        const auto& rx = audioRx.frames[i];
        EXPECT_EQ(audioInputFrames[i].sendPts, rx.receivePts) << "Audio frame " << i << " pts mismatch";
        EXPECT_EQ((UINT32) audioInputFrames[i].data.size(), (UINT32) rx.data.size()) << "Audio frame " << i << " size mismatch";
        if (rx.data.size() == audioInputFrames[i].data.size()) {
            EXPECT_EQ(0, MEMCMP(rx.data.data(), audioInputFrames[i].data.data(), rx.data.size())) << "Audio frame " << i << " content mismatch";
        }
    }

    // --- Verify frame delivery latency ---
    // Video frame 0 has higher latency because the jitter buffer delays the first frame until the next
    // frame's first packet arrives (firstFrameProcessed guard for codecs that fragment across packets).
    // Audio (Opus) uses alwaysSinglePacketFrames so all frames including the first use marker-bit delivery.
    constexpr UINT64 FIRST_VIDEO_FRAME_LATENCY_LIMIT = 50 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    constexpr UINT64 STEADY_STATE_LATENCY_LIMIT = 10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

    // Video latency: assert first frame, collect steady-state for median
    std::vector<UINT64> videoSteadyLatencies;
    for (UINT32 i = 0; i < videoRx.frames.size() && i < NUM_VIDEO_FRAMES; i++) {
        UINT64 latency = videoRx.frames[i].receiveTime - videoInputFrames[i].sendTime;
        DOUBLE latencyMs = (DOUBLE) latency / (DOUBLE) HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        DLOGI("video frame %u latency: %.2f ms", i, latencyMs);
        if (i == 0) {
            EXPECT_LT(latency, FIRST_VIDEO_FRAME_LATENCY_LIMIT) << "Video frame 0 latency " << latencyMs << " ms exceeded limit";
        } else {
            videoSteadyLatencies.push_back(latency);
        }
    }
    if (!videoSteadyLatencies.empty()) {
        std::sort(videoSteadyLatencies.begin(), videoSteadyLatencies.end());
        UINT64 medianVideoLatency = videoSteadyLatencies[videoSteadyLatencies.size() / 2];
        DOUBLE medianVideoMs = (DOUBLE) medianVideoLatency / (DOUBLE) HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        DLOGI("video median steady-state latency: %.2f ms", medianVideoMs);
        EXPECT_LT(medianVideoLatency, STEADY_STATE_LATENCY_LIMIT) << "Video median latency " << medianVideoMs << " ms exceeded limit";
    }

    // Audio latency: all frames use median (Opus never fragments)
    std::vector<UINT64> audioLatencies;
    for (UINT32 i = 0; i < audioRx.frames.size() && i < NUM_AUDIO_FRAMES; i++) {
        UINT64 latency = audioRx.frames[i].receiveTime - audioInputFrames[i].sendTime;
        DOUBLE latencyMs = (DOUBLE) latency / (DOUBLE) HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        DLOGI("audio frame %u latency: %.2f ms", i, latencyMs);
        audioLatencies.push_back(latency);
    }
    if (!audioLatencies.empty()) {
        std::sort(audioLatencies.begin(), audioLatencies.end());
        UINT64 medianAudioLatency = audioLatencies[audioLatencies.size() / 2];
        DOUBLE medianAudioMs = (DOUBLE) medianAudioLatency / (DOUBLE) HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        DLOGI("audio median latency: %.2f ms", medianAudioMs);
        EXPECT_LT(medianAudioLatency, STEADY_STATE_LATENCY_LIMIT) << "Audio median latency " << medianAudioMs << " ms exceeded limit";
    }
}

// Reproduces a use-after-free in sendPacketToRtpReceiver's RED split branch.
//
// When useRedForOpus is true on both peers, the receiver's sendPacketToRtpReceiver
// splits each inbound RED packet into N+1 synthetic per-Opus-frame packets and pushes
// each through the jitter buffer. The JB dedups an incoming synthetic against an
// already-present real packet at the same seq by calling freeRtpPacket(pSyn) from
// inside jitterBufferPush. The production code then reads pSyn->isSynthetic and
// pSyn->payloadLength after the push — that's a read of freed memory.
//
// Under default libc / macOS allocators the freed byte often still reads as its
// previous value, so the bug is silent in opt-mode testing. Under ASan (or any
// scrubbing / guard allocator) it surfaces immediately as heap-use-after-free.
//
// The test drives the full receive path: two connected PeerConnections, both with
// useRedForOpus=TRUE, one sending Opus frames, the other receiving. Every wire
// packet after the first triggers the dedup branch, so the UAF is exercised many
// times per run. Assertion failure is incidental — the real signal is an ASan
// heap-use-after-free when the build has -fsanitize=address.
TEST_F(PeerConnectionFunctionalityTest, opusRedEndToEndDedup)
{
    constexpr UINT32 NUM_FRAMES = 50;
    constexpr UINT32 OPUS_FRAME_SIZE = 80; // arbitrary; codec is opaque below RED
    constexpr UINT64 OPUS_FRAME_DURATION = 20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

    RtcConfiguration cfg{};
    initRtcConfiguration(&cfg);
    cfg.kvsRtcConfiguration.useRedForOpus = TRUE;
    // The dedup path that frees the synthetic only fires reliably in the RealTime JB —
    // the classic JB removes packets from its hash table on delivery, so subsequent
    // synthetics get stored as new packets rather than dedup'd, and the UAF branch
    // (if (pSyn->isSynthetic) inside `synDiscarded`) is never executed.
    cfg.kvsRtcConfiguration.useRealTimeJitterBuffer = TRUE;

    PRtcPeerConnection offerPc = NULL, answerPc = NULL;
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&cfg, &offerPc));
    EXPECT_EQ(STATUS_SUCCESS, createPeerConnection(&cfg, &answerPc));

    RtcMediaStreamTrack offerAudioTrack{}, answerAudioTrack{};
    PRtcRtpTransceiver offerAudioTransceiver = NULL, answerAudioTransceiver = NULL;
    addTrackToPeerConnection(offerPc, &offerAudioTrack, &offerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);
    addTrackToPeerConnection(answerPc, &answerAudioTrack, &answerAudioTransceiver, RTC_CODEC_OPUS, MEDIA_STREAM_TRACK_KIND_AUDIO);

    std::atomic<UINT32> framesReceived{0};
    auto onAudioFrame = [](UINT64 customData, PFrame pFrame) -> void {
        UNUSED_PARAM(pFrame);
        ((std::atomic<UINT32>*) customData)->fetch_add(1);
    };
    EXPECT_EQ(STATUS_SUCCESS, transceiverOnFrame(answerAudioTransceiver, (UINT64) &framesReceived, onAudioFrame));

    ASSERT_EQ(TRUE, connectTwoPeers(offerPc, answerPc));

    // Sanity: the SDP negotiation must have wired up RED on both sides. If it did not,
    // the RED receive branch never runs and the UAF never fires — the test would then
    // be a false "pass" under ASan, so assert the gate here.
    PKvsPeerConnection pKvsOffer = (PKvsPeerConnection) offerPc;
    PKvsPeerConnection pKvsAnswer = (PKvsPeerConnection) answerPc;
    EXPECT_TRUE(pKvsOffer->useRedForOpus);
    EXPECT_TRUE(pKvsAnswer->useRedForOpus);

    // After negotiation, the answer-side transceiver must have receiverRedPayloadType set
    // (that's what gates the sendPacketToRtpReceiver RED-split branch). If this is zero,
    // the UAF line we are trying to exercise is unreachable.
    PKvsRtpTransceiver pKvsAnswerAudio = (PKvsRtpTransceiver) answerAudioTransceiver;
    EXPECT_NE(0, pKvsAnswerAudio->receiverRedPayloadType) << "RED not negotiated on answer side — RED receive branch will not run";

    PBYTE opusBytes = (PBYTE) MEMALLOC(OPUS_FRAME_SIZE);
    MEMSET(opusBytes, 0xAA, OPUS_FRAME_SIZE);
    Frame frame{};
    frame.frameData = opusBytes;
    frame.size = OPUS_FRAME_SIZE;
    frame.presentationTs = 0;

    for (UINT32 i = 0; i < NUM_FRAMES; i++) {
        STATUS s = writeFrame(offerAudioTransceiver, &frame);
        EXPECT_TRUE(s == STATUS_SUCCESS || s == STATUS_SRTP_NOT_READY_YET);
        frame.presentationTs += OPUS_FRAME_DURATION;
        THREAD_SLEEP(OPUS_FRAME_DURATION);
    }

    // Allow in-flight packets to drain through the receiver.
    for (UINT32 i = 0; i < 50 && framesReceived.load() < 10; i++) {
        THREAD_SLEEP(20 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }

    // At least a handful of frames should have been delivered — confirms the RED
    // receive branch actually ran and therefore the UAF line was executed many times.
    EXPECT_GT(framesReceived.load(), 0u) << "answerer did not receive any Opus frames; RED receive branch never exercised";

    // Snapshot counters under the transceiver's statsLock — the ICE/RTP thread writes
    // inboundStats.fec* fields under this same lock from transceiverOnRtpPacketReceived,
    // so reading bare would be a TSan-visible data race.
    UINT64 fecReceivedSnapshot;
    UINT64 fecDiscardedSnapshot;
    MUTEX_LOCK(pKvsAnswerAudio->statsLock);
    fecReceivedSnapshot = pKvsAnswerAudio->inboundStats.fecPacketsReceived;
    fecDiscardedSnapshot = pKvsAnswerAudio->inboundStats.fecPacketsDiscarded;
    MUTEX_UNLOCK(pKvsAnswerAudio->statsLock);

    // Confirm the RED receive path actually counted redundant blocks — if this is zero
    // then sendPacketToRtpReceiver's RED split branch never executed the buggy read.
    DLOGI("opusRedEndToEndDedup: framesReceived=%u fecPacketsReceived=%llu fecPacketsDiscarded=%llu", framesReceived.load(),
          (unsigned long long) fecReceivedSnapshot, (unsigned long long) fecDiscardedSnapshot);
    EXPECT_GT(fecReceivedSnapshot, 0u) << "no redundant blocks counted — the RED split branch in sendPacketToRtpReceiver did not run, "
                                       << "so the UAF line was not exercised and this test cannot reproduce the bug";

    MEMFREE(opusBytes);

    closePeerConnection(offerPc);
    closePeerConnection(answerPc);
    freePeerConnection(&offerPc);
    freePeerConnection(&answerPc);
}
} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
