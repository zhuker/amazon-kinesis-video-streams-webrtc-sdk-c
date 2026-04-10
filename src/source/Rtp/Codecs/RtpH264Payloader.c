#define LOG_CLASS "RtpH264Payloader"

#include "../../Include_i.h"

// Maximum number of NAL units we can track for STAP-A aggregation per frame
#define MAX_NALUS_PER_FRAME 64

// Structure to track NAL unit info for aggregation decisions
typedef struct {
    PBYTE pNalu;
    UINT32 length;
} NaluInfo;

// Helper function to calculate STAP-A size overhead
// STAP-A format: 1 byte header + (2 bytes size + nalu data) for each NAL
#define STAP_A_NALU_OVERHEAD 2 // 2 bytes for size field per NAL

// Read the first unsigned Exp-Golomb ue(v) code from an H.264 RBSP bitstream.
// pData points at the NALU byte *after* the 1-byte NALU header (i.e. start of the
// slice_header RBSP). nBytes is the number of bytes available. Handles the
// emulation-prevention byte (0x03 after two zero bytes) inline.
// Returns TRUE on success and writes the decoded value to *pValue.
static BOOL h264ReadUeGolomb(PBYTE pData, UINT32 nBytes, PUINT32 pValue)
{
    UINT32 bitPos = 0;
    UINT32 zeroBits = 0;
    UINT32 value = 0;
    UINT32 i;
    // Cap scan length — slice header's first_mb_in_slice is tiny; 8 bytes is plenty
    // to cover any realistic leading zero run and the value bits. This also bounds work.
    UINT32 maxBits = (nBytes < 8 ? nBytes : 8) * 8;

    // Count leading zero bits
    while (bitPos < maxBits) {
        UINT32 byteIdx = bitPos >> 3;
        // Skip emulation-prevention byte: when we see 00 00 03, the 03 must be dropped.
        if (byteIdx >= 2 && pData[byteIdx] == 0x03 && pData[byteIdx - 1] == 0x00 && pData[byteIdx - 2] == 0x00) {
            bitPos += 8;
            continue;
        }
        if ((pData[byteIdx] >> (7 - (bitPos & 7))) & 1) {
            break;
        }
        zeroBits++;
        bitPos++;
    }
    if (bitPos >= maxBits || zeroBits >= 32) {
        return FALSE;
    }
    bitPos++; // consume the terminating 1 bit
    // Read `zeroBits` value bits
    if (bitPos + zeroBits > maxBits) {
        return FALSE;
    }
    for (i = 0; i < zeroBits; i++) {
        UINT32 byteIdx = bitPos >> 3;
        if (byteIdx >= 2 && pData[byteIdx] == 0x03 && pData[byteIdx - 1] == 0x00 && pData[byteIdx - 2] == 0x00) {
            bitPos += 8;
            if (bitPos + (zeroBits - i) > maxBits) {
                return FALSE;
            }
            byteIdx = bitPos >> 3;
        }
        value = (value << 1) | ((pData[byteIdx] >> (7 - (bitPos & 7))) & 1);
        bitPos++;
    }
    *pValue = (1u << zeroBits) - 1 + value;
    return TRUE;
}

// Decide whether a single H.264 NALU marks the start of a new access unit (frame).
// pNalu points to the NALU starting with its 1-byte header; naluLen is the total NALU length.
// For VCL slices (types 1 and 5), returns TRUE only if slice_header.first_mb_in_slice == 0.
// For SPS/PPS/SEI/AUD, returns TRUE unconditionally (they precede a new picture).
static BOOL h264NaluIsFrameStart(PBYTE pNalu, UINT32 naluLen)
{
    UINT8 naluType;
    UINT32 firstMb = 0;

    if (pNalu == NULL || naluLen == 0) {
        return FALSE;
    }
    naluType = *pNalu & NAL_TYPE_MASK;
    switch (naluType) {
        case H264_NALU_TYPE_SPS:
        case H264_NALU_TYPE_PPS:
        case H264_NALU_TYPE_SEI:
        case H264_NALU_TYPE_AUD:
            return TRUE;
        case H264_NALU_TYPE_SLICE:
        case H264_NALU_TYPE_IDR:
            // slice_header starts right after the 1-byte NALU header.
            // first_mb_in_slice is the first ue(v) code in the RBSP.
            if (naluLen < 2) {
                return FALSE;
            }
            if (!h264ReadUeGolomb(pNalu + 1, naluLen - 1, &firstMb)) {
                return FALSE;
            }
            return firstMb == 0;
        default:
            return FALSE;
    }
}

