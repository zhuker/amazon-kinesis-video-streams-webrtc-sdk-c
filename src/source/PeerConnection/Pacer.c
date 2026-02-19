/*******************************************
Pacer - Smooth packet transmission for congestion control
Based on GCC RFC draft-ietf-rmcat-gcc-02 Section 4
*******************************************/

#define LOG_CLASS "Pacer"
#include "../Include_i.h"

//
// Internal helper functions
//

static PPacerPacket pacerCreatePacket(PBYTE pData, UINT32 size, UINT16 twccSeqNum)
{
    PPacerPacket pPacket = (PPacerPacket) MEMCALLOC(1, SIZEOF(PacerPacket));
    if (pPacket != NULL) {
        pPacket->pData = pData;
        pPacket->size = size;
        pPacket->twccSeqNum = twccSeqNum;
        pPacket->enqueueTimeKvs = GETTIME();
        pPacket->pNext = NULL;
    }
    return pPacket;
}

static VOID pacerFreePacket(PPacerPacket pPacket)
{
    if (pPacket != NULL) {
        SAFE_MEMFREE(pPacket->pData);
        MEMFREE(pPacket);
    }
}

static VOID pacerClearQueue(PPacer pPacer)
{
    PPacerPacket pCurrent, pNext;

    pCurrent = pPacer->pHead;
    while (pCurrent != NULL) {
        pNext = pCurrent->pNext;
        pacerFreePacket(pCurrent);
        pCurrent = pNext;
    }

    pPacer->pHead = NULL;
    pPacer->pTail = NULL;
    pPacer->queueSize = 0;
    pPacer->queueBytes = 0;
}

//
// Public API Implementation
//

STATUS createPacer(PPacer* ppPacer, TIMER_QUEUE_HANDLE timerQueueHandle, PPacerConfig pConfig)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PPacer pPacer = NULL;

    CHK(ppPacer != NULL, STATUS_NULL_ARG);
    CHK(IS_VALID_TIMER_QUEUE_HANDLE(timerQueueHandle), STATUS_INVALID_ARG);

    pPacer = (PPacer) MEMCALLOC(1, SIZEOF(Pacer));
    CHK(pPacer != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pPacer->lock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pPacer->lock), STATUS_INVALID_OPERATION);

    pPacer->timerQueueHandle = timerQueueHandle;
    pPacer->timerId = MAX_UINT32;

    // Set configuration
    if (pConfig != NULL) {
        pPacer->targetBitrateBps = pConfig->initialBitrateBps > 0 ? pConfig->initialBitrateBps : 300000;
        pPacer->maxQueueSize = pConfig->maxQueueSize > 0 ? pConfig->maxQueueSize : PACER_DEFAULT_MAX_QUEUE_SIZE;
        pPacer->maxQueueBytes = pConfig->maxQueueBytes > 0 ? pConfig->maxQueueBytes : PACER_DEFAULT_MAX_QUEUE_BYTES;
        pPacer->pacingFactor = pConfig->pacingFactor > 0 ? pConfig->pacingFactor : PACER_DEFAULT_PACING_FACTOR;
        pPacer->maxQueueTimeKvs = pConfig->maxQueueTimeKvs;  // 0 = disabled
        pPacer->enabled = pConfig->enabled;
    } else {
        pPacer->targetBitrateBps = 300000;  // 300 kbps default
        pPacer->maxQueueSize = PACER_DEFAULT_MAX_QUEUE_SIZE;
        pPacer->maxQueueBytes = PACER_DEFAULT_MAX_QUEUE_BYTES;
        pPacer->pacingFactor = PACER_DEFAULT_PACING_FACTOR;
        pPacer->maxQueueTimeKvs = 0;  // disabled by default
        pPacer->enabled = TRUE;
    }

    // Initialize queue
    pPacer->pHead = NULL;
    pPacer->pTail = NULL;
    pPacer->queueSize = 0;
    pPacer->queueBytes = 0;

    // Initialize timing
    pPacer->lastSendTimeKvs = GETTIME();
    pPacer->budgetBytes = 0;

    // Initialize stats
    MEMSET(&pPacer->stats, 0, SIZEOF(PacerStats));

    *ppPacer = pPacer;
    pPacer = NULL;

