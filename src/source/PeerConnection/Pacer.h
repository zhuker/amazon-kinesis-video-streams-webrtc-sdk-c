/*******************************************
Pacer - Smooth packet transmission for congestion control
Based on GCC RFC draft-ietf-rmcat-gcc-02 Section 4
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PACER__
#define __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PACER__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Pacing interval in milliseconds (5ms as recommended by GCC RFC)
#define PACER_INTERVAL_MS  5
#define PACER_INTERVAL_KVS (PACER_INTERVAL_MS * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)

// Default queue limits
#define PACER_DEFAULT_MAX_QUEUE_SIZE  500               // Maximum packets in queue
#define PACER_DEFAULT_MAX_QUEUE_BYTES (2 * 1024 * 1024) // 2MB max queue size

// Default pacing factor - allows sending at higher rate to clear queues faster
// libwebrtc uses 2.5x, which helps clear large I-frames quickly
#define PACER_DEFAULT_PACING_FACTOR 2.5

// Minimum bitrate to prevent divide-by-zero
#define PACER_MIN_BITRATE_BPS 10000 // 10 kbps minimum

/**
 * Packet info for batch enqueue (frame-based pacing)
 */
typedef struct {
    PBYTE pData;       //!< Packet data (pacer takes ownership)
    UINT32 size;       //!< Packet size in bytes
    UINT16 twccSeqNum; //!< TWCC sequence number
} PacerPacketInfo, *PPacerPacketInfo;

/**
 * Queued packet for paced sending
 */
typedef struct __PacerPacket {
    PBYTE pData;                 //!< Encrypted packet data (owned by pacer)
    UINT32 size;                 //!< Packet size in bytes
    UINT64 enqueueTimeKvs;       //!< When packet was enqueued
    UINT16 twccSeqNum;           //!< TWCC sequence number for tracking
    struct __PacerPacket* pNext; //!< Next packet in queue
} PacerPacket, *PPacerPacket;

/**
 * Pacer configuration
 */
typedef struct {
    UINT64 initialBitrateBps; //!< Initial target bitrate (default: 300 kbps)
    UINT32 maxQueueSize;      //!< Maximum packets in queue (default: 500)
    UINT32 maxQueueBytes;     //!< Maximum bytes in queue (default: 2MB)
    DOUBLE pacingFactor;      //!< Pacing factor multiplier (default: 2.5, like libwebrtc)
    UINT64 maxQueueTimeKvs;   //!< Max time packet can wait in queue in 100ns units (0=disabled)
    BOOL enabled;             //!< Enable pacing (default: TRUE)
} PacerConfig, *PPacerConfig;

/**
 * Pacer statistics
 */
typedef struct {
    UINT64 packetsSent;         //!< Total packets sent through pacer
    UINT64 bytesSent;           //!< Total bytes sent
    UINT64 packetsDropped;      //!< Packets dropped due to queue overflow
    UINT64 bytesDropped;        //!< Bytes dropped due to queue overflow
    UINT64 currentQueueSize;    //!< Current number of packets in queue
    UINT64 currentQueueBytes;   //!< Current bytes in queue
    UINT64 maxQueueSizeReached; //!< Maximum queue size reached
    UINT64 avgQueueDelayKvs;    //!< Average queuing delay (100ns units)
} PacerStats, *PPacerStats;

/**
 * Pacer structure
 */
typedef struct __Pacer {
    MUTEX lock;   //!< Thread safety
    BOOL enabled; //!< Is pacing enabled?

    // Queue management
    PPacerPacket pHead;   //!< Head of packet queue
    PPacerPacket pTail;   //!< Tail of packet queue
    UINT32 queueSize;     //!< Number of packets in queue
    UINT32 queueBytes;    //!< Total bytes in queue
    UINT32 maxQueueSize;  //!< Maximum packets allowed
    UINT32 maxQueueBytes; //!< Maximum bytes allowed

    // Rate control
    UINT64 targetBitrateBps; //!< Target bitrate from GCC
    UINT64 budgetBytes;      //!< Bytes allowed to send this interval
    UINT64 lastSendTimeKvs;  //!< Time of last packet send
    DOUBLE pacingFactor;     //!< Pacing factor multiplier (e.g., 2.5x)
    UINT64 maxQueueTimeKvs;  //!< Max queue time for frame-rate pacing in 100ns units (0=disabled)

    // Timer
    TIMER_QUEUE_HANDLE timerQueueHandle; //!< Timer queue handle
    UINT32 timerId;                      //!< Pacer timer ID

    // Parent peer connection (for sending) - stored as PVOID to avoid circular dependency
    PVOID pKvsPeerConnection;

    // Statistics
    PacerStats stats;
} Pacer, *PPacer;

//
// Public API
//