// Helper function to create a STAP-A packet from multiple NAL units
static STATUS createStapAPayload(NaluInfo* pNaluInfos, UINT32 naluCount, PPayloadArray pPayloadArray, PUINT32 pFilledLength,
                                 PUINT32 pFilledSubLenSize)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 i;
    UINT32 totalSize = STAP_A_HEADER_SIZE;
    UINT8 maxNri = 0;
    UINT8 stapHeader;
    PBYTE pPayload = NULL;
    BOOL sizeCalculationOnly = (pPayloadArray == NULL);

    CHK(pNaluInfos != NULL && naluCount > 0 && pFilledLength != NULL && pFilledSubLenSize != NULL, STATUS_NULL_ARG);

    // Calculate total size and max NRI
    for (i = 0; i < naluCount; i++) {
        totalSize += STAP_A_NALU_OVERHEAD + pNaluInfos[i].length;
        UINT8 nri = pNaluInfos[i].pNalu[0] & 0x60;
        if (nri > maxNri) {
            maxNri = nri;
        }
    }

    *pFilledLength = totalSize;
    *pFilledSubLenSize = 1; // STAP-A is always one RTP packet

    CHK(!sizeCalculationOnly, retStatus);
    CHK(pPayloadArray->payloadSubLength != NULL && pPayloadArray->payloadBuffer != NULL, STATUS_NULL_ARG);
    CHK(1 <= pPayloadArray->maxPayloadSubLenSize && totalSize <= pPayloadArray->maxPayloadLength, STATUS_BUFFER_TOO_SMALL);

    pPayload = pPayloadArray->payloadBuffer;

    // Write STAP-A header: type (24) | NRI
    stapHeader = STAP_A_INDICATOR | maxNri;
    *pPayload++ = stapHeader;

    // Write each NAL unit with its 2-byte size prefix
    for (i = 0; i < naluCount; i++) {
        // Write 2-byte size in big-endian
        putUnalignedInt16BigEndian(pPayload, (UINT16) pNaluInfos[i].length);
        pPayload += SIZEOF(UINT16);

        // Write NAL unit data
        MEMCPY(pPayload, pNaluInfos[i].pNalu, pNaluInfos[i].length);
        pPayload += pNaluInfos[i].length;
    }

    pPayloadArray->payloadSubLength[0] = totalSize;

CleanUp:
    return retStatus;
}

