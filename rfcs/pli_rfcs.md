# PLI

The primary definition for the Picture Loss Indication (PLI) packet is found in RFC 4585.

In the context of WebRTC and RTP/RTCP, PLI is a mechanism used by the receiver to inform the sender that it has lost some encoded video data and cannot decode the current frame, effectively requesting a new keyframe (Intra-frame) to restore synchronization.

Modern FIR packet may include multiple SSRCs 

```
0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P| FMT=4   |    PT=206     |        Length = 6             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of Packet Sender                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             SSRC of Media Source (UNUSED = 0)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Target SSRC 1                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Seq Nr 1    |            Reserved (All 0)                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
...
|                       Target SSRC N                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Seq Nr N    |            Reserved (All 0)                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

## Primary RFC
[RFC 4585](rfc4585.txt)

RFC 4585: Extended RTP Profile for RTCP-Based Feedback (RTP/AVPF)

Section 6.3.1: This is the exact definition of the Picture Loss Indication (PLI) packet.

Packet Type (PT): 206 (Payload-Specific Feedback Message)

Feedback Message Type (FMT): 1

Definition: It specifies that PLI is used when the receiver has lost undefined amounts of data and requires a refresh (usually a keyframe) to restore the image.

## Closely Related RFCs
While RFC 4585 defines PLI, the following RFCs are critical for understanding how it is used in WebRTC and how it differs from similar commands.

### 1. RFC 5104: Codec Control Messages in the RTP/AVPF Profile

[RFC 5104](rfc5104.txt)

Relevance: Defines the Full Intra Request (FIR), which is frequently confused with PLI.

Distinction:

PLI (RFC 4585): Used when the decoder fails due to packet loss ("I lost data, help me recover").

FIR (RFC 5104): Used when the application layer needs a keyframe for non-loss reasons, such as a new user joining a conference ("I am new here, please send a full frame").

Note: In many WebRTC implementations, PLI and FIR trigger identical behavior (sending a keyframe), but they are semantically different.

### 2. RFC 8834: Media Transport and Use of RTP in WebRTC

[RFC 8834](rfc8834.txt)

Relevance: This RFC mandates that all WebRTC endpoints MUST implement the AVPF profile (RFC 4585) and specifically mentions the support for PLI to handle packet loss recovery in video streams.

Summary of Packet Structure (RFC 4585)
When you are looking at a packet capture (Wireshark) or implementing a parser, a PLI packet is identified by:

RTCP Packet Type: 206 (Payload-Specific Feedback)

FMT (Feedback Message Type): 1 (PLI)

SSRC of sender: The SSRC of the device requesting the keyframe.

SSRC of media source: The SSRC of the video stream that is broken.