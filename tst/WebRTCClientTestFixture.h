#include "gtest/gtest.h"
#include "../src/source/Include_i.h"
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>

#define TEST_DEFAULT_REGION             ((PCHAR) "us-west-2")
#define TEST_DEFAULT_STUN_URL_POSTFIX   (KINESIS_VIDEO_STUN_URL_POSTFIX)
#define TEST_STREAMING_TOKEN_DURATION   (40 * HUNDREDS_OF_NANOS_IN_A_SECOND)
#define TEST_JITTER_BUFFER_CLOCK_RATE   (1000)
#define TEST_SIGNALING_MASTER_CLIENT_ID (PCHAR) "Test_Master_ClientId"
#define TEST_SIGNALING_VIEWER_CLIENT_ID (PCHAR) "Test_Viewer_ClientId"
#define TEST_SIGNALING_CHANNEL_NAME     (PCHAR) "ScaryTestChannel_"
#define TEST_KMS_KEY_ID_ARN             (PCHAR) "arn:aws:kms:us-west-2:123456789012:key/0000-0000-0000-0000-0000"
#define TEST_CHANNEL_ARN                (PCHAR) "arn:aws:kinesisvideo:us-west-2:123456789012:channel/ScaryTestChannel"
#define TEST_STREAM_ARN                 (PCHAR) "arn:aws:kinesisvideo:us-west-2:123456789012:stream/ScaryTestStream"
#define SIGNAING_TEST_CORRELATION_ID    (PCHAR) "Test_correlation_id"
#define TEST_SIGNALING_MESSAGE_TTL      (120 * HUNDREDS_OF_NANOS_IN_A_SECOND)
#define TEST_VIDEO_FRAME_SIZE           (120 * 1024)
#define TEST_FILE_CREDENTIALS_FILE_PATH (PCHAR) "credsFile"
#define MAX_TEST_AWAIT_DURATION         (2 * HUNDREDS_OF_NANOS_IN_A_SECOND)
#define TEST_CACHE_FILE_PATH            (PCHAR) "./.TestSignalingCache_v1"

namespace com {
namespace amazonaws {
namespace kinesis {
namespace video {
namespace webrtcclient {

#ifdef ENABLE_SIGNALING
// This comes from Producer-C, but is not exported. We are copying it here instead of making it part of the public API.
// It *MAY* become de-synchronized. If you hit issues after updating Producer-C confirm these two structs are in sync
typedef struct {
    AwsCredentialProvider credentialProvider;
    PAwsCredentials pAwsCredentials;
} StaticCredentialProvider, *PStaticCredentialProvider;
#endif

STATUS createRtpPacketWithSeqNum(UINT16 seqNum, PRtpPacket* ppRtpPacket);

// Shared TestFrame.flags values. FULL means onFrameReady delivered every byte
// of the frame; PARTIAL means onFrameDropped fired but some packets were
// still salvageable through fillPartialFrameData; DROPPED means nothing was
// recoverable. Additional states can be added here as tests need them.
// Enum (not static constexpr) to avoid pre-C++17 ODR-use link errors when
// these values are passed to gtest EXPECT_EQ by reference.
enum : uint32_t {
    TEST_FRAME_FULL = 1,
    TEST_FRAME_PARTIAL = 2,
    TEST_FRAME_DROPPED = 3,
};

// Shared test frame type used by H264 integration / peer connection tests.
// The loader (WebRtcClientTestBase::loadH264FramesFromFolder) populates
// `data`, `sendPts` (= i * frameDuration) and `timescale`. Remaining fields
// are runtime state that specific tests fill in at send / receive time:
//   - sendTime / receiveTime: GETTIME() wall clock
//   - receivePts: presentation ts as seen at the receiver
//   - flags: test-defined status bits (see TEST_FRAME_* above)
// sendPts / receivePts are in units of 1/timescale seconds. Tests that care
// about a specific clock rate (e.g. the H264 jitter test uses RTP 90 kHz)
// pass an explicit timescale / frameDuration to the loader.
struct TestFrame {
    std::vector<BYTE> data;
    UINT64 sendPts = 0;
    UINT64 receivePts = 0;
    UINT32 timescale = 0;
    UINT64 sendTime = 0;
    UINT64 receiveTime = 0;
    UINT32 flags = 0;
};

class WebRtcClientTestBase : public ::testing::Test {
  public:
    SIGNALING_CLIENT_HANDLE mSignalingClientHandle;
    std::vector<std::thread> threads;
    std::mutex lock;
    BOOL noNewThreads = FALSE;