STATUS createPayloadForH264(UINT32 mtu, PBYTE nalus, UINT32 nalusLength, PBYTE payloadBuffer, PUINT32 pPayloadLength, PUINT32 pPayloadSubLength,
                            PUINT32 pPayloadSubLenSize)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PBYTE curPtrInNalus = nalus;
    UINT32 remainNalusLength = nalusLength;
    UINT32 nextNaluLength = 0;
    UINT32 startIndex = 0;
    UINT32 singlePayloadLength = 0;
    UINT32 singlePayloadSubLenSize = 0;
    BOOL sizeCalculationOnly = (payloadBuffer == NULL);
    PayloadArray payloadArray;

    // Arrays to collect NAL units for aggregation
    NaluInfo naluInfos[MAX_NALUS_PER_FRAME];
    UINT32 naluCount = 0;
    UINT32 i;

    // Variables for STAP-A aggregation tracking
    UINT32 stapStartIndex = 0;
    UINT32 stapNaluCount = 0;
    UINT32 currentStapSize = STAP_A_HEADER_SIZE;

    CHK(nalus != NULL && pPayloadSubLenSize != NULL && pPayloadLength != NULL && (sizeCalculationOnly || pPayloadSubLength != NULL), STATUS_NULL_ARG);
    CHK(mtu > FU_A_HEADER_SIZE, STATUS_RTP_INPUT_MTU_TOO_SMALL);

    if (sizeCalculationOnly) {
        payloadArray.payloadLength = 0;
        payloadArray.payloadSubLenSize = 0;
        payloadArray.maxPayloadLength = 0;
        payloadArray.maxPayloadSubLenSize = 0;
    } else {
        payloadArray.payloadLength = *pPayloadLength;
        payloadArray.payloadSubLenSize = *pPayloadSubLenSize;
        payloadArray.maxPayloadLength = *pPayloadLength;
        payloadArray.maxPayloadSubLenSize = *pPayloadSubLenSize;
    }
    payloadArray.payloadBuffer = payloadBuffer;
    payloadArray.payloadSubLength = pPayloadSubLength;

    // First pass: collect all NAL units
    while (remainNalusLength > 0 && naluCount < MAX_NALUS_PER_FRAME) {
        CHK_STATUS(getNextNaluLength(curPtrInNalus, remainNalusLength, &startIndex, &nextNaluLength));

        curPtrInNalus += startIndex;
        remainNalusLength -= startIndex;

        if (remainNalusLength == 0) {
            break;
        }

        naluInfos[naluCount].pNalu = curPtrInNalus;
        naluInfos[naluCount].length = nextNaluLength;
        naluCount++;

        remainNalusLength -= nextNaluLength;
        curPtrInNalus += nextNaluLength;
    }

    // Check if we exceeded the limit
    if (remainNalusLength > 0 && naluCount >= MAX_NALUS_PER_FRAME) {
        DLOGE("Frame has more than %u NAL units, cannot process", MAX_NALUS_PER_FRAME);
        CHK(FALSE, STATUS_INVALID_ARG);
    }

    CHK(naluCount > 0, retStatus);

    // Second pass: decide aggregation strategy and create payloads
    stapStartIndex = 0;
    stapNaluCount = 0;
    currentStapSize = STAP_A_HEADER_SIZE;

    for (i = 0; i < naluCount; i++) {
        UINT32 naluSize = naluInfos[i].length;
        UINT32 stapNaluSize = STAP_A_NALU_OVERHEAD + naluSize; // size with 2-byte length prefix

        if (naluSize > mtu) {
            // NAL unit too large - flush any pending STAP-A and use FU-A

            // Flush pending STAP-A aggregation
            if (stapNaluCount > 1) {
                // Create STAP-A packet
                if (sizeCalculationOnly) {
                    CHK_STATUS(createStapAPayload(&naluInfos[stapStartIndex], stapNaluCount, NULL, &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadLength += singlePayloadLength;
                    payloadArray.payloadSubLenSize += singlePayloadSubLenSize;
                } else {
                    CHK_STATUS(
                        createStapAPayload(&naluInfos[stapStartIndex], stapNaluCount, &payloadArray, &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadBuffer += singlePayloadLength;
                    payloadArray.payloadSubLength += singlePayloadSubLenSize;
                    payloadArray.maxPayloadLength -= singlePayloadLength;
                    payloadArray.maxPayloadSubLenSize -= singlePayloadSubLenSize;
                }
            } else if (stapNaluCount == 1) {
                // Only one NAL - send as single NAL packet
                if (sizeCalculationOnly) {
                    CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[stapStartIndex].pNalu, naluInfos[stapStartIndex].length, NULL,
                                                     &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadLength += singlePayloadLength;
                    payloadArray.payloadSubLenSize += singlePayloadSubLenSize;
                } else {
                    CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[stapStartIndex].pNalu, naluInfos[stapStartIndex].length, &payloadArray,
                                                     &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadBuffer += singlePayloadLength;
                    payloadArray.payloadSubLength += singlePayloadSubLenSize;
                    payloadArray.maxPayloadLength -= singlePayloadLength;
                    payloadArray.maxPayloadSubLenSize -= singlePayloadSubLenSize;
                }
            }

            // Reset STAP tracking
            stapStartIndex = i + 1;
            stapNaluCount = 0;
            currentStapSize = STAP_A_HEADER_SIZE;

            // Use FU-A for this large NAL
            if (sizeCalculationOnly) {
                CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[i].pNalu, naluInfos[i].length, NULL, &singlePayloadLength, &singlePayloadSubLenSize));
                payloadArray.payloadLength += singlePayloadLength;
                payloadArray.payloadSubLenSize += singlePayloadSubLenSize;
            } else {
                CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[i].pNalu, naluInfos[i].length, &payloadArray, &singlePayloadLength,
                                                 &singlePayloadSubLenSize));
                payloadArray.payloadBuffer += singlePayloadLength;
                payloadArray.payloadSubLength += singlePayloadSubLenSize;
                payloadArray.maxPayloadLength -= singlePayloadLength;
                payloadArray.maxPayloadSubLenSize -= singlePayloadSubLenSize;
            }

        } else if (currentStapSize + stapNaluSize <= mtu) {
            // NAL fits in current STAP-A - add it
            if (stapNaluCount == 0) {
                stapStartIndex = i;
            }
            stapNaluCount++;
            currentStapSize += stapNaluSize;

        } else {
            // NAL doesn't fit in current STAP-A - flush and start new

            // Flush current STAP-A
            if (stapNaluCount > 1) {
                // Create STAP-A packet
                if (sizeCalculationOnly) {
                    CHK_STATUS(createStapAPayload(&naluInfos[stapStartIndex], stapNaluCount, NULL, &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadLength += singlePayloadLength;
                    payloadArray.payloadSubLenSize += singlePayloadSubLenSize;
                } else {
                    CHK_STATUS(
                        createStapAPayload(&naluInfos[stapStartIndex], stapNaluCount, &payloadArray, &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadBuffer += singlePayloadLength;
                    payloadArray.payloadSubLength += singlePayloadSubLenSize;
                    payloadArray.maxPayloadLength -= singlePayloadLength;
                    payloadArray.maxPayloadSubLenSize -= singlePayloadSubLenSize;
                }
            } else if (stapNaluCount == 1) {
                // Only one NAL - send as single NAL packet
                if (sizeCalculationOnly) {
                    CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[stapStartIndex].pNalu, naluInfos[stapStartIndex].length, NULL,
                                                     &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadLength += singlePayloadLength;
                    payloadArray.payloadSubLenSize += singlePayloadSubLenSize;
                } else {
                    CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[stapStartIndex].pNalu, naluInfos[stapStartIndex].length, &payloadArray,
                                                     &singlePayloadLength, &singlePayloadSubLenSize));
                    payloadArray.payloadBuffer += singlePayloadLength;
                    payloadArray.payloadSubLength += singlePayloadSubLenSize;
                    payloadArray.maxPayloadLength -= singlePayloadLength;
                    payloadArray.maxPayloadSubLenSize -= singlePayloadSubLenSize;
                }
            }

            // Start new STAP-A with current NAL
            stapStartIndex = i;
            stapNaluCount = 1;
            currentStapSize = STAP_A_HEADER_SIZE + stapNaluSize;
        }
    }

    // Flush any remaining NALs
    if (stapNaluCount > 1) {
        // Create STAP-A packet for remaining NALs
        if (sizeCalculationOnly) {
            CHK_STATUS(createStapAPayload(&naluInfos[stapStartIndex], stapNaluCount, NULL, &singlePayloadLength, &singlePayloadSubLenSize));
            payloadArray.payloadLength += singlePayloadLength;
            payloadArray.payloadSubLenSize += singlePayloadSubLenSize;
        } else {
            CHK_STATUS(createStapAPayload(&naluInfos[stapStartIndex], stapNaluCount, &payloadArray, &singlePayloadLength, &singlePayloadSubLenSize));
            payloadArray.payloadBuffer += singlePayloadLength;
            payloadArray.payloadSubLength += singlePayloadSubLenSize;
            payloadArray.maxPayloadLength -= singlePayloadLength;
            payloadArray.maxPayloadSubLenSize -= singlePayloadSubLenSize;
        }
    } else if (stapNaluCount == 1) {
        // Single remaining NAL - send as single NAL packet
        if (sizeCalculationOnly) {
            CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[stapStartIndex].pNalu, naluInfos[stapStartIndex].length, NULL, &singlePayloadLength,
                                             &singlePayloadSubLenSize));
            payloadArray.payloadLength += singlePayloadLength;
            payloadArray.payloadSubLenSize += singlePayloadSubLenSize;
        } else {
            CHK_STATUS(createPayloadFromNalu(mtu, naluInfos[stapStartIndex].pNalu, naluInfos[stapStartIndex].length, &payloadArray,
                                             &singlePayloadLength, &singlePayloadSubLenSize));
            payloadArray.payloadBuffer += singlePayloadLength;
            payloadArray.payloadSubLength += singlePayloadSubLenSize;
            payloadArray.maxPayloadLength -= singlePayloadLength;
            payloadArray.maxPayloadSubLenSize -= singlePayloadSubLenSize;
        }
    }

