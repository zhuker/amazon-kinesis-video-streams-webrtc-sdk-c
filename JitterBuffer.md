# Jitter Buffer Documentation

## Overview

The jitter buffer is responsible for reassembling RTP packets into complete video frames. It handles:
- Packet reordering (packets arriving out of sequence)
- Packet loss detection and frame dropping
- Latency management via configurable max latency
- Frame boundary detection via RTP timestamps

## Key Components

### Data Structures

**JitterBuffer struct** ([JitterBuffer.h:20-45](src/source/PeerConnection/JitterBuffer.h#L20-L45)):
- `headSequenceNumber` / `tailSequenceNumber`: Current buffer range
- `headTimestamp` / `tailTimestamp`: RTP timestamp range
- `maxLatency`: Maximum allowed delay before forcing frame drops (in clock rate units)
- `pPkgBufferHashTable`: Hash table storing RTP packets by sequence number

### Callbacks

- `onFrameReadyFn`: Called when a complete frame is ready for decoding
- `onFrameDroppedFn`: Called when an incomplete frame must be dropped
- `depayPayloadFn`: Codec-specific depayloading (e.g., H264, VP8)

## Frame Delivery Conditions

A frame is delivered (`onFrameReadyFn` called) when ALL of these conditions are met:

1. **Has start packet**: The frame's first packet (identified by depayloader's `isStart` flag) is present
2. **Is contiguous**: All packets from start to end have no gaps in sequence numbers
3. **Frame boundary detected**: Either:
   - Marker bit is set on a packet (immediate delivery), OR
   - A packet with a different RTP timestamp arrives (new frame started)

## Frame Drop Conditions

A frame is dropped (`onFrameDroppedFn` called) when:

1. **Missing start packet**: Frame has packets but the first packet is missing
2. **Not contiguous**: Gaps exist in the sequence number range
3. **Latency exceeded**: `headTimestamp < tailTimestamp - maxLatency`
4. **Buffer closing**: `freeJitterBuffer()` called with packets remaining

## Latency Optimization

### Marker Bit Immediate Delivery

**Location**: [JitterBuffer.c:665-683](src/source/PeerConnection/JitterBuffer.c#L665-L683)

The jitter buffer delivers frames immediately when the marker bit is detected, rather than waiting for the next frame's first packet. This reduces latency by one frame interval (typically 33ms at 30fps).

**Delivery conditions for marker bit path**:
```c
if (curTimestamp == pJitterBuffer->headTimestamp &&  // Packet is from head frame
    pCurPacket->header.marker &&                     // Has marker bit
    containStartForEarliestFrame &&                  // Has start packet
    headFrameIsContiguous) {                         // No gaps in frame
    // Deliver immediately
}
```

### Per-Frame Continuity Tracking

**Problem solved**: Previously, a gap in frame N would incorrectly block delivery of intact frames N+1, N+2, etc.

**Solution**: Track continuity per-frame using `headFrameIsContiguous` instead of the global `isFrameDataContinuous`.

**Key variables** ([JitterBuffer.c:499-511](src/source/PeerConnection/JitterBuffer.c#L499-L511)):
- `headFrameIsContiguous`: TRUE if current head frame has no gaps
- `sawGapSinceLastFrame`: Whether any gap was detected since last frame boundary
- `firstGapIndex`: Sequence number where first gap was found
- `lastHeadFrameSeqNum`: Last sequence number belonging to head frame
- `headFrameEnded`: TRUE if head frame's marker bit was seen

**Gap evaluation at frame boundary** ([JitterBuffer.c:584-597](src/source/PeerConnection/JitterBuffer.c#L584-L597)):
```c
if (sawGapSinceLastFrame && seenHeadFramePacket) {
    if (firstGapIndex <= lastHeadFrameSeqNum) {
        // Gap is within head frame's known packet range
        headFrameIsContiguous = FALSE;
    } else if (!headFrameEnded) {
        // Gap is after last seen head packet, but frame not complete
        headFrameIsContiguous = FALSE;
    }
    // else: gap is AFTER head frame's marker bit, so it's in next frame
}
```

### Buffer Flush on Close

**Location**: [JitterBuffer.c:77-99](src/source/PeerConnection/JitterBuffer.c#L77-L99)

When `freeJitterBuffer()` is called, it processes all remaining frames:
```c
while (maxIterations-- > 0) {
    jitterBufferInternalParse(pJitterBuffer, TRUE);  // bufferClosed=TRUE
    if (pJitterBuffer->headSequenceNumber == prevHead) {
        break;  // No progress made
    }
    // Check if all frames processed
    INT32 remaining = (INT32)((UINT16)(pJitterBuffer->tailSequenceNumber -
                               pJitterBuffer->headSequenceNumber + 1));
    if (remaining <= 0) {
        break;
    }
    prevHead = pJitterBuffer->headSequenceNumber;
}
```

This ensures all complete frames are delivered even when the buffer is being closed, rather than dropping everything.

## Packet Loss Behavior

### Random Packet Loss

- **Single packet loss**: Frame containing the lost packet is dropped
- **Subsequent frames**: Delivered normally if intact (per-frame tracking)
- **Recovery**: Automatic after dropped frame's packets cleared

### Burst Packet Loss

- **Within single frame**: Frame dropped, next frame delivered if intact
- **Spanning multiple frames**: Multiple frames dropped, first intact frame delivered

### Latency-Driven Drops

When `headTimestamp < earliestAllowedTimestamp`:
1. Incomplete frames at head are dropped
2. Processing continues to find next deliverable frame
3. Gap detection continues for remaining frames

## Sequence Number and Timestamp Overflow

The jitter buffer handles wraparound for both:

**Sequence number overflow** ([JitterBuffer.c:219-263](src/source/PeerConnection/JitterBuffer.c#L219-L263)):
- Detects when sequence numbers wrap from 65535 to 0
- Uses `sequenceNumberOverflowState` flag
- `MAX_OUT_OF_ORDER_PACKET_DIFFERENCE` (512) limits reordering window

**Timestamp overflow** ([JitterBuffer.c:265-303](src/source/PeerConnection/JitterBuffer.c#L265-L303)):
- Detects when RTP timestamps wrap from MAX_UINT32 to 0
- Uses `timestampOverFlowState` flag
- Correlates with sequence number to avoid false detection

## API

### createJitterBuffer

```c
STATUS createJitterBuffer(
    FrameReadyFunc onFrameReadyFunc,     // Callback for complete frames
    FrameDroppedFunc onFrameDroppedFunc, // Callback for dropped frames
    DepayRtpPayloadFunc depayRtpPayloadFunc, // Codec depayloader
    UINT32 maxLatency,                   // Max latency in 100ns units (0 = default)
    UINT32 clockRate,                    // RTP clock rate (e.g., 90000 for video)
    UINT64 customData,                   // User data passed to callbacks
    PJitterBuffer* ppJitterBuffer        // Output: created buffer
);
```

### jitterBufferPush

```c
STATUS jitterBufferPush(
    PJitterBuffer pJitterBuffer,  // Jitter buffer instance
    PRtpPacket pRtpPacket,        // RTP packet (ownership transferred)
    PBOOL pPacketDiscarded        // Output: TRUE if packet outside latency window
);
```

### freeJitterBuffer

```c
STATUS freeJitterBuffer(PJitterBuffer* ppJitterBuffer);
```

Processes all remaining frames (delivering complete ones, dropping incomplete ones) before freeing resources.

## Testing

### Unit Tests

Run from `build/` directory:
```bash
./tst/webrtc_client_test --gtest_filter="JitterBufferFunctionalityTest.*"
```

Key tests:
- `continousPacketsComeOutOfOrder`: Verifies reordering tolerance
- `markerBitTriggersImmediateDelivery`: Verifies low-latency delivery
- `intactFrameAfterDroppedFrame`: Verifies per-frame continuity tracking

### Integration Tests

```bash
./tst/webrtc_client_test --gtest_filter="H264JitterBufferIntegrationTest.*"
```

Tests various packet loss patterns:
- **Random loss**: 1%, 5% uniform random loss
- **Burst loss**: Consecutive packet drops
- **Periodic loss**: Every Nth packet dropped
- **Gilbert-Elliott**: Markov model with good/bad states
- **Reordering**: Packets arriving out of sequence

### Frame Accounting

For integration tests, the following invariant holds:
```
framesReceived + framesDropped = totalFrames - framesFullyDropped
```

Where:
- `framesReceived`: Complete frames delivered via `onFrameReadyFn`
- `framesDropped`: Incomplete frames dropped via `onFrameDroppedFn`
- `framesFullyDropped`: Frames where ALL packets were lost (no callback)

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Hash table buckets | 3000 |
| Hash table bucket length | 2 |
| Max out-of-order distance | 512 packets |
| Default max latency | Configurable (DEFAULT_JITTER_BUFFER_MAX_LATENCY) |

## Design Decisions

1. **Hash table for packet storage**: O(1) lookup by sequence number
2. **Per-frame continuity**: Prevents cascade drops after single frame loss
3. **Marker bit delivery**: Reduces latency without sacrificing reliability
4. **Flush loop on close**: Ensures all deliverable frames are delivered
5. **Sequence number overflow handling**: Supports long-running streams