CleanUp:
    if (pPacer != NULL) {
        freePacer(&pPacer);
    }

    LEAVES();
    return retStatus;
}

STATUS freePacer(PPacer* ppPacer)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PPacer pPacer = NULL;

    CHK(ppPacer != NULL, STATUS_NULL_ARG);
    pPacer = *ppPacer;
    CHK(pPacer != NULL, retStatus);

    // Stop timer if running
    pacerStop(pPacer);

    // Clear the queue
    if (IS_VALID_MUTEX_VALUE(pPacer->lock)) {
        MUTEX_LOCK(pPacer->lock);
        pacerClearQueue(pPacer);
        MUTEX_UNLOCK(pPacer->lock);
        MUTEX_FREE(pPacer->lock);
    }

    SAFE_MEMFREE(pPacer);
    *ppPacer = NULL;

CleanUp:
    LEAVES();
    return retStatus;
}

STATUS pacerStart(PPacer pPacer, PVOID pKvsPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    UINT32 timerId = MAX_UINT32;

    CHK(pPacer != NULL && pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);
    locked = TRUE;

    pPacer->pKvsPeerConnection = pKvsPeerConnection;

    // Only start timer if enabled and not already running
    CHK(pPacer->enabled && pPacer->timerId == MAX_UINT32, retStatus);

    pPacer->lastSendTimeKvs = GETTIME();
    pPacer->budgetBytes = 0;

    // Drop the pacer lock before calling into the timer queue to avoid
    // lock-order-inversion: pacerStart holds pacer lock then acquires timer
    // queue lock, while timerQueueExecutor holds timer queue lock then
    // acquires pacer lock in pacerTimerCallback.
    MUTEX_UNLOCK(pPacer->lock);
    locked = FALSE;

    // Start periodic timer without holding the pacer lock
    CHK_STATUS(timerQueueAddTimer(pPacer->timerQueueHandle,
                                   PACER_INTERVAL_KVS,         // Initial delay
                                   PACER_INTERVAL_KVS,         // Period
                                   pacerTimerCallback,
                                   (UINT64) pPacer,
                                   &timerId));

    MUTEX_LOCK(pPacer->lock);
    locked = TRUE;
    pPacer->timerId = timerId;

    DLOGD("Pacer started with target bitrate %llu bps", pPacer->targetBitrateBps);

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pPacer->lock);
    }

    LEAVES();
    return retStatus;
}

STATUS pacerStop(PPacer pPacer)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    UINT32 timerId = MAX_UINT32;

    CHK(pPacer != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);
    locked = TRUE;

    if (pPacer->timerId != MAX_UINT32) {
        timerId = pPacer->timerId;
        pPacer->timerId = MAX_UINT32;
    }

    // Drop the pacer lock before calling into the timer queue to maintain
    // consistent lock ordering (see pacerStart comment).
    MUTEX_UNLOCK(pPacer->lock);
    locked = FALSE;

    if (timerId != MAX_UINT32) {
        timerQueueCancelTimer(pPacer->timerQueueHandle, timerId, (UINT64) pPacer);
        DLOGD("Pacer stopped");
    }

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pPacer->lock);
    }

    LEAVES();
    return retStatus;
}

STATUS pacerEnqueuePacket(PPacer pPacer, PBYTE pData, UINT32 size, UINT16 twccSeqNum)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PPacerPacket pPacket = NULL;

    CHK(pPacer != NULL && pData != NULL && size > 0, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);
    locked = TRUE;

    // Check queue limits
    if (pPacer->queueSize >= pPacer->maxQueueSize || pPacer->queueBytes + size > pPacer->maxQueueBytes) {
        // Queue is full, drop the packet
        pPacer->stats.packetsDropped++;
        pPacer->stats.bytesDropped += size;
        DLOGW("Pacer queue full, dropping packet. Queue: %u/%u packets, %u/%u bytes",
              pPacer->queueSize, pPacer->maxQueueSize,
              pPacer->queueBytes, pPacer->maxQueueBytes);
        // Free the packet data since we own it
        SAFE_MEMFREE(pData);
        CHK(FALSE, STATUS_NOT_ENOUGH_MEMORY);
    }

    // Create packet wrapper
    pPacket = pacerCreatePacket(pData, size, twccSeqNum);
    CHK(pPacket != NULL, STATUS_NOT_ENOUGH_MEMORY);

    // Add to tail of queue
    if (pPacer->pTail != NULL) {
        pPacer->pTail->pNext = pPacket;
    } else {
        pPacer->pHead = pPacket;
    }
    pPacer->pTail = pPacket;
    pPacer->queueSize++;
    pPacer->queueBytes += size;

    // Update stats
    pPacer->stats.currentQueueSize = pPacer->queueSize;
    pPacer->stats.currentQueueBytes = pPacer->queueBytes;
    if (pPacer->queueSize > pPacer->stats.maxQueueSizeReached) {
        pPacer->stats.maxQueueSizeReached = pPacer->queueSize;
    }

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pPacer->lock);
    }

    LEAVES();
    return retStatus;
}

