/**
 * H264 Jitter Buffer Simulator
 *
 * Standalone tool that simulates the H264 jitter buffer pipeline:
 *   read H264 frames -> packetize to RTP -> apply loss -> jitter buffer -> output H264 stream
 *
 * Usage: h264JitterBufferSim --input <dir> [options]
 */

#include <vector>
#include <set>
#include <map>
#include <random>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <string>

extern "C" {
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
#include "Rtp/RtpPacket.h"
#include "PeerConnection/JitterBuffer.h"
#include "Rtp/Codecs/RtpH264Payloader.h"
}

#define SIM_DEFAULT_MTU            1200
#define SIM_DEFAULT_CLOCK_RATE     90000
#define SIM_DEFAULT_SSRC           0x12345678
#define SIM_DEFAULT_PAYLOAD_TYPE   96
#define SIM_DEFAULT_FPS            30
#define SIM_DEFAULT_MAX_LATENCY_MS 5000
#define SIM_DEFAULT_SEED           42
#define SIM_FRAME_BUFFER_SIZE      500000

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum LossModel {
    LOSS_NONE,
    LOSS_RANDOM,
    LOSS_BURST,
    LOSS_PERIODIC,
    LOSS_GILBERT_ELLIOTT,
};

struct RtpPacketInfo {
    PRtpPacket pPacket;
    UINT32 frameIndex;
    UINT32 timestamp;
    UINT16 sequenceNumber;
    UINT32 payloadLength;
    BYTE nalIndicator;
    BYTE fuHeader;
};

struct SimConfig {
    CHAR inputDir[512];
    CHAR outputFile[512];
    UINT32 numFrames;       // 0 = auto-detect
    UINT32 mtu;
    UINT32 clockRate;
    UINT32 ssrc;
    UINT8 payloadType;
    UINT32 fps;
    UINT32 maxLatencyMs;

    LossModel lossModel;
    DOUBLE randomRate;
    UINT32 burstSize;
    UINT32 burstCount;
    UINT32 periodicPeriod;
    DOUBLE geP;
    DOUBLE geR;
    DOUBLE geLossGood;
    DOUBLE geLossBad;
    UINT32 seed;

    BOOL includePartial;
};

struct FrameLossAnalysis {
    UINT32 framesFullyDropped;
    UINT32 framesPartiallyDropped;
    UINT32 framesPartiallyDelivered;
    UINT32 framesIntact;
};

struct SimState {
    SimConfig config;
    PJitterBuffer pJitterBuffer;

    std::vector<std::vector<BYTE>> originalFrames;
    std::vector<RtpPacketInfo> allPackets;
    std::set<UINT32> dropIndices;

    std::vector<UINT32> receivedFrameTimestamps;
    std::vector<UINT32> droppedFrameTimestamps;

    FILE* outputFp;

    UINT32 totalPacketsSent;
    UINT32 totalFramesSent;
    UINT32 totalFramesReceived;
    UINT32 totalFramesDropped;
    UINT32 framesReceivedFull;
    UINT32 framesReceivedPartial;
    UINT32 framesDroppedPartialRecovered;
    UINT32 framesWritten;