    WebRtcClientTestBase();

    PCHAR getAccessKey()
    {
        return mAccessKey;
    }

    PCHAR getSecretKey()
    {
        return mSecretKey;
    }

    PCHAR getSessionToken()
    {
        return mSessionToken;
    }

#ifdef ENABLE_SIGNALING
    VOID initializeSignalingClientStructs()
    {
        mTags[0].version = TAG_CURRENT_VERSION;
        mTags[0].name = (PCHAR) "Tag Name 0";
        mTags[0].value = (PCHAR) "Tag Value 0";
        mTags[1].version = TAG_CURRENT_VERSION;
        mTags[1].name = (PCHAR) "Tag Name 1";
        mTags[1].value = (PCHAR) "Tag Value 1";
        mTags[2].version = TAG_CURRENT_VERSION;
        mTags[2].name = (PCHAR) "Tag Name 2";
        mTags[2].value = (PCHAR) "Tag Value 2";

        mSignalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
        mSignalingClientCallbacks.customData = (UINT64) this;
        mSignalingClientCallbacks.messageReceivedFn = NULL;
        mSignalingClientCallbacks.errorReportFn = NULL;
        mSignalingClientCallbacks.stateChangeFn = NULL;
        mSignalingClientCallbacks.getCurrentTimeFn = NULL;

        mClientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
        mClientInfo.loggingLevel = LOG_LEVEL_VERBOSE;
        mClientInfo.cacheFilePath = NULL; // Use the default path
        STRCPY(mClientInfo.clientId, TEST_SIGNALING_MASTER_CLIENT_ID);

        mClientInfo.signalingRetryStrategyCallbacks.createRetryStrategyFn = createRetryStrategyFn;
        mClientInfo.signalingRetryStrategyCallbacks.getCurrentRetryAttemptNumberFn = getCurrentRetryAttemptNumberFn;
        mClientInfo.signalingRetryStrategyCallbacks.freeRetryStrategyFn = freeRetryStrategyFn;
        mClientInfo.signalingRetryStrategyCallbacks.executeRetryStrategyFn = executeRetryStrategyFn;
        mClientInfo.signalingClientCreationMaxRetryAttempts = 0;

        MEMSET(&mChannelInfo, 0x00, SIZEOF(mChannelInfo));
        mChannelInfo.version = CHANNEL_INFO_CURRENT_VERSION;
        mChannelInfo.pChannelName = mChannelName;
        mChannelInfo.pKmsKeyId = NULL;
        mChannelInfo.tagCount = 3;
        mChannelInfo.pTags = mTags;
        mChannelInfo.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
        mChannelInfo.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
        mChannelInfo.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_NONE;
        mChannelInfo.cachingPeriod = 0;
        mChannelInfo.retry = TRUE;
        mChannelInfo.reconnect = TRUE;
        mChannelInfo.pCertPath = mCaCertPath;
        mChannelInfo.messageTtl = TEST_SIGNALING_MESSAGE_TTL;

        if ((mChannelInfo.pRegion = getenv(DEFAULT_REGION_ENV_VAR)) == NULL) {
            mChannelInfo.pRegion = (PCHAR) TEST_DEFAULT_REGION;
        }
    }

    STATUS initializeSignalingClient(PAwsCredentialProvider pCredentialProvider = NULL)
    {
        STATUS retStatus;

        initializeSignalingClientStructs();

        retStatus = createSignalingClientSync(&mClientInfo, &mChannelInfo, &mSignalingClientCallbacks,
                                              pCredentialProvider != NULL ? pCredentialProvider : mTestCredentialProvider, &mSignalingClientHandle);

        if (mAccessKeyIdSet) {
            EXPECT_EQ(STATUS_SUCCESS, retStatus);
        } else {
            mSignalingClientHandle = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
            EXPECT_NE(STATUS_SUCCESS, retStatus);
        }

        retStatus = signalingClientFetchSync(mSignalingClientHandle);

        return retStatus;
    }

    STATUS deinitializeSignalingClient()
    {
        // Delete the created channel
        if (mAccessKeyIdSet) {
            deleteChannelLws(FROM_SIGNALING_CLIENT_HANDLE(mSignalingClientHandle), 0);
        }

        EXPECT_EQ(STATUS_SUCCESS, freeSignalingClient(&mSignalingClientHandle));

        return STATUS_SUCCESS;
    }
#else
    STATUS initializeSignalingClient()
    {
        return STATUS_SUCCESS;
    }
    STATUS deinitializeSignalingClient()
    {
        return STATUS_SUCCESS;
    }

#endif