CleanUp:
    if (STATUS_FAILED(retStatus) && sizeCalculationOnly) {
        payloadArray.payloadLength = 0;
        payloadArray.payloadSubLenSize = 0;
    }

    if (pPayloadSubLenSize != NULL && pPayloadLength != NULL) {
        *pPayloadLength = payloadArray.payloadLength;
        *pPayloadSubLenSize = payloadArray.payloadSubLenSize;
    }

    LEAVES();
    return retStatus;
}

STATUS getNextNaluLength(PBYTE nalus, UINT32 nalusLength, PUINT32 pStart, PUINT32 pNaluLength)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    UINT32 zeroCount = 0, offset = 0;
    BOOL naluFound = FALSE;
    PBYTE pCurrent = NULL;

    CHK(nalus != NULL && pStart != NULL && pNaluLength != NULL, STATUS_NULL_ARG);

    // Annex-B Nalu will have 0x000000001 or 0x000001 start code, at most 4 bytes
    while (offset < 4 && offset < nalusLength && nalus[offset] == 0) {
        offset++;
    }

    CHK(offset < nalusLength && offset < 4 && offset >= 2 && nalus[offset] == 1, STATUS_RTP_INVALID_NALU);
    *pStart = ++offset;
    pCurrent = nalus + offset;

    /* Not doing validation on number of consecutive zeros being less than 4 because some device can produce
     * data with trailing zeros. */
    while (offset < nalusLength) {
        if (*pCurrent == 0) {
            /* Maybe next byte is 1 */
            offset++;
            pCurrent++;

        } else if (*pCurrent == 1) {
            if (*(pCurrent - 1) == 0 && *(pCurrent - 2) == 0) {
                zeroCount = *(pCurrent - 3) == 0 ? 3 : 2;
                naluFound = TRUE;
                break;
            }

            /* The jump is always 3 because of the 1 previously matched.
             * All the 0's must be after this '1' matched at offset */
            offset += 3;
            pCurrent += 3;
        } else {
            /* Can jump 3 bytes forward */
            offset += 3;
            pCurrent += 3;
        }
    }
    *pNaluLength = MIN(offset, nalusLength) - *pStart - (naluFound ? zeroCount : 0);