    FrameLossAnalysis analysis;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void printUsage(const char* progName);
static BOOL parseArgs(int argc, char* argv[], SimConfig* config);
static STATUS loadFrames(SimState* state);
static STATUS packetizeFrame(SimState* state, UINT32 frameIndex, UINT32 timestamp, UINT16* pSeqNum);
static void packetizeAllFrames(SimState* state);
static std::set<UINT32> generateDropSet(const SimConfig* config, UINT32 totalPackets);
static bool isStartingPacket(const RtpPacketInfo& pkt);
static FrameLossAnalysis analyzeFrameLoss(const SimState* state);
static bool isFramePartial(const SimState* state, UINT32 timestamp);
static STATUS frameReadyCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize);
static STATUS frameDroppedCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 timestamp);
static void pushPackets(SimState* state);
static void printStatistics(const SimState* state);
static void cleanup(SimState* state);

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void printUsage(const char* progName)
{
    printf("Usage: %s --input <dir> [options]\n\n", progName);
    printf("Options:\n");
    printf("  --input DIR              Input directory with frame-XXXX.h264 files (required)\n");
    printf("  --output FILE            Output raw H264 file (default: output.h264)\n");
    printf("  --frames N               Number of frames to read, 0=all (default: 0)\n");
    printf("  --mtu N                  RTP MTU (default: %d)\n", SIM_DEFAULT_MTU);
    printf("  --max-latency N          Jitter buffer max latency in ms (default: %d)\n", SIM_DEFAULT_MAX_LATENCY_MS);
    printf("  --fps N                  Frames per second (default: %d)\n", SIM_DEFAULT_FPS);
    printf("\nLoss model (pick one):\n");
    printf("  --loss none              No packet loss (default)\n");
    printf("  --loss random <percent>  Random loss, e.g. 5 for 5%%\n");
    printf("  --loss burst <size> <count>\n");
    printf("  --loss periodic <period> Drop every Nth packet\n");
    printf("  --loss ge <p> <r> [lossGood lossBad]  Gilbert-Elliott model\n");
    printf("\nPartial frame handling:\n");
    printf("  --include-partial        Write partially delivered frames (default)\n");
    printf("  --exclude-partial        Skip partially delivered frames\n");
    printf("\nOther:\n");
    printf("  --seed N                 RNG seed (default: %d)\n", SIM_DEFAULT_SEED);
    printf("  -h, --help               Show this help\n");
}

static BOOL parseArgs(int argc, char* argv[], SimConfig* config)
{
    // Defaults
    MEMSET(config, 0, SIZEOF(SimConfig));
    STRCPY(config->outputFile, "output.h264");
    config->mtu = SIM_DEFAULT_MTU;
    config->clockRate = SIM_DEFAULT_CLOCK_RATE;
    config->ssrc = SIM_DEFAULT_SSRC;
    config->payloadType = SIM_DEFAULT_PAYLOAD_TYPE;
    config->fps = SIM_DEFAULT_FPS;
    config->maxLatencyMs = SIM_DEFAULT_MAX_LATENCY_MS;
    config->lossModel = LOSS_NONE;
    config->seed = SIM_DEFAULT_SEED;
    config->includePartial = TRUE;
    config->geLossGood = 0.0;
    config->geLossBad = 1.0;

    BOOL hasInput = FALSE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return FALSE;
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            STRNCPY(config->inputDir, argv[++i], SIZEOF(config->inputDir) - 1);
            hasInput = TRUE;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            STRNCPY(config->outputFile, argv[++i], SIZEOF(config->outputFile) - 1);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            config->numFrames = (UINT32) atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mtu") == 0 && i + 1 < argc) {
            config->mtu = (UINT32) atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-latency") == 0 && i + 1 < argc) {
            config->maxLatencyMs = (UINT32) atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            config->fps = (UINT32) atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            config->seed = (UINT32) atoi(argv[++i]);
        } else if (strcmp(argv[i], "--include-partial") == 0) {
            config->includePartial = TRUE;
        } else if (strcmp(argv[i], "--exclude-partial") == 0) {
            config->includePartial = FALSE;
        } else if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "none") == 0) {
                config->lossModel = LOSS_NONE;
            } else if (strcmp(argv[i], "random") == 0 && i + 1 < argc) {
                config->lossModel = LOSS_RANDOM;
                config->randomRate = atof(argv[++i]) / 100.0;
            } else if (strcmp(argv[i], "burst") == 0 && i + 2 < argc) {
                config->lossModel = LOSS_BURST;
                config->burstSize = (UINT32) atoi(argv[++i]);
                config->burstCount = (UINT32) atoi(argv[++i]);
            } else if (strcmp(argv[i], "periodic") == 0 && i + 1 < argc) {
                config->lossModel = LOSS_PERIODIC;
                config->periodicPeriod = (UINT32) atoi(argv[++i]);
            } else if (strcmp(argv[i], "ge") == 0 && i + 2 < argc) {
                config->lossModel = LOSS_GILBERT_ELLIOTT;
                config->geP = atof(argv[++i]);
                config->geR = atof(argv[++i]);
                if (i + 2 < argc && argv[i + 1][0] != '-') {
                    config->geLossGood = atof(argv[++i]);
                    config->geLossBad = atof(argv[++i]);
                }
            } else {
                printf("Error: unknown loss model '%s'\n", argv[i]);
                return FALSE;
            }
        } else {
            printf("Error: unknown option '%s'\n", argv[i]);
            printUsage(argv[0]);
            return FALSE;
        }
    }

    if (!hasInput) {
        printf("Error: --input is required\n\n");
        printUsage(argv[0]);
        return FALSE;
    }

    return TRUE;
}