    static STATUS createRetryStrategyFn(PKvsRetryStrategy pKvsRetryStrategy)
    {
        STATUS retStatus = STATUS_SUCCESS;
        PExponentialBackoffRetryStrategyState pExponentialBackoffRetryStrategyState = NULL;

        CHK_STATUS(exponentialBackoffRetryStrategyCreate(pKvsRetryStrategy));
        CHK(pKvsRetryStrategy->retryStrategyType == KVS_RETRY_STRATEGY_EXPONENTIAL_BACKOFF_WAIT, STATUS_INTERNAL_ERROR);

        pExponentialBackoffRetryStrategyState = TO_EXPONENTIAL_BACKOFF_STATE(pKvsRetryStrategy->pRetryStrategy);

        // Overwrite retry config to avoid slow long running tests
        pExponentialBackoffRetryStrategyState->exponentialBackoffRetryStrategyConfig.retryFactorTime = HUNDREDS_OF_NANOS_IN_A_MILLISECOND * 5;
        pExponentialBackoffRetryStrategyState->exponentialBackoffRetryStrategyConfig.maxRetryWaitTime = HUNDREDS_OF_NANOS_IN_A_MILLISECOND * 75;

    CleanUp:
        return retStatus;
    }

    static STATUS getCurrentRetryAttemptNumberFn(PKvsRetryStrategy pKvsRetryStrategy, PUINT32 pRetryCount)
    {
        return getExponentialBackoffRetryCount(pKvsRetryStrategy, pRetryCount);
    }

    static STATUS freeRetryStrategyFn(PKvsRetryStrategy pKvsRetryStrategy)
    {
        return exponentialBackoffRetryStrategyFree(pKvsRetryStrategy);
    }

    static STATUS executeRetryStrategyFn(PKvsRetryStrategy pKvsRetryStrategy, PUINT64 retryWaitTime)
    {
        return getExponentialBackoffRetryStrategyWaitTime(pKvsRetryStrategy, retryWaitTime);
    }

    STATUS readFrameData(PBYTE pFrame, PUINT32 pSize, UINT32 index, PCHAR frameFilePath, RTC_CODEC rtcCodec)
    {
        STATUS retStatus = STATUS_SUCCESS;
        CHAR filePath[MAX_PATH_LEN + 1];
        UINT64 size = 0;

        CHK(pFrame != NULL && pSize != NULL, STATUS_NULL_ARG);

        switch (rtcCodec) {
            case RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE:
                SNPRINTF(filePath, MAX_PATH_LEN, "%s/frame-%04d.h264", frameFilePath, index);
                break;
            case RTC_CODEC_H265:
                SNPRINTF(filePath, MAX_PATH_LEN, "%s/frame-%04d.h265", frameFilePath, index);
                break;
            case RTC_CODEC_OPUS:
                SNPRINTF(filePath, MAX_PATH_LEN, "%s/sample-%03d.opus", frameFilePath, index);
                break;
            default:
                break;
        }

        // Get the size and read into frame
        CHK_STATUS(readFile(filePath, TRUE, NULL, &size));
        CHK_STATUS(readFile(filePath, TRUE, pFrame, &size));

        *pSize = (UINT32) size;

    CleanUp:

        return retStatus;
    }

    // Extract NAL units from an Annex-B buffer, handling both 3- and 4-byte start codes.
    // Returns the number of NALs found; populates naluOffsets/naluLengths with positions
    // of each NAL's payload (i.e. after the start code).
    static UINT32 extractNaluInfo(PBYTE data, UINT32 dataLen, PUINT32 naluOffsets, PUINT32 naluLengths, UINT32 maxNalus)
    {
        UINT32 naluCount = 0, i = 0, naluStart = 0;
        while (i < dataLen && naluCount < maxNalus) {
            if (i + 2 < dataLen && data[i] == 0 && data[i + 1] == 0) {
                UINT32 startCodeLen = 0;
                if (data[i + 2] == 1) {
                    startCodeLen = 3;
                } else if (i + 3 < dataLen && data[i + 2] == 0 && data[i + 3] == 1) {
                    startCodeLen = 4;
                } else {
                    i++;
                    continue;
                }
                if (naluCount > 0) {
                    naluLengths[naluCount - 1] = i - naluStart;
                }
                naluStart = i + startCodeLen;
                naluOffsets[naluCount] = naluStart;
                naluCount++;
                i += startCodeLen;
            } else {
                i++;
            }
        }
        if (naluCount > 0) {
            naluLengths[naluCount - 1] = dataLen - naluStart;
        }
        return naluCount;
    }