STATUS pacerSetTargetBitrate(PPacer pPacer, UINT64 bitrateBps)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pPacer != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);

    // Enforce minimum bitrate
    pPacer->targetBitrateBps = MAX(bitrateBps, PACER_MIN_BITRATE_BPS);
    DLOGV("Pacer target bitrate set to %llu bps", pPacer->targetBitrateBps);

    MUTEX_UNLOCK(pPacer->lock);

CleanUp:
    return retStatus;
}

UINT64 pacerGetTargetBitrate(PPacer pPacer)
{
    UINT64 bitrate = 0;

    if (pPacer == NULL) {
        return 0;
    }

    MUTEX_LOCK(pPacer->lock);
    bitrate = pPacer->targetBitrateBps;
    MUTEX_UNLOCK(pPacer->lock);

    return bitrate;
}

STATUS pacerGetStats(PPacer pPacer, PPacerStats pStats)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pPacer != NULL && pStats != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);
    *pStats = pPacer->stats;
    MUTEX_UNLOCK(pPacer->lock);

CleanUp:
    return retStatus;
}

BOOL pacerIsEnabled(PPacer pPacer)
{
    BOOL enabled = FALSE;

    if (pPacer == NULL) {
        return FALSE;
    }

    MUTEX_LOCK(pPacer->lock);
    enabled = pPacer->enabled;
    MUTEX_UNLOCK(pPacer->lock);

    return enabled;
}

STATUS pacerSetEnabled(PPacer pPacer, BOOL enabled)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pPacer != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);
    pPacer->enabled = enabled;
    MUTEX_UNLOCK(pPacer->lock);

    DLOGI("Pacer %s", enabled ? "enabled" : "disabled");

CleanUp:
    return retStatus;
}

UINT32 pacerGetQueueSize(PPacer pPacer)
{
    UINT32 size = 0;

    if (pPacer == NULL) {
        return 0;
    }

    MUTEX_LOCK(pPacer->lock);
    size = pPacer->queueSize;
    MUTEX_UNLOCK(pPacer->lock);

    return size;
}

STATUS pacerSetMaxQueueTime(PPacer pPacer, UINT64 maxQueueTimeKvs)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pPacer != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);
    pPacer->maxQueueTimeKvs = maxQueueTimeKvs;
    MUTEX_UNLOCK(pPacer->lock);

    DLOGI("Pacer max queue time set to %llu (100ns units)", maxQueueTimeKvs);

CleanUp:
    return retStatus;
}

UINT64 pacerGetMaxQueueTime(PPacer pPacer)
{
    UINT64 maxQueueTimeKvs = 0;

    if (pPacer == NULL) {
        return 0;
    }

    MUTEX_LOCK(pPacer->lock);
    maxQueueTimeKvs = pPacer->maxQueueTimeKvs;
    MUTEX_UNLOCK(pPacer->lock);

    return maxQueueTimeKvs;
}