// ---------------------------------------------------------------------------
// Frame loading
// ---------------------------------------------------------------------------

static STATUS loadFrames(SimState* state)
{
    CHAR filePath[MAX_PATH_LEN + 1];
    UINT64 size = 0;
    BYTE frameBuffer[SIM_FRAME_BUFFER_SIZE];

    for (UINT32 i = 1; ; i++) {
        SNPRINTF(filePath, MAX_PATH_LEN, "%s/frame-%04d.h264", state->config.inputDir, i);

        size = 0;
        STATUS status = readFile(filePath, TRUE, NULL, &size);
        if (STATUS_FAILED(status)) {
            break; // no more files
        }
        if (size > SIM_FRAME_BUFFER_SIZE) {
            printf("Warning: frame %u too large (%llu bytes), skipping\n", i, (unsigned long long) size);
            continue;
        }

        status = readFile(filePath, TRUE, frameBuffer, &size);
        if (STATUS_FAILED(status)) {
            printf("Warning: failed to read %s\n", filePath);
            break;
        }

        state->originalFrames.push_back(std::vector<BYTE>(frameBuffer, frameBuffer + (UINT32) size));

        if (state->config.numFrames > 0 && i >= state->config.numFrames) {
            break;
        }
    }

    if (state->originalFrames.empty()) {
        printf("Error: no frames loaded from %s\n", state->config.inputDir);
        return STATUS_INVALID_ARG;
    }

    printf("Loaded %zu frames from %s\n", state->originalFrames.size(), state->config.inputDir);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// RTP packetization (ported from H264JitterBufferIntegrationTest::packetizeFrame)
// ---------------------------------------------------------------------------

static STATUS packetizeFrame(SimState* state, UINT32 frameIndex, UINT32 timestamp, UINT16* pSeqNum)
{
    STATUS retStatus = STATUS_SUCCESS;
    PayloadArray payloadArray;
    MEMSET(&payloadArray, 0, SIZEOF(payloadArray));
    PBYTE frameData = state->originalFrames[frameIndex].data();
    UINT32 frameSize = (UINT32) state->originalFrames[frameIndex].size();
    PRtpPacket pPacketList = NULL;
    PRtpPacket pPacketCopy = NULL;
    UINT32 packetSize = 0;
    PBYTE rawPacket = NULL;

    // Get required sizes
    CHK_STATUS(createPayloadForH264(state->config.mtu, frameData, frameSize, NULL,
                                    &payloadArray.payloadLength, NULL,
                                    &payloadArray.payloadSubLenSize));

    // Allocate buffers
    payloadArray.payloadBuffer = (PBYTE) MEMALLOC(payloadArray.payloadLength);
    payloadArray.payloadSubLength = (PUINT32) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(UINT32));
    CHK(payloadArray.payloadBuffer != NULL && payloadArray.payloadSubLength != NULL, STATUS_NOT_ENOUGH_MEMORY);

    // Fill payload data
    CHK_STATUS(createPayloadForH264(state->config.mtu, frameData, frameSize,
                                    payloadArray.payloadBuffer, &payloadArray.payloadLength,
                                    payloadArray.payloadSubLength, &payloadArray.payloadSubLenSize));

    // Create RTP packets
    pPacketList = (PRtpPacket) MEMALLOC(payloadArray.payloadSubLenSize * SIZEOF(RtpPacket));
    CHK(pPacketList != NULL, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(constructRtpPackets(&payloadArray, state->config.payloadType, *pSeqNum,
                                   timestamp, state->config.ssrc,
                                   pPacketList, payloadArray.payloadSubLenSize));

    // Store packet info (create owned copies)
    for (UINT32 i = 0; i < payloadArray.payloadSubLenSize; i++) {
        pPacketCopy = NULL;
        rawPacket = NULL;

        packetSize = RTP_GET_RAW_PACKET_SIZE(&pPacketList[i]);
        rawPacket = (PBYTE) MEMALLOC(packetSize);
        CHK(rawPacket != NULL, STATUS_NOT_ENOUGH_MEMORY);

        CHK_STATUS(createBytesFromRtpPacket(&pPacketList[i], rawPacket, &packetSize));
        CHK_STATUS(createRtpPacketFromBytes(rawPacket, packetSize, &pPacketCopy));
        rawPacket = NULL; // createRtpPacketFromBytes takes ownership

        RtpPacketInfo info;
        info.pPacket = pPacketCopy;
        info.frameIndex = frameIndex;
        info.timestamp = timestamp;
        info.sequenceNumber = pPacketCopy->header.sequenceNumber;
        info.payloadLength = pPacketCopy->payloadLength;
        info.nalIndicator = (pPacketCopy->payloadLength > 0) ? pPacketCopy->payload[0] : 0;
        info.fuHeader = (pPacketCopy->payloadLength > 1) ? pPacketCopy->payload[1] : 0;
        state->allPackets.push_back(info);
    }

    *pSeqNum = GET_UINT16_SEQ_NUM(*pSeqNum + payloadArray.payloadSubLenSize);
    state->totalFramesSent++;

CleanUp:
    SAFE_MEMFREE(payloadArray.payloadBuffer);
    SAFE_MEMFREE(payloadArray.payloadSubLength);
    SAFE_MEMFREE(pPacketList);
    SAFE_MEMFREE(rawPacket);

    return retStatus;
}

static void packetizeAllFrames(SimState* state)
{
    UINT16 seqNum = 0;
    UINT32 timestamp = 0;

    for (UINT32 i = 0; i < (UINT32) state->originalFrames.size(); i++) {
        STATUS status = packetizeFrame(state, i, timestamp, &seqNum);
        if (STATUS_FAILED(status)) {
            printf("Error: failed to packetize frame %u (status=0x%08x)\n", i, status);
            return;
        }
        timestamp += state->config.clockRate / state->config.fps;
    }
    printf("Created %zu RTP packets from %u frames\n", state->allPackets.size(), state->totalFramesSent);
}

// ---------------------------------------------------------------------------
// Loss generation (ported from test's DropGenerator lambdas)
// ---------------------------------------------------------------------------

static std::set<UINT32> generateDropSet(const SimConfig* config, UINT32 totalPackets)
{
    std::set<UINT32> dropIndices;
    std::mt19937 gen(config->seed);
    std::uniform_real_distribution<> dis(0.0, 1.0);

    switch (config->lossModel) {
        case LOSS_NONE:
            break;

        case LOSS_RANDOM:
            for (UINT32 i = 0; i < totalPackets; i++) {
                if (dis(gen) < config->randomRate) {
                    dropIndices.insert(i);
                }
            }
            break;

        case LOSS_BURST: {
            if (config->burstCount == 0 || config->burstSize == 0) break;
            UINT32 burstInterval = totalPackets / (config->burstCount + 1);
            for (UINT32 b = 0; b < config->burstCount; b++) {
                UINT32 burstStart = burstInterval * (b + 1);
                for (UINT32 i = 0; i < config->burstSize && (burstStart + i) < totalPackets; i++) {
                    dropIndices.insert(burstStart + i);
                }
            }
            break;
        }

        case LOSS_PERIODIC:
            if (config->periodicPeriod == 0) break;
            for (UINT32 i = config->periodicPeriod - 1; i < totalPackets; i += config->periodicPeriod) {
                dropIndices.insert(i);
            }
            break;

        case LOSS_GILBERT_ELLIOTT: {
            bool inBadState = false;
            for (UINT32 i = 0; i < totalPackets; i++) {
                if (inBadState) {
                    if (dis(gen) < config->geR) {
                        inBadState = false;
                    }
                } else {
                    if (dis(gen) < config->geP) {
                        inBadState = true;
                    }
                }
                DOUBLE pLoss = inBadState ? config->geLossBad : config->geLossGood;
                if (dis(gen) < pLoss) {
                    dropIndices.insert(i);
                }
            }
            break;
        }
    }

    return dropIndices;
}

// ---------------------------------------------------------------------------
// Frame loss analysis (ported from test's analyzeFrameLoss)
// ---------------------------------------------------------------------------

static bool isStartingPacket(const RtpPacketInfo& pkt)
{
    BYTE nalType = pkt.nalIndicator & 0x1F;
    if (nalType == 28) {
        // FU-A: check Start bit in FU header
        return (pkt.fuHeader & 0x80) != 0;
    } else if (nalType >= 1 && nalType <= 23) {
        return true;
    } else if (nalType == 24 || nalType == 25) {
        // STAP-A or STAP-B
        return true;
    }
    return false;
}

static FrameLossAnalysis analyzeFrameLoss(const SimState* state)
{
    std::map<UINT32, std::vector<UINT32>> packetIndicesByFrame;
    for (UINT32 i = 0; i < (UINT32) state->allPackets.size(); i++) {
        packetIndicesByFrame[state->allPackets[i].frameIndex].push_back(i);
    }

    FrameLossAnalysis result = {0, 0, 0, 0};
    for (const auto& kv : packetIndicesByFrame) {
        const std::vector<UINT32>& packetIndices = kv.second;
        UINT32 totalPackets = (UINT32) packetIndices.size();
        UINT32 droppedPackets = 0;

        std::vector<UINT32> remainingIndices;
        for (UINT32 pktIdx : packetIndices) {
            if (state->dropIndices.count(pktIdx)) {
                droppedPackets++;
            } else {
                remainingIndices.push_back(pktIdx);
            }
        }

        if (droppedPackets == 0) {
            result.framesIntact++;
        } else if (droppedPackets == totalPackets) {
            result.framesFullyDropped++;
        } else {
            bool hasStart = !remainingIndices.empty() && isStartingPacket(state->allPackets[remainingIndices[0]]);
            bool isContinuous = true;

            for (size_t i = 1; i < remainingIndices.size() && isContinuous; i++) {
                UINT16 prevSeq = state->allPackets[remainingIndices[i - 1]].sequenceNumber;
                UINT16 curSeq = state->allPackets[remainingIndices[i]].sequenceNumber;
                UINT16 expectedSeq = (prevSeq + 1) & 0xFFFF;
                if (curSeq != expectedSeq) {
                    isContinuous = false;
                }
            }

            if (hasStart && isContinuous) {
                result.framesPartiallyDelivered++;
            } else {
                result.framesPartiallyDropped++;
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Partial frame detection
// ---------------------------------------------------------------------------

static bool isFramePartial(const SimState* state, UINT32 timestamp)
{
    for (UINT32 i = 0; i < (UINT32) state->allPackets.size(); i++) {
        if (state->allPackets[i].timestamp == timestamp) {
            if (state->dropIndices.count(i)) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Jitter buffer callbacks
// ---------------------------------------------------------------------------

static STATUS frameReadyCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize)
{
    SimState* state = (SimState*) customData;

    if (frameSize == 0) {
        return STATUS_SUCCESS;
    }

    PBYTE frameBuffer = (PBYTE) MEMALLOC(frameSize);
    if (frameBuffer == NULL) {
        return STATUS_SUCCESS;
    }

    UINT32 filledSize = 0;
    STATUS status = jitterBufferFillFrameData(
        state->pJitterBuffer, frameBuffer, frameSize, &filledSize,
        startIndex, endIndex);

    if (STATUS_SUCCEEDED(status) && filledSize == frameSize) {
        // Look up timestamp
        UINT32 ts = 0;
        for (const auto& pkt : state->allPackets) {
            if (pkt.sequenceNumber == startIndex) {
                ts = pkt.timestamp;
                break;
            }
        }

        bool partial = isFramePartial(state, ts);

        // With --exclude-partial, skip partial frames entirely (no counting, no writing)
        if (partial && !state->config.includePartial) {
            MEMFREE(frameBuffer);
            return STATUS_SUCCESS;
        }

        state->totalFramesReceived++;
        state->receivedFrameTimestamps.push_back(ts);

        if (partial) {
            state->framesReceivedPartial++;
        } else {
            state->framesReceivedFull++;
        }

        if (state->outputFp != NULL) {
            fwrite(frameBuffer, 1, frameSize, state->outputFp);
            state->framesWritten++;
        }
    }

    MEMFREE(frameBuffer);
    return STATUS_SUCCESS;
}

// Replicates PeerConnection.c onFrameDroppedFunc: extracts partial frame data
// from surviving packets in the jitter buffer and writes to output.
static STATUS frameDroppedCallback(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 timestamp)
{
    // printf("frameDroppedCallback start %u end %u ts %u\n", startIndex, endIndex, timestamp);

    STATUS retStatus = STATUS_SUCCESS;
    SimState* state = (SimState*) customData;
    UINT64 hashValue = 0;
    PRtpPacket pPacket = NULL;
    UINT16 index;
    UINT32 partialFrameSize = 0;
    UINT32 totalPartialSize = 0;
    UINT32 filledSize = 0;
    PBYTE pFrameBuffer = NULL;
    PBYTE pCurPtr = NULL;
    BOOL hasEntry = FALSE;
    BOOL isFirstInFrame = TRUE;

    state->totalFramesDropped++;
    state->droppedFrameTimestamps.push_back(timestamp);

    // With --exclude-partial, skip extraction entirely (no counting, no writing)
    CHK(state->config.includePartial, retStatus);
    CHK(state->pJitterBuffer != NULL, retStatus);

    // First pass: calculate total size of available packets
    // NOTE: use local status instead of CHK_STATUS to avoid aborting the entire
    // frame when a single packet's depay fails (e.g. truncated FU-A fragment).
    isFirstInFrame = TRUE;
    for (index = startIndex; UINT16_DEC(index) != endIndex; index++) {

        if (STATUS_FAILED(hashTableContains(state->pJitterBuffer->pPkgBufferHashTable, index, &hasEntry))) {
            continue;
        }
        if (hasEntry) {
            if (STATUS_FAILED(hashTableGet(state->pJitterBuffer->pPkgBufferHashTable, index, &hashValue))) {
                continue;
            }
            pPacket = (PRtpPacket) hashValue;
            partialFrameSize = 0;
            BOOL depayIsFirst = isFirstInFrame;
            if (STATUS_FAILED(state->pJitterBuffer->depayPayloadFn(pPacket->payload, pPacket->payloadLength, NULL, &partialFrameSize, &depayIsFirst))) {
                printf("Warning: depay size query failed for packet index %u, skipping\n", index);
                continue;
            }
            totalPartialSize += partialFrameSize;
            isFirstInFrame = FALSE;
        }
    }

    CHK(totalPartialSize > 0, retStatus);

    // Allocate buffer and fill with depayloaded data
    pFrameBuffer = (PBYTE) MEMALLOC(totalPartialSize);
    CHK(pFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);

    // Fill pass: continue on failure — same rationale as the size calculation loop.
    isFirstInFrame = TRUE;
    pCurPtr = pFrameBuffer;
    for (index = startIndex; UINT16_DEC(index) != endIndex; index++) {
        if (STATUS_FAILED(hashTableContains(state->pJitterBuffer->pPkgBufferHashTable, index, &hasEntry))) {
            continue;
        }
        if (hasEntry) {
            if (STATUS_FAILED(hashTableGet(state->pJitterBuffer->pPkgBufferHashTable, index, &hashValue))) {
                continue;
            }
            pPacket = (PRtpPacket) hashValue;
            partialFrameSize = totalPartialSize - filledSize;
            BOOL depayIsFirst = isFirstInFrame;
            if (STATUS_FAILED(state->pJitterBuffer->depayPayloadFn(pPacket->payload, pPacket->payloadLength, pCurPtr, &partialFrameSize, &depayIsFirst))) {
                printf("Warning: depay fill failed for packet index %u, skipping\n", index);
                continue;
            }
            pCurPtr += partialFrameSize;
            filledSize += partialFrameSize;
            isFirstInFrame = FALSE;
        }
    }

    // Write partial frame to output (includePartial is guaranteed true here)
    if (filledSize > 0) {
        state->framesDroppedPartialRecovered++;
        if (state->outputFp != NULL) {
            fwrite(pFrameBuffer, 1, filledSize, state->outputFp);
            state->framesWritten++;
        }
    }

CleanUp:
    SAFE_MEMFREE(pFrameBuffer);
    return STATUS_SUCCESS; // Always return success to not break jitter buffer
}

// ---------------------------------------------------------------------------
// Packet push
// ---------------------------------------------------------------------------

static void pushPackets(SimState* state)
{
    for (UINT32 i = 0; i < (UINT32) state->allPackets.size(); i++) {
        if (state->dropIndices.count(i)) {
            continue; // simulate loss
        }

        auto& info = state->allPackets[i];
        if (info.pPacket == NULL) {
            continue;
        }

        BOOL discarded = FALSE;
        STATUS status = jitterBufferPush(state->pJitterBuffer, info.pPacket, &discarded);
        if (STATUS_FAILED(status)) {
            printf("Warning: jitterBufferPush failed for packet %u (status=0x%08x)\n", i, status);
            continue;
        }

        if (!discarded) {
            state->totalPacketsSent++;
        }
        // Jitter buffer owns the packet now
        info.pPacket = NULL;
    }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

static const char* lossModelName(const SimConfig* config)
{
    switch (config->lossModel) {
        case LOSS_NONE:             return "none";
        case LOSS_RANDOM:           return "random";
        case LOSS_BURST:            return "burst";
        case LOSS_PERIODIC:         return "periodic";
        case LOSS_GILBERT_ELLIOTT:  return "gilbert-elliott";
        default:                    return "unknown";
    }
}

static void printStatistics(const SimState* state)
{
    const SimConfig* c = &state->config;
    UINT32 totalPackets = (UINT32) state->allPackets.size();
    UINT32 packetsDropped = (UINT32) state->dropIndices.size();

    printf("\n=== H264 Jitter Buffer Simulation Results ===\n");
    printf("Input:              %s (%zu frames)\n", c->inputDir, state->originalFrames.size());

    printf("Loss model:         %s", lossModelName(c));
    switch (c->lossModel) {
        case LOSS_RANDOM:
            printf(" (%.1f%%)", c->randomRate * 100.0);
            break;
        case LOSS_BURST:
            printf(" (%u bursts of %u packets)", c->burstCount, c->burstSize);
            break;
        case LOSS_PERIODIC:
            printf(" (every %u)", c->periodicPeriod);
            break;
        case LOSS_GILBERT_ELLIOTT:
            printf(" (p=%.3f, r=%.3f)", c->geP, c->geR);
            break;
        default:
            break;
    }
    printf("\n");

    printf("Partial frames:     %s\n", c->includePartial ? "included" : "excluded");
    printf("MTU:                %u\n", c->mtu);
    printf("Max latency:        %u ms\n", c->maxLatencyMs);

    printf("\nPackets:  %u total, %u dropped (%.1f%%), %u pushed\n",
           totalPackets, packetsDropped,
           totalPackets > 0 ? (DOUBLE) packetsDropped / totalPackets * 100.0 : 0.0,
           state->totalPacketsSent);

    const FrameLossAnalysis* a = &state->analysis;
    printf("Frames:   %u total\n", state->totalFramesSent);
    printf("  Intact:              %u\n", a->framesIntact);
    printf("  Partially dropped:   %u\n", a->framesPartiallyDropped);
    printf("  Partially delivered: %u\n", a->framesPartiallyDelivered);
    printf("  Fully dropped:       %u\n", a->framesFullyDropped);

    printf("\nReceived: %u  (%u full + %u partial)\n",
           state->totalFramesReceived, state->framesReceivedFull, state->framesReceivedPartial);
    printf("Dropped:  %u  (%u partial recovered)\n", state->totalFramesDropped, state->framesDroppedPartialRecovered);

    UINT32 accountedFrames = state->totalFramesReceived + state->totalFramesDropped;
    UINT32 expectedAccounted = state->totalFramesSent - a->framesFullyDropped;
    if (accountedFrames != expectedAccounted) {
        printf("WARNING: frame accounting mismatch: received(%u) + dropped(%u) = %u, expected %u\n",
               state->totalFramesReceived, state->totalFramesDropped,
               accountedFrames, expectedAccounted);
    }

    UINT32 totalLost = a->framesFullyDropped + (state->totalFramesDropped - state->framesDroppedPartialRecovered);
    DOUBLE pctLost = state->totalFramesSent > 0 ? (DOUBLE) totalLost / state->totalFramesSent * 100.0 : 0.0;
    printf("Lost:     %u / %u (%.1f%%) frames not delivered at all\n",
           totalLost, state->totalFramesSent, pctLost);

    printf("\nOutput:   %u frames written to %s\n", state->framesWritten, state->config.outputFile);
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

static void cleanup(SimState* state)
{
    // Free any packets still owned by us (dropped packets that were never pushed)
    for (auto& info : state->allPackets) {
        if (info.pPacket != NULL) {
            freeRtpPacket(&info.pPacket);
        }
    }
    state->allPackets.clear();

    if (state->pJitterBuffer != NULL) {
        freeJitterBuffer(&state->pJitterBuffer);
        state->pJitterBuffer = NULL;
    }

    if (state->outputFp != NULL) {
        fclose(state->outputFp);
        state->outputFp = NULL;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

INT32 main(INT32 argc, CHAR* argv[])
{
    SimState state;
    MEMSET(&state, 0, SIZEOF(SimState));
    state.pJitterBuffer = NULL;
    state.outputFp = NULL;

    if (!parseArgs(argc, argv, &state.config)) {
        return 1;
    }

    // Initialize KVS WebRTC (needed for MEMALLOC, logging, etc.)
    STATUS retStatus = initKvsWebRtc();
    if (STATUS_FAILED(retStatus)) {
        printf("Error: initKvsWebRtc failed (0x%08x)\n", retStatus);
        return 1;
    }
    SET_LOGGER_LOG_LEVEL(LOG_LEVEL_WARN);

    // Load frames
    retStatus = loadFrames(&state);
    if (STATUS_FAILED(retStatus)) {
        deinitKvsWebRtc();
        return 1;
    }

    // Packetize
    packetizeAllFrames(&state);
    if (state.totalFramesSent == 0) {
        printf("Error: no frames packetized\n");
        cleanup(&state);
        deinitKvsWebRtc();
        return 1;
    }

    // Generate drop set
    state.dropIndices = generateDropSet(&state.config, (UINT32) state.allPackets.size());

    // Analyze expected frame loss
    state.analysis = analyzeFrameLoss(&state);

    // Create jitter buffer
    UINT32 maxLatency = state.config.maxLatencyMs * (UINT32) HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    retStatus = createJitterBuffer(
        frameReadyCallback,
        frameDroppedCallback,
        depayH264FromRtpPayload,
        maxLatency,
        state.config.clockRate,
        (UINT64) &state,
        &state.pJitterBuffer);

    if (STATUS_FAILED(retStatus)) {
        printf("Error: createJitterBuffer failed (0x%08x)\n", retStatus);
        cleanup(&state);
        deinitKvsWebRtc();
        return 1;
    }

    // Open output file
    state.outputFp = fopen(state.config.outputFile, "wb");
    if (state.outputFp == NULL) {
        printf("Error: cannot open output file %s\n", state.config.outputFile);
        cleanup(&state);
        deinitKvsWebRtc();
        return 1;
    }

    // Push packets (skipping dropped ones)
    pushPackets(&state);

    // Flush jitter buffer (delivers/drops remaining frames)
    freeJitterBuffer(&state.pJitterBuffer);
    state.pJitterBuffer = NULL;

    // Close output
    fclose(state.outputFp);
    state.outputFp = NULL;

    // Print results
    printStatistics(&state);

    // Cleanup
    cleanup(&state);
    deinitKvsWebRtc();

    return 0;
}