/**
 * Create a new pacer
 *
 * @param[out] ppPacer Pointer to receive the new pacer
 * @param[in] timerQueueHandle Timer queue for scheduling
 * @param[in] pConfig Optional configuration (NULL for defaults)
 * @return STATUS code
 */
STATUS createPacer(PPacer* ppPacer, TIMER_QUEUE_HANDLE timerQueueHandle, PPacerConfig pConfig);

/**
 * Free a pacer and all queued packets
 *
 * @param[in,out] ppPacer Pointer to pacer to free
 * @return STATUS code
 */
STATUS freePacer(PPacer* ppPacer);

/**
 * Start the pacer timer
 *
 * @param[in] pPacer Pacer instance
 * @param[in] pKvsPeerConnection Parent peer connection for sending (PKvsPeerConnection)
 * @return STATUS code
 */
STATUS pacerStart(PPacer pPacer, PVOID pKvsPeerConnection);

/**
 * Stop the pacer timer
 *
 * @param[in] pPacer Pacer instance
 * @return STATUS code
 */
STATUS pacerStop(PPacer pPacer);

/**
 * Enqueue a packet for paced sending
 * The pacer takes ownership of the packet data
 *
 * @param[in] pPacer Pacer instance
 * @param[in] pData Packet data (pacer takes ownership, will free)
 * @param[in] size Packet size in bytes
 * @param[in] twccSeqNum TWCC sequence number for the packet
 * @return STATUS code (STATUS_SUCCESS or STATUS_NOT_ENOUGH_MEMORY if queue full)
 */
STATUS pacerEnqueuePacket(PPacer pPacer, PBYTE pData, UINT32 size, UINT16 twccSeqNum);

/**
 * Enqueue multiple packets as a frame (batch enqueue with single lock)
 * All packets share the same enqueue timestamp for frame-deadline pacing.
 * The pacer takes ownership of all packet data.
 *
 * @param[in] pPacer Pacer instance
 * @param[in] pPackets Array of packet info structures
 * @param[in] count Number of packets in array
 * @return STATUS code (STATUS_SUCCESS or STATUS_NOT_ENOUGH_MEMORY if queue full)
 */
STATUS pacerEnqueueFrame(PPacer pPacer, PPacerPacketInfo pPackets, UINT32 count);

/**
 * Set the target bitrate for pacing
 *
 * @param[in] pPacer Pacer instance
 * @param[in] bitrateBps Target bitrate in bits per second
 * @return STATUS code
 */
STATUS pacerSetTargetBitrate(PPacer pPacer, UINT64 bitrateBps);

/**
 * Get current target bitrate
 *
 * @param[in] pPacer Pacer instance
 * @return Current target bitrate in bps
 */
UINT64 pacerGetTargetBitrate(PPacer pPacer);

/**
 * Get pacer statistics
 *
 * @param[in] pPacer Pacer instance
 * @param[out] pStats Statistics structure to fill
 * @return STATUS code
 */
STATUS pacerGetStats(PPacer pPacer, PPacerStats pStats);

/**
 * Check if pacing is enabled
 *
 * @param[in] pPacer Pacer instance
 * @return TRUE if enabled, FALSE otherwise
 */
BOOL pacerIsEnabled(PPacer pPacer);

/**
 * Enable or disable pacing
 *
 * @param[in] pPacer Pacer instance
 * @param[in] enabled TRUE to enable, FALSE to disable
 * @return STATUS code
 */
STATUS pacerSetEnabled(PPacer pPacer, BOOL enabled);

/**
 * Get current queue depth
 *
 * @param[in] pPacer Pacer instance
 * @return Number of packets currently queued
 */
UINT32 pacerGetQueueSize(PPacer pPacer);

/**
 * Set max queue time for frame-rate pacing
 *
 * @param[in] pPacer Pacer instance
 * @param[in] maxQueueTimeKvs Max time in 100ns units (0 to disable)
 * @return STATUS code
 */
STATUS pacerSetMaxQueueTime(PPacer pPacer, UINT64 maxQueueTimeKvs);

/**
 * Get current max queue time
 *
 * @param[in] pPacer Pacer instance
 * @return Current max queue time in 100ns units (0 if disabled)
 */
UINT64 pacerGetMaxQueueTime(PPacer pPacer);

//
// Internal functions (exposed for unit testing)
//

/**
 * Timer callback for paced sending
 */
STATUS pacerTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData);

/**
 * Send packets up to the current budget
 */
STATUS pacerDrainQueue(PPacer pPacer);

/**
 * Calculate bytes allowed to send based on bitrate and elapsed time
 */
UINT32 pacerCalculateBudget(PPacer pPacer, UINT64 elapsedTimeKvs);

#ifdef __cplusplus
}
#endif
#endif /* __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PACER__ */