STATUS pacerEnqueueFrame(PPacer pPacer, PPacerPacketInfo pPackets, UINT32 count)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PPacerPacket pPacket = NULL;
    UINT64 frameEnqueueTime;
    UINT32 totalBytes = 0;
    UINT32 i;

    CHK(pPacer != NULL && pPackets != NULL && count > 0, STATUS_NULL_ARG);

    // Calculate total bytes for queue limit check
    for (i = 0; i < count; i++) {
        CHK(pPackets[i].pData != NULL && pPackets[i].size > 0, STATUS_NULL_ARG);
        totalBytes += pPackets[i].size;
    }

    MUTEX_LOCK(pPacer->lock);
    locked = TRUE;

    // Check queue limits for entire frame
    if (pPacer->queueSize + count > pPacer->maxQueueSize ||
        pPacer->queueBytes + totalBytes > pPacer->maxQueueBytes) {
        // Queue is full, drop the entire frame
        pPacer->stats.packetsDropped += count;
        pPacer->stats.bytesDropped += totalBytes;
        DLOGW("Pacer queue full, dropping frame (%u packets, %u bytes). Queue: %u/%u packets, %u/%u bytes",
              count, totalBytes,
              pPacer->queueSize, pPacer->maxQueueSize,
              pPacer->queueBytes, pPacer->maxQueueBytes);
        // Free all packet data since we own it
        for (i = 0; i < count; i++) {
            SAFE_MEMFREE(pPackets[i].pData);
        }
        CHK(FALSE, STATUS_NOT_ENOUGH_MEMORY);
    }

    // All packets in frame share the same enqueue time
    frameEnqueueTime = GETTIME();

    // Enqueue all packets
    for (i = 0; i < count; i++) {
        pPacket = (PPacerPacket) MEMCALLOC(1, SIZEOF(PacerPacket));
        if (pPacket == NULL) {
            // Memory allocation failed - free remaining packet data
            for (; i < count; i++) {
                SAFE_MEMFREE(pPackets[i].pData);
            }
            CHK(FALSE, STATUS_NOT_ENOUGH_MEMORY);
        }

        pPacket->pData = pPackets[i].pData;
        pPacket->size = pPackets[i].size;
        pPacket->twccSeqNum = pPackets[i].twccSeqNum;
        pPacket->enqueueTimeKvs = frameEnqueueTime;  // Same time for all packets in frame
        pPacket->pNext = NULL;

        // Add to tail of queue
        if (pPacer->pTail != NULL) {
            pPacer->pTail->pNext = pPacket;
        } else {
            pPacer->pHead = pPacket;
        }
        pPacer->pTail = pPacket;
        pPacer->queueSize++;
        pPacer->queueBytes += pPacket->size;
    }

    // Update stats
    pPacer->stats.currentQueueSize = pPacer->queueSize;
    pPacer->stats.currentQueueBytes = pPacer->queueBytes;
    if (pPacer->queueSize > pPacer->stats.maxQueueSizeReached) {
        pPacer->stats.maxQueueSizeReached = pPacer->queueSize;
    }

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pPacer->lock);
    }

    LEAVES();
    return retStatus;
}

//
// Internal functions
//

UINT32 pacerCalculateBudget(PPacer pPacer, UINT64 elapsedTimeKvs)
{
    DOUBLE elapsedSec;
    UINT64 bytesPerInterval;

    if (pPacer == NULL || elapsedTimeKvs == 0) {
        return 0;
    }

    // Convert elapsed time to seconds
    elapsedSec = (DOUBLE) elapsedTimeKvs / HUNDREDS_OF_NANOS_IN_A_SECOND;

    // Calculate bytes allowed: bitrate * time / 8
    // Apply pacing factor to allow faster queue clearing (libwebrtc uses 2.5x)
    bytesPerInterval = (UINT64)(pPacer->targetBitrateBps * elapsedSec * pPacer->pacingFactor / 8.0);

    return (UINT32) MIN(bytesPerInterval, MAX_UINT32);
}