CleanUp:

    // As we might hit error often in a "bad" frame scenario, we can't use CHK_LOG_ERR as it will be too frequent
    if (STATUS_FAILED(retStatus)) {
        DLOGD("Warning: Failed to get the next NALu in H264 payload with 0x%08x", retStatus);
    }

    LEAVES();
    return retStatus;
}

STATUS createPayloadFromNalu(UINT32 mtu, PBYTE nalu, UINT32 naluLength, PPayloadArray pPayloadArray, PUINT32 filledLength, PUINT32 filledSubLenSize)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PBYTE pPayload = NULL;
    UINT8 naluType = 0;
    UINT8 naluRefIdc = 0;
    UINT32 maxPayloadSize = 0;
    UINT32 curPayloadSize = 0;
    UINT32 remainingNaluLength = naluLength;
    UINT32 payloadLength = 0;
    UINT32 payloadSubLenSize = 0;
    PBYTE pCurPtrInNalu = NULL;
    BOOL sizeCalculationOnly = (pPayloadArray == NULL);

    CHK(nalu != NULL && filledLength != NULL && filledSubLenSize != NULL, STATUS_NULL_ARG);
    CHK(sizeCalculationOnly || (pPayloadArray->payloadSubLength != NULL && pPayloadArray->payloadBuffer != NULL), STATUS_NULL_ARG);
    CHK(mtu > FU_A_HEADER_SIZE, STATUS_RTP_INPUT_MTU_TOO_SMALL);

    // https://yumichan.net/video-processing/video-compression/introduction-to-h264-nal-unit/
    naluType = *nalu & 0x1F;   // last 5 bits
    naluRefIdc = *nalu & 0x60; // 2 bits higher than naluType

    if (!sizeCalculationOnly) {
        pPayload = pPayloadArray->payloadBuffer;
    }

    if (naluLength <= mtu) {
        payloadLength += naluLength;
        payloadSubLenSize++;

        if (!sizeCalculationOnly) {
            CHK(payloadSubLenSize <= pPayloadArray->maxPayloadSubLenSize && payloadLength <= pPayloadArray->maxPayloadLength,
                STATUS_BUFFER_TOO_SMALL);

            // Single NALU https://tools.ietf.org/html/rfc6184#section-5.6
            MEMCPY(pPayload, nalu, naluLength);
            pPayloadArray->payloadSubLength[payloadSubLenSize - 1] = naluLength;
            pPayload += pPayloadArray->payloadSubLength[payloadSubLenSize - 1];
        }
    } else {
        // FU-A https://tools.ietf.org/html/rfc6184#section-5.8
        maxPayloadSize = mtu - FU_A_HEADER_SIZE;

        // According to the RFC, the first octet is skipped due to redundant information
        remainingNaluLength--;
        pCurPtrInNalu = nalu + 1;

        while (remainingNaluLength != 0) {
            curPayloadSize = MIN(maxPayloadSize, remainingNaluLength);
            payloadSubLenSize++;
            payloadLength += FU_A_HEADER_SIZE + curPayloadSize;

            if (!sizeCalculationOnly) {
                CHK(payloadSubLenSize <= pPayloadArray->maxPayloadSubLenSize && payloadLength <= pPayloadArray->maxPayloadLength,
                    STATUS_BUFFER_TOO_SMALL);

                MEMCPY(pPayload + FU_A_HEADER_SIZE, pCurPtrInNalu, curPayloadSize);
                /* FU-A indicator is 28 */
                pPayload[0] = 28 | naluRefIdc;
                pPayload[1] = naluType;
                if (remainingNaluLength == naluLength - 1) {
                    // Set for starting bit
                    pPayload[1] |= 1 << 7;
                } else if (remainingNaluLength == curPayloadSize) {
                    // Set for ending bit
                    pPayload[1] |= 1 << 6;
                }

                pPayloadArray->payloadSubLength[payloadSubLenSize - 1] = FU_A_HEADER_SIZE + curPayloadSize;
                pPayload += pPayloadArray->payloadSubLength[payloadSubLenSize - 1];
            }

            pCurPtrInNalu += curPayloadSize;
            remainingNaluLength -= curPayloadSize;
        }
    }