    bool connectTwoPeers(PRtcPeerConnection offerPc, PRtcPeerConnection answerPc, PCHAR pOfferCertFingerprint = NULL,
                         PCHAR pAnswerCertFingerprint = NULL);

    // Non-trickle variant: starts gathering on both PCs, waits for each to signal gathering completion, then
    // exchanges post-gathering SDPs (containing a=candidate lines inline). Optional strip flags remove all
    // a=candidate: lines from the offer or answer SDP before it is handed to the remote peer — use either to
    // exercise peer-reflexive discovery on the receiving side. Returns true iff both PCs reach CONNECTED.
    bool connectTwoPeersNoTrickle(PRtcPeerConnection offerPc, PRtcPeerConnection answerPc, bool stripOfferCandidates = false,
                                  bool stripAnswerCandidates = false);
    void addTrackToPeerConnection(PRtcPeerConnection pRtcPeerConnection, PRtcMediaStreamTrack track, PRtcRtpTransceiver* transceiver, RTC_CODEC codec,
                                  MEDIA_STREAM_TRACK_KIND kind);
    void getIceServers(PRtcConfiguration pRtcConfiguration);
    static void initRtcConfiguration(PRtcConfiguration pRtcConfiguration);

    // Reads `count` raw frame files from `folder` via readFrameData into a
    // vector of TestFrames. File index starts at `firstIndex` (H.264/H.265
    // sample folders are 1-based, Opus sample folders are 0-based). Each
    // TestFrame's `sendPts` is set to `i * frameDuration` and `timescale`
    // to the caller-supplied value, so the default call (H.264 at 25 fps
    // in hundreds-of-nanos) matches the fullCycleVideoAudioDataChannel
    // PTS convention with no post-load fixups. The H.264 jitter integration
    // test overrides timescale/frameDuration to get 30 fps in 90 kHz RTP
    // ticks; fullCycle's Opus loader passes RTC_CODEC_OPUS and firstIndex=0.
    std::vector<TestFrame> loadFramesFromFolder(PCHAR folder, UINT32 count,
                                                RTC_CODEC codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE,
                                                UINT32 timescale = HUNDREDS_OF_NANOS_IN_A_SECOND,
                                                UINT64 frameDuration = HUNDREDS_OF_NANOS_IN_A_SECOND / 25, UINT32 firstIndex = 1);

    // Parses NAL units out of both frames via extractNaluInfo and asserts
    // that the two frames contain the same number of NAL units and that
    // each NAL unit matches in length and byte content. Does not compare
    // total byte size — the depayloader may emit 3-byte start codes where
    // the source had 4-byte ones (or vice versa), so frames can legitimately
    // differ by one byte per NAL boundary on a perfect round trip.
    // `context` is prefixed to every gtest diagnostic message.
    static void expectTestFramesNalUnitsEqual(const TestFrame& expected, const TestFrame& actual, const char* context);

  protected:
    virtual void SetUp();
    virtual void TearDown();
    PCHAR GetTestName();

#ifdef ENABLE_SIGNALING
    PAwsCredentialProvider mTestCredentialProvider;
#endif

    PCHAR mAccessKey;
    PCHAR mSecretKey;
    PCHAR mSessionToken;
    PCHAR mRegion;
    PCHAR mCaCertPath;
    UINT64 mStreamingRotationPeriod;
    UINT32 mLogLevel;

    SIZE_T stateChangeCount[RTC_PEER_CONNECTION_TOTAL_STATE_COUNT] = {0};

    CHAR mDefaultRegion[128 + 1];
    BOOL mAccessKeyIdSet;
    CHAR mChannelName[MAX_CHANNEL_NAME_LEN + 1];
    CHAR mChannelArn[MAX_ARN_LEN + 1];
    CHAR mStreamArn[MAX_ARN_LEN + 1];
    CHAR mKmsKeyId[MAX_ARN_LEN + 1];

    ChannelInfo mChannelInfo;
    SignalingClientCallbacks mSignalingClientCallbacks;
    SignalingClientInfo mClientInfo;
    Tag mTags[3];
};

typedef struct {
    PRtcPeerConnection pc;
    WebRtcClientTestBase* client;
} PeerContainer, *PPeerContainer;

} // namespace webrtcclient
} // namespace video
} // namespace kinesis
} // namespace amazonaws
} // namespace com