STATUS pacerDrainQueue(PPacer pPacer)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PPacerPacket pPacket = NULL;
    UINT64 now = GETTIME();
    UINT64 elapsedTimeKvs;
    UINT32 bytesSent = 0;
    UINT64 queueDelayKvs;
    PKvsPeerConnection pKvsPeerConnection = NULL;

    CHK(pPacer != NULL, STATUS_NULL_ARG);
    CHK(pPacer->pKvsPeerConnection != NULL, STATUS_INVALID_OPERATION);

    pKvsPeerConnection = (PKvsPeerConnection) pPacer->pKvsPeerConnection;

    // Calculate budget based on time elapsed since last send
    elapsedTimeKvs = now - pPacer->lastSendTimeKvs;
    pPacer->budgetBytes += pacerCalculateBudget(pPacer, elapsedTimeKvs);
    pPacer->lastSendTimeKvs = now;

    // Frame-rate pacing: ensure oldest packet meets deadline
    if (pPacer->maxQueueTimeKvs > 0 && pPacer->pHead != NULL) {
        UINT64 oldestPacketAge = now - pPacer->pHead->enqueueTimeKvs;

        if (oldestPacketAge < pPacer->maxQueueTimeKvs) {
            // Time remaining to clear queue
            UINT64 timeRemainingKvs = pPacer->maxQueueTimeKvs - oldestPacketAge;
            DOUBLE intervalsRemaining = (DOUBLE) timeRemainingKvs / PACER_INTERVAL_KVS;

            if (intervalsRemaining > 0) {
                // Minimum bytes to send this interval to meet deadline
                UINT64 minBudget = (UINT64)(pPacer->queueBytes / intervalsRemaining);

                // Use the larger budget
                if (minBudget > pPacer->budgetBytes) {
                    pPacer->budgetBytes = minBudget;
                }
            }
        } else {
            // Past deadline - send everything immediately
            pPacer->budgetBytes = pPacer->queueBytes;
        }
    }

    // Send packets while we have budget and packets in queue
    while (pPacer->pHead != NULL && pPacer->budgetBytes >= pPacer->pHead->size) {
        // Dequeue packet from head
        pPacket = pPacer->pHead;
        pPacer->pHead = pPacket->pNext;
        if (pPacer->pHead == NULL) {
            pPacer->pTail = NULL;
        }
        pPacer->queueSize--;
        pPacer->queueBytes -= pPacket->size;

        // Calculate queuing delay
        queueDelayKvs = now - pPacket->enqueueTimeKvs;

        // Update average queue delay (exponential moving average)
        if (pPacer->stats.avgQueueDelayKvs == 0) {
            pPacer->stats.avgQueueDelayKvs = queueDelayKvs;
        } else {
            pPacer->stats.avgQueueDelayKvs = (UINT64)(0.9 * pPacer->stats.avgQueueDelayKvs + 0.1 * queueDelayKvs);
        }

        // Send the packet
        retStatus = iceAgentSendPacket(pKvsPeerConnection->pIceAgent, pPacket->pData, pPacket->size);
        if (STATUS_SUCCEEDED(retStatus)) {
            UINT64 sentTimeKvs = GETTIME();

            bytesSent += pPacket->size;
            pPacer->stats.packetsSent++;
            pPacer->stats.bytesSent += pPacket->size;

            // Deduct from budget
            pPacer->budgetBytes -= pPacket->size;

            // Update TWCC manager with actual send time
            if (pPacket->twccSeqNum != 0 && pKvsPeerConnection->twccExtId != 0) {
                twccManagerOnPacedPacketSent(pKvsPeerConnection, pPacket->twccSeqNum, pPacket->size, sentTimeKvs);
            }
        } else {
            DLOGW("Failed to send paced packet: 0x%08x", retStatus);
        }

        // Free the packet (data is owned by packet)
        pacerFreePacket(pPacket);
        pPacket = NULL;
    }

    // Update queue stats
    pPacer->stats.currentQueueSize = pPacer->queueSize;
    pPacer->stats.currentQueueBytes = pPacer->queueBytes;

    // Cap budget to prevent accumulating too much credit
    // Max budget = 2x interval worth of bytes
    UINT32 maxBudget = pacerCalculateBudget(pPacer, 2 * PACER_INTERVAL_KVS);
    if (pPacer->budgetBytes > maxBudget) {
        pPacer->budgetBytes = maxBudget;
    }

CleanUp:
    LEAVES();
    return retStatus;
}

STATUS pacerTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    UNUSED_PARAM(currentTime);

    STATUS retStatus = STATUS_SUCCESS;
    PPacer pPacer = (PPacer) customData;

    CHK(pPacer != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pPacer->lock);

    // Only drain if enabled
    if (pPacer->enabled) {
        pacerDrainQueue(pPacer);
    }

    MUTEX_UNLOCK(pPacer->lock);

CleanUp:
    return retStatus;
}