CleanUp:
    if (STATUS_FAILED(retStatus) && sizeCalculationOnly) {
        payloadLength = 0;
        payloadSubLenSize = 0;
    }

    if (filledLength != NULL && filledSubLenSize != NULL) {
        *filledLength = payloadLength;
        *filledSubLenSize = payloadSubLenSize;
    }

    LEAVES();
    return retStatus;
}

STATUS depayH264FromRtpPayload(PBYTE pRawPacket, UINT32 packetLength, PBYTE pNaluData, PUINT32 pNaluLength, PBOOL pIsStart)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 naluLength = 0;
    UINT8 naluType = 0;
    UINT8 naluRefIdc = 0;
    UINT8 indicator = 0;
    BOOL sizeCalculationOnly = (pNaluData == NULL);
    BOOL isStartingPacket = FALSE;
    BOOL isFrameStart = FALSE;
    PBYTE pCurPtr = pRawPacket;
    static BYTE start4ByteCode[] = {0x00, 0x00, 0x00, 0x01};
    static BYTE start3ByteCode[] = {0x00, 0x00, 0x01};
    UINT16 subNaluSize = 0;
    BOOL firstNaluInStap = TRUE;

    // pIsStart serves as input/output:
    //   Input:  if non-NULL and *pIsStart == FALSE, this is NOT the first NAL in the frame
    //           -> use 3-byte start codes instead of 4-byte for the leading NAL.
    //           NULL or *pIsStart == TRUE -> use 4-byte (backward compatible default).
    //   Output: set to TRUE if this packet begins a new access unit (frame). For VCL slice
    //           NALUs this requires slice_header.first_mb_in_slice == 0; for SPS/PPS/SEI/AUD
    //           it is unconditional; for FU-A non-start fragments it is always FALSE.
    BOOL useShortStartCode = (pIsStart != NULL && *pIsStart == FALSE);
    PBYTE startCode = useShortStartCode ? start3ByteCode : start4ByteCode;
    UINT32 startCodeSize = useShortStartCode ? SIZEOF(start3ByteCode) : SIZEOF(start4ByteCode);

    CHK(pRawPacket != NULL && pNaluLength != NULL, STATUS_NULL_ARG);
    CHK(packetLength > 0, retStatus);

    // indicator for types https://tools.ietf.org/html/rfc3984#section-5.2
    indicator = *pRawPacket & NAL_TYPE_MASK;
    switch (indicator) {
        case FU_A_INDICATOR:
            // FU-A indicator
            naluRefIdc = *pCurPtr & 0x60;
            pCurPtr++;
            naluType = *pCurPtr & 0x1f;
            isStartingPacket = (*pCurPtr & (1 << 7)) != 0;
            if (isStartingPacket) {
                naluLength = packetLength - FU_A_HEADER_SIZE + 1;
                // A FU-A start fragment is a frame start only if the underlying NALU
                // type says so. For slice/IDR, parse first_mb_in_slice from the fragment
                // payload (which begins at FU_A_HEADER_SIZE; the original NAL header was
                // stripped by the packetizer, so slice_header bytes start there directly).
                if (naluType == H264_NALU_TYPE_SPS || naluType == H264_NALU_TYPE_PPS || naluType == H264_NALU_TYPE_SEI ||
                    naluType == H264_NALU_TYPE_AUD) {
                    isFrameStart = TRUE;
                } else if (naluType == H264_NALU_TYPE_SLICE || naluType == H264_NALU_TYPE_IDR) {
                    UINT32 firstMb = 0;
                    if (packetLength > FU_A_HEADER_SIZE &&
                        h264ReadUeGolomb(pRawPacket + FU_A_HEADER_SIZE, packetLength - FU_A_HEADER_SIZE, &firstMb)) {
                        isFrameStart = (firstMb == 0);
                    }
                }
            } else {
                naluLength = packetLength - FU_A_HEADER_SIZE;
            }
            break;
        case FU_B_INDICATOR:
            // FU-B indicator
            naluLength = packetLength - FU_A_HEADER_SIZE + 1;
            break;
        case STAP_A_INDICATOR:
            pCurPtr += STAP_A_HEADER_SIZE;
            firstNaluInStap = TRUE;
            do {
                subNaluSize = getUnalignedInt16BigEndian(pCurPtr);
                pCurPtr += SIZEOF(UINT16);
                // First NAL in packet uses startCode (4-byte if first in frame, 3-byte otherwise),
                // subsequent NALs in STAP always use 3-byte
                naluLength += subNaluSize + (firstNaluInStap ? startCodeSize : SIZEOF(start3ByteCode));
                // If any contained NALU is a frame start, this STAP-A begins a frame.
                if (!isFrameStart && h264NaluIsFrameStart(pCurPtr, subNaluSize)) {
                    isFrameStart = TRUE;
                }
                firstNaluInStap = FALSE;
                pCurPtr += subNaluSize;
            } while (subNaluSize > 0 && pCurPtr < pRawPacket + packetLength);
            isStartingPacket = TRUE;
            break;
        case STAP_B_INDICATOR:
            pCurPtr += STAP_B_HEADER_SIZE;
            firstNaluInStap = TRUE;
            do {
                subNaluSize = getUnalignedInt16BigEndian(pCurPtr);
                pCurPtr += SIZEOF(UINT16);
                naluLength += subNaluSize + (firstNaluInStap ? startCodeSize : SIZEOF(start3ByteCode));
                if (!isFrameStart && h264NaluIsFrameStart(pCurPtr, subNaluSize)) {
                    isFrameStart = TRUE;
                }
                firstNaluInStap = FALSE;
                pCurPtr += subNaluSize;
            } while (subNaluSize > 0 && pCurPtr < pRawPacket + packetLength);
            isStartingPacket = TRUE;
            break;
        default:
            // Single NALU https://tools.ietf.org/html/rfc6184#section-5.6
            naluLength = packetLength;
            isStartingPacket = TRUE;
            isFrameStart = h264NaluIsFrameStart(pRawPacket, packetLength);
    }

    if (isStartingPacket && indicator != STAP_A_INDICATOR && indicator != STAP_B_INDICATOR) {
        naluLength += startCodeSize;
    }

    // Only return size if given buffer is NULL
    CHK(!sizeCalculationOnly, retStatus);
    CHK(naluLength <= *pNaluLength, STATUS_BUFFER_TOO_SMALL);

    if (isStartingPacket && indicator != STAP_A_INDICATOR && indicator != STAP_B_INDICATOR) {
        MEMCPY(pNaluData, startCode, startCodeSize);
        naluLength -= startCodeSize;
        pNaluData += startCodeSize;
    }
    switch (indicator) {
        case FU_A_INDICATOR:
            DLOGS("FU_A_INDICATOR starting packet %d len %d", isStartingPacket, naluLength);
            if (isStartingPacket) {
                MEMCPY(pNaluData + 1, pRawPacket + FU_A_HEADER_SIZE, naluLength - 1);
                *pNaluData = naluRefIdc | naluType;
            } else {
                MEMCPY(pNaluData, pRawPacket + FU_A_HEADER_SIZE, naluLength);
            }
            break;
        case FU_B_INDICATOR:
            DLOGS("FU_B_INDICATOR starting packet %d len %d", isStartingPacket, naluLength);
            if (isStartingPacket) {
                MEMCPY(pNaluData + 1, pRawPacket + FU_B_HEADER_SIZE, naluLength - 1);
                *pNaluData = naluRefIdc | naluType;
            } else {
                MEMCPY(pNaluData, pRawPacket + FU_B_HEADER_SIZE, naluLength);
            }
            break;
        case STAP_A_INDICATOR:
            naluLength = 0;
            pCurPtr = pRawPacket + STAP_A_HEADER_SIZE;
            firstNaluInStap = TRUE;
            do {
                subNaluSize = getUnalignedInt16BigEndian(pCurPtr);
                pCurPtr += SIZEOF(UINT16);
                if (firstNaluInStap) {
                    MEMCPY(pNaluData, startCode, startCodeSize);
                    pNaluData += startCodeSize;
                    naluLength += startCodeSize;
                    firstNaluInStap = FALSE;
                } else {
                    MEMCPY(pNaluData, start3ByteCode, SIZEOF(start3ByteCode));
                    pNaluData += SIZEOF(start3ByteCode);
                    naluLength += SIZEOF(start3ByteCode);
                }
                MEMCPY(pNaluData, pCurPtr, subNaluSize);
                pCurPtr += subNaluSize;
                pNaluData += subNaluSize;
                naluLength += subNaluSize;
            } while (subNaluSize > 0 && pCurPtr < pRawPacket + packetLength);
            DLOGS("STAP_A_INDICATOR starting packet %d len %d", isStartingPacket, naluLength);
            break;
        case STAP_B_INDICATOR:
            naluLength = 0;
            pCurPtr = pRawPacket + STAP_B_HEADER_SIZE;
            firstNaluInStap = TRUE;
            do {
                subNaluSize = getUnalignedInt16BigEndian(pCurPtr);
                pCurPtr += SIZEOF(UINT16);
                if (firstNaluInStap) {
                    MEMCPY(pNaluData, startCode, startCodeSize);
                    pNaluData += startCodeSize;
                    naluLength += startCodeSize;
                    firstNaluInStap = FALSE;
                } else {
                    MEMCPY(pNaluData, start3ByteCode, SIZEOF(start3ByteCode));
                    pNaluData += SIZEOF(start3ByteCode);
                    naluLength += SIZEOF(start3ByteCode);
                }
                MEMCPY(pNaluData, pCurPtr, subNaluSize);
                pCurPtr += subNaluSize;
                pNaluData += subNaluSize;
                naluLength += subNaluSize;
            } while (subNaluSize > 0 && pCurPtr < pRawPacket + packetLength);
            DLOGS("STAP_B_INDICATOR starting packet %d len %d", isStartingPacket, naluLength);
            break;
        default:
            DLOGS("Single NALU %d len %d", isStartingPacket, packetLength);
            MEMCPY(pNaluData, pRawPacket, naluLength);
    }
    if (isStartingPacket && indicator != STAP_A_INDICATOR && indicator != STAP_B_INDICATOR) {
        naluLength += startCodeSize;
    }
    DLOGS("Wrote naluLength %d isStartingPacket %d", naluLength, isStartingPacket);

CleanUp:
    if (STATUS_FAILED(retStatus) && sizeCalculationOnly) {
        naluLength = 0;
    }

    if (pNaluLength != NULL) {
        *pNaluLength = naluLength;
    }

    if (pIsStart != NULL) {
        *pIsStart = isFrameStart;
    }

    LEAVES();
    return retStatus;
}
