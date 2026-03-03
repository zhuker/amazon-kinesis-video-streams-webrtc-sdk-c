#define LOG_CLASS "SctpAssoc"
#include "../Include_i.h"

#define SCTP_NOW_MS() (GETTIME() / (10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND))

// Comparisons for wrapping TSNs (RFC 9260 serial number arithmetic)
#define TSN_LT(a, b)  ((INT32) ((a) - (b)) < 0)
#define TSN_LTE(a, b) ((INT32) ((a) - (b)) <= 0)
#define TSN_GT(a, b)  ((INT32) ((a) - (b)) > 0)
#define TSN_GTE(a, b) ((INT32) ((a) - (b)) >= 0)

static UINT32 sctpGenerateTag()
{
    UINT32 tag = 0;
    while (tag == 0) {
        tag = (UINT32) (GETTIME() & 0xFFFFFFFF);
        tag ^= (UINT32) ((GETTIME() >> 32) & 0xFFFFFFFF);
        // Ensure nonzero
        if (tag == 0) {
            tag = 0x12345678;
        }
    }
    return tag;
}

static VOID sctpSendPacket(PSctpAssociation pAssoc, UINT32 packetLen, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    sctpFinalizePacket(pAssoc->outPacket, packetLen);
    outboundFn(outboundCustomData, pAssoc->outPacket, packetLen);
}

// Flush queued messages after association reaches ESTABLISHED.
// sctpAssocSend() is declared in SctpAssoc.h, so it's visible here.
static VOID sctpFlushSendQueue(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 i;
    for (i = 0; i < SCTP_MAX_QUEUED_MESSAGES; i++) {
        if (!pAssoc->sendQueue[i].inUse) {
            continue;
        }
        DLOGD("SCTP: Flushing queued message for stream %u (ppid=%u, len=%u)", pAssoc->sendQueue[i].streamId, pAssoc->sendQueue[i].ppid,
              pAssoc->sendQueue[i].payloadLen);
        sctpAssocSend(pAssoc, pAssoc->sendQueue[i].streamId, pAssoc->sendQueue[i].ppid, pAssoc->sendQueue[i].unordered, pAssoc->sendQueue[i].payload,
                      pAssoc->sendQueue[i].payloadLen, pAssoc->sendQueue[i].maxRetransmits, pAssoc->sendQueue[i].lifetimeMs, outboundFn,
                      outboundCustomData);
        pAssoc->sendQueue[i].inUse = FALSE;
        pAssoc->sendQueueCount--;
    }
}

static VOID sctpSendSack(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 offset;
    UINT16 gapStarts[SCTP_MAX_GAP_BLOCKS];
    UINT16 gapEnds[SCTP_MAX_GAP_BLOCKS];
    UINT16 numGaps = 0;
    UINT32 i, j, minTsn, temp;
    UINT32 sortedTsns[SCTP_MAX_RECEIVED];
    UINT32 sortedCount;

    if (!pAssoc->peerCumulativeTsnValid) {
        return;
    }

    // Sort received out-of-order TSNs and build gap ack blocks
    sortedCount = pAssoc->receivedTsnCount;
    if (sortedCount > SCTP_MAX_RECEIVED) {
        sortedCount = SCTP_MAX_RECEIVED;
    }
    MEMCPY(sortedTsns, pAssoc->receivedTsns, sortedCount * sizeof(UINT32));

    // Simple insertion sort (small N in practice)
    for (i = 1; i < sortedCount; i++) {
        temp = sortedTsns[i];
        j = i;
        while (j > 0 && TSN_GT(sortedTsns[j - 1], temp)) {
            sortedTsns[j] = sortedTsns[j - 1];
            j--;
        }
        sortedTsns[j] = temp;
    }

    // Build gap blocks relative to peerCumulativeTsn
    for (i = 0; i < sortedCount && numGaps < SCTP_MAX_GAP_BLOCKS; i++) {
        UINT32 tsn = sortedTsns[i];
        if (TSN_LTE(tsn, pAssoc->peerCumulativeTsn)) {
            continue;
        }
        UINT16 offset16 = (UINT16) (tsn - pAssoc->peerCumulativeTsn);
        if (numGaps > 0 && offset16 == gapEnds[numGaps - 1] + 1) {
            // Extend current gap block
            gapEnds[numGaps - 1] = offset16;
        } else {
            // New gap block
            gapStarts[numGaps] = offset16;
            gapEnds[numGaps] = offset16;
            numGaps++;
        }
    }

    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
    offset += sctpWriteSackChunk(pAssoc->outPacket + offset, pAssoc->peerCumulativeTsn, SCTP_DEFAULT_ARWND, gapStarts, gapEnds, numGaps);

    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);
    pAssoc->needSack = FALSE;
}

static STATUS sctpHandleInit(PSctpAssociation pAssoc, PBYTE pValue, UINT32 valueLen, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 peerTag, peerArwnd, peerInitialTsn;
    UINT16 peerOutStreams, peerInStreams;
    UINT32 offset;
    SctpCookie cookie;

    if (valueLen < 16) {
        return STATUS_SUCCESS; // ignore malformed
    }

    peerTag = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 0));
    peerArwnd = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 4));
    peerOutStreams = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 8));
    peerInStreams = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 10));
    peerInitialTsn = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 12));

    DLOGD("SCTP: Received INIT tag=0x%08x arwnd=%u os=%u is=%u tsn=%u", peerTag, peerArwnd, peerOutStreams, peerInStreams, peerInitialTsn);

    // Build cookie
    MEMSET(&cookie, 0, sizeof(SctpCookie));
    cookie.magic1 = SCTP_COOKIE_MAGIC1;
    cookie.magic2 = SCTP_COOKIE_MAGIC2;
    cookie.peerTag = peerTag;
    cookie.myTag = pAssoc->myVerificationTag;
    cookie.peerInitialTsn = peerInitialTsn;
    cookie.myInitialTsn = pAssoc->nextTsn;
    cookie.peerArwnd = peerArwnd;
    cookie.tieTag = pAssoc->tieTag;
    cookie.numInStreams = (peerOutStreams < SCTP_MAX_STREAMS) ? peerOutStreams : SCTP_MAX_STREAMS;
    cookie.numOutStreams = (peerInStreams < SCTP_MAX_STREAMS) ? peerInStreams : SCTP_MAX_STREAMS;

    // Send INIT-ACK with vtag = peer's initiate tag
    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, peerTag);
    offset += sctpWriteInitAckChunk(pAssoc->outPacket + offset, pAssoc->myVerificationTag, SCTP_DEFAULT_ARWND, SCTP_MAX_STREAMS, SCTP_MAX_STREAMS,
                                    pAssoc->nextTsn, (PBYTE) &cookie, SCTP_COOKIE_SIZE);

    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

    return STATUS_SUCCESS;
}

static STATUS sctpHandleInitAck(PSctpAssociation pAssoc, PBYTE pValue, UINT32 valueLen, SctpAssocOutboundPacketFn outboundFn,
                                UINT64 outboundCustomData)
{
    UINT32 peerTag, peerArwnd, peerInitialTsn;
    UINT16 peerOutStreams, peerInStreams;
    UINT32 paramOffset;
    UINT16 paramType, paramLen;
    UINT32 offset;
    BOOL cookieFound = FALSE;

    if (valueLen < 16) {
        return STATUS_SUCCESS;
    }

    if (pAssoc->state != SCTP_ASSOC_COOKIE_WAIT && pAssoc->state != SCTP_ASSOC_CLOSED) {
        DLOGD("SCTP: Ignoring INIT-ACK in state %d", pAssoc->state);
        return STATUS_SUCCESS;
    }

    peerTag = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 0));
    peerArwnd = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 4));
    peerOutStreams = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 8));
    peerInStreams = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 10));
    peerInitialTsn = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 12));

    DLOGD("SCTP: Received INIT-ACK tag=0x%08x arwnd=%u tsn=%u", peerTag, peerArwnd, peerInitialTsn);

    pAssoc->peerVerificationTag = peerTag;
    pAssoc->peerArwnd = peerArwnd;
    pAssoc->peerCumulativeTsn = peerInitialTsn - 1;
    pAssoc->peerCumulativeTsnValid = TRUE;

    // Find State Cookie parameter
    paramOffset = 16;
    while (paramOffset + 4 <= valueLen) {
        paramType = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + paramOffset));
        paramLen = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + paramOffset + 2));
        if (paramLen < 4) {
            break;
        }
        if (paramType == SCTP_PARAM_STATE_COOKIE) {
            UINT32 cookieDataLen = paramLen - 4;
            if (cookieDataLen <= SCTP_COOKIE_SIZE && paramOffset + paramLen <= valueLen) {
                MEMCPY(pAssoc->cookieEchoData, pValue + paramOffset + 4, cookieDataLen);
                pAssoc->cookieEchoValid = TRUE;
                cookieFound = TRUE;
            }
            break;
        }
        // Advance to next parameter (padded to 4 bytes)
        paramOffset += ((paramLen + 3) & ~3u);
    }

    if (!cookieFound) {
        DLOGW("SCTP: No State Cookie in INIT-ACK");
        return STATUS_SUCCESS;
    }

    // Send COOKIE-ECHO
    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
    offset += sctpWriteCookieEchoChunk(pAssoc->outPacket + offset, pAssoc->cookieEchoData, SCTP_COOKIE_SIZE);

    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

    pAssoc->state = SCTP_ASSOC_COOKIE_ECHOED;
    pAssoc->t1InitExpiry = SCTP_NOW_MS() + pAssoc->rtoMs;
    DLOGD("SCTP: Sent COOKIE-ECHO, state -> COOKIE_ECHOED");

    return STATUS_SUCCESS;
}

static STATUS sctpHandleCookieEcho(PSctpAssociation pAssoc, PBYTE pValue, UINT32 valueLen, SctpAssocOutboundPacketFn outboundFn,
                                   UINT64 outboundCustomData)
{
    SctpCookie* pCookie;
    UINT32 offset;

    if (valueLen < SCTP_COOKIE_SIZE) {
        DLOGW("SCTP: COOKIE-ECHO too small (%u bytes)", valueLen);
        return STATUS_SUCCESS;
    }

    pCookie = (SctpCookie*) pValue;
    if (pCookie->magic1 != SCTP_COOKIE_MAGIC1 || pCookie->magic2 != SCTP_COOKIE_MAGIC2) {
        DLOGW("SCTP: Invalid cookie magic");
        return STATUS_SUCCESS;
    }

    DLOGD("SCTP: Valid COOKIE-ECHO received, setting up association");

    // Apply cookie parameters
    pAssoc->peerVerificationTag = pCookie->peerTag;
    pAssoc->peerCumulativeTsn = pCookie->peerInitialTsn - 1;
    pAssoc->peerCumulativeTsnValid = TRUE;
    pAssoc->peerArwnd = pCookie->peerArwnd;

    // Send COOKIE-ACK with vtag = peer's tag
    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
    offset += sctpWriteCookieAckChunk(pAssoc->outPacket + offset);

    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

    pAssoc->state = SCTP_ASSOC_ESTABLISHED;
    pAssoc->t1InitExpiry = 0; // stop T1 timer
    DLOGI("SCTP: Association ESTABLISHED (via COOKIE-ECHO)");

    // Flush any messages queued before ESTABLISHED
    sctpFlushSendQueue(pAssoc, outboundFn, outboundCustomData);

    return STATUS_SUCCESS;
}

static STATUS sctpHandleCookieAck(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    if (pAssoc->state != SCTP_ASSOC_COOKIE_ECHOED) {
        DLOGD("SCTP: Ignoring COOKIE-ACK in state %d", pAssoc->state);
        return STATUS_SUCCESS;
    }

    pAssoc->state = SCTP_ASSOC_ESTABLISHED;
    pAssoc->t1InitExpiry = 0; // stop T1 timer
    DLOGI("SCTP: Association ESTABLISHED (via COOKIE-ACK)");

    // Flush any messages queued before ESTABLISHED
    sctpFlushSendQueue(pAssoc, outboundFn, outboundCustomData);

    return STATUS_SUCCESS;
}

static STATUS sctpHandleData(PSctpAssociation pAssoc, UINT8 flags, PBYTE pValue, UINT32 valueLen, SctpAssocOutboundPacketFn outboundFn,
                             UINT64 outboundCustomData, SctpAssocMessageFn messageFn, UINT64 messageCustomData)
{
    UINT32 tsn, ppid;
    UINT16 streamId, ssn;
    PBYTE pPayload;
    UINT32 payloadLen;

    if (valueLen < 12) { // TSN(4) + SID(2) + SSN(2) + PPID(4) = 12 minimum
        return STATUS_SUCCESS;
    }

    if (pAssoc->state != SCTP_ASSOC_ESTABLISHED) {
        return STATUS_SUCCESS;
    }

    tsn = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 0));
    streamId = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 4));
    ssn = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 6));
    ppid = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 8));
    pPayload = pValue + 12;
    payloadLen = valueLen - 12;

    // Update receive tracking
    if (!pAssoc->peerCumulativeTsnValid) {
        pAssoc->peerCumulativeTsn = tsn - 1;
        pAssoc->peerCumulativeTsnValid = TRUE;
    }

    if (tsn == pAssoc->peerCumulativeTsn + 1) {
        // In-order: advance cumulative TSN
        pAssoc->peerCumulativeTsn = tsn;

        // Check if any out-of-order TSNs are now consecutive
        BOOL advanced = TRUE;
        while (advanced) {
            advanced = FALSE;
            UINT32 k;
            for (k = 0; k < pAssoc->receivedTsnCount; k++) {
                if (pAssoc->receivedTsns[k] == pAssoc->peerCumulativeTsn + 1) {
                    pAssoc->peerCumulativeTsn++;
                    // Remove this entry by swapping with last
                    pAssoc->receivedTsns[k] = pAssoc->receivedTsns[pAssoc->receivedTsnCount - 1];
                    pAssoc->receivedTsnCount--;
                    advanced = TRUE;
                    break;
                }
            }
        }
    } else if (TSN_GT(tsn, pAssoc->peerCumulativeTsn + 1)) {
        // Out-of-order: add to received list if not duplicate
        BOOL duplicate = FALSE;
        UINT32 k;
        for (k = 0; k < pAssoc->receivedTsnCount; k++) {
            if (pAssoc->receivedTsns[k] == tsn) {
                duplicate = TRUE;
                break;
            }
        }
        if (!duplicate && pAssoc->receivedTsnCount < SCTP_MAX_RECEIVED) {
            pAssoc->receivedTsns[pAssoc->receivedTsnCount++] = tsn;
        }
    } else {
        // Duplicate or old TSN — ignore but still SACK
        pAssoc->needSack = TRUE;
        sctpSendSack(pAssoc, outboundFn, outboundCustomData);
        return STATUS_SUCCESS;
    }

    pAssoc->needSack = TRUE;

    // Deliver message to callback — handle both complete and fragmented messages
    if (messageFn != NULL) {
        BOOL isBegin = (flags & SCTP_DATA_FLAG_BEGIN) != 0;
        BOOL isEnd = (flags & SCTP_DATA_FLAG_END) != 0;

        if (isBegin && isEnd) {
            // Complete unfragmented message — deliver immediately
            messageFn(messageCustomData, streamId, ppid, pPayload, payloadLen);
        } else if (isBegin) {
            // First fragment — start reassembly
            if (pAssoc->reassemblyBuf == NULL) {
                pAssoc->reassemblyBuf = (PBYTE) MEMALLOC(SCTP_MAX_REASSEMBLY_SIZE);
            }
            if (pAssoc->reassemblyBuf != NULL && payloadLen <= SCTP_MAX_REASSEMBLY_SIZE) {
                MEMCPY(pAssoc->reassemblyBuf, pPayload, payloadLen);
                pAssoc->reassemblyLen = payloadLen;
                pAssoc->reassemblyStreamId = streamId;
                pAssoc->reassemblySsn = ssn;
                pAssoc->reassemblyPpid = ppid;
                pAssoc->reassemblyInProgress = TRUE;
            }
        } else if (pAssoc->reassemblyInProgress) {
            // Middle or last fragment — append
            if (pAssoc->reassemblyBuf != NULL && pAssoc->reassemblyLen + payloadLen <= SCTP_MAX_REASSEMBLY_SIZE) {
                MEMCPY(pAssoc->reassemblyBuf + pAssoc->reassemblyLen, pPayload, payloadLen);
                pAssoc->reassemblyLen += payloadLen;
            }
            if (isEnd) {
                // Last fragment — deliver complete message
                messageFn(messageCustomData, pAssoc->reassemblyStreamId, pAssoc->reassemblyPpid, pAssoc->reassemblyBuf, pAssoc->reassemblyLen);
                pAssoc->reassemblyInProgress = FALSE;
                pAssoc->reassemblyLen = 0;
            }
        }
    }

    // Send SACK immediately
    sctpSendSack(pAssoc, outboundFn, outboundCustomData);

    return STATUS_SUCCESS;
}

// Drain the pending-send queue after the congestion window has grown (called from sctpHandleSack).
static VOID sctpDrainPendingQueue(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 slot;
    for (slot = 0; slot < SCTP_MAX_PENDING_SENDS; slot++) {
        if (!pAssoc->pendingQueue[slot].inUse) {
            continue;
        }
        if (pAssoc->flightSize >= pAssoc->cwnd || pAssoc->flightSize >= pAssoc->peerArwnd ||
            pAssoc->outstandingCount >= SCTP_MAX_OUTSTANDING) {
            break; // still congested or outstanding table full — leave remaining entries in queue
        }
        // Snapshot and evict before calling sctpAssocSend, which may re-queue if still congested
        UINT16 streamId = pAssoc->pendingQueue[slot].streamId;
        UINT32 ppid = pAssoc->pendingQueue[slot].ppid;
        BOOL unordered = pAssoc->pendingQueue[slot].unordered;
        UINT16 maxRetransmits = pAssoc->pendingQueue[slot].maxRetransmits;
        UINT64 lifetimeMs = pAssoc->pendingQueue[slot].lifetimeMs;
        PBYTE payload = pAssoc->pendingQueue[slot].payload;
        UINT32 payloadLen = pAssoc->pendingQueue[slot].payloadLen;
        pAssoc->pendingQueue[slot].payload = NULL;
        pAssoc->pendingQueue[slot].inUse = FALSE;
        pAssoc->pendingQueueCount--;

        // sctpAssocSend makes its own copy if it re-queues, so freeing payload after is safe
        sctpAssocSend(pAssoc, streamId, ppid, unordered, payload, payloadLen, maxRetransmits, lifetimeMs, outboundFn, outboundCustomData);
        MEMFREE(payload);
    }
}

static STATUS sctpHandleSack(PSctpAssociation pAssoc, PBYTE pValue, UINT32 valueLen, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 cumTsn, peerArwnd;
    UINT16 numGaps, numDups;
    UINT32 i;
    UINT32 bytesNewlyAcked = 0;

    if (valueLen < 12) {
        return STATUS_SUCCESS;
    }

    cumTsn = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 0));
    peerArwnd = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 4));
    numGaps = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 8));
    numDups = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + 10));

    pAssoc->peerArwnd = peerArwnd;

    // Mark outstanding DATA chunks as acked
    for (i = 0; i < SCTP_MAX_OUTSTANDING; i++) {
        if (!pAssoc->outstanding[i].inUse || pAssoc->outstanding[i].acked) {
            continue;
        }
        if (TSN_LTE(pAssoc->outstanding[i].tsn, cumTsn)) {
            pAssoc->outstanding[i].acked = TRUE;
            bytesNewlyAcked += pAssoc->outstanding[i].payloadLen;
            pAssoc->flightSize -= pAssoc->outstanding[i].payloadLen;
            if (pAssoc->outstanding[i].payload != NULL) {
                MEMFREE(pAssoc->outstanding[i].payload);
                pAssoc->outstanding[i].payload = NULL;
            }
            pAssoc->outstanding[i].inUse = FALSE;
            pAssoc->outstandingCount--;
        }
    }

    // Also process gap ack blocks
    if (numGaps > 0 && valueLen >= 12 + numGaps * 4) {
        UINT32 gapOffset = 12;
        for (i = 0; i < numGaps; i++) {
            UINT16 gapStart = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + gapOffset));
            UINT16 gapEnd = (UINT16) getUnalignedInt16BigEndian((PINT16) (pValue + gapOffset + 2));
            gapOffset += 4;

            UINT32 gapTsnStart = cumTsn + gapStart;
            UINT32 gapTsnEnd = cumTsn + gapEnd;
            UINT32 j;

            for (j = 0; j < SCTP_MAX_OUTSTANDING; j++) {
                if (!pAssoc->outstanding[j].inUse || pAssoc->outstanding[j].acked) {
                    continue;
                }
                if (TSN_GTE(pAssoc->outstanding[j].tsn, gapTsnStart) && TSN_LTE(pAssoc->outstanding[j].tsn, gapTsnEnd)) {
                    pAssoc->outstanding[j].acked = TRUE;
                    bytesNewlyAcked += pAssoc->outstanding[j].payloadLen;
                    pAssoc->flightSize -= pAssoc->outstanding[j].payloadLen;
                    if (pAssoc->outstanding[j].payload != NULL) {
                        MEMFREE(pAssoc->outstanding[j].payload);
                        pAssoc->outstanding[j].payload = NULL;
                    }
                    pAssoc->outstanding[j].inUse = FALSE;
                    pAssoc->outstandingCount--;
                }
            }
        }
    }

    // Update cumulative ack
    if (TSN_GT(cumTsn, pAssoc->cumulativeAckTsn)) {
        pAssoc->cumulativeAckTsn = cumTsn;
    }

    // Congestion control: slow start
    if (bytesNewlyAcked > 0) {
        if (pAssoc->cwnd <= pAssoc->ssthresh) {
            // Slow start
            UINT32 increase = bytesNewlyAcked;
            if (increase > pAssoc->mtu) {
                increase = pAssoc->mtu;
            }
            pAssoc->cwnd += increase;
        } else {
            // Congestion avoidance
            pAssoc->cwnd += pAssoc->mtu;
        }
        // Stop T3 if nothing outstanding
        if (pAssoc->outstandingCount == 0) {
            pAssoc->t3RtxExpiry = 0;
        } else {
            // Restart T3
            pAssoc->t3RtxExpiry = SCTP_NOW_MS() + pAssoc->rtoMs;
        }

        // Window just opened — try to send any messages that were held back by congestion
        if (pAssoc->pendingQueueCount > 0) {
            sctpDrainPendingQueue(pAssoc, outboundFn, outboundCustomData);
        }
    }

    return STATUS_SUCCESS;
}

static STATUS sctpHandleForwardTsn(PSctpAssociation pAssoc, PBYTE pValue, UINT32 valueLen, SctpAssocOutboundPacketFn outboundFn,
                                   UINT64 outboundCustomData)
{
    UINT32 newCumTsn;

    if (valueLen < 4) {
        return STATUS_SUCCESS;
    }

    newCumTsn = (UINT32) getUnalignedInt32BigEndian((PINT32) (pValue + 0));

    if (TSN_GT(newCumTsn, pAssoc->peerCumulativeTsn)) {
        pAssoc->peerCumulativeTsn = newCumTsn;

        // Remove any received TSNs <= newCumTsn
        UINT32 i = 0;
        while (i < pAssoc->receivedTsnCount) {
            if (TSN_LTE(pAssoc->receivedTsns[i], newCumTsn)) {
                pAssoc->receivedTsns[i] = pAssoc->receivedTsns[pAssoc->receivedTsnCount - 1];
                pAssoc->receivedTsnCount--;
            } else {
                i++;
            }
        }

        // Advance cumulative if consecutive TSNs now exist
        BOOL advanced = TRUE;
        while (advanced) {
            advanced = FALSE;
            UINT32 k;
            for (k = 0; k < pAssoc->receivedTsnCount; k++) {
                if (pAssoc->receivedTsns[k] == pAssoc->peerCumulativeTsn + 1) {
                    pAssoc->peerCumulativeTsn++;
                    pAssoc->receivedTsns[k] = pAssoc->receivedTsns[pAssoc->receivedTsnCount - 1];
                    pAssoc->receivedTsnCount--;
                    advanced = TRUE;
                    break;
                }
            }
        }
    }

    // Send SACK in response
    pAssoc->needSack = TRUE;
    sctpSendSack(pAssoc, outboundFn, outboundCustomData);

    return STATUS_SUCCESS;
}

static STATUS sctpHandleShutdown(PSctpAssociation pAssoc, PBYTE pValue, UINT32 valueLen, SctpAssocOutboundPacketFn outboundFn,
                                 UINT64 outboundCustomData)
{
    UINT32 offset;

    // Send SHUTDOWN-ACK
    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
    offset += sctpWriteShutdownAckChunk(pAssoc->outPacket + offset);
    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

    pAssoc->state = SCTP_ASSOC_SHUTDOWN_ACK_SENT;
    return STATUS_SUCCESS;
}

static STATUS sctpHandleShutdownAck(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 offset;

    // Send SHUTDOWN-COMPLETE
    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
    offset += sctpWriteShutdownCompleteChunk(pAssoc->outPacket + offset);
    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

    pAssoc->state = SCTP_ASSOC_CLOSED;
    return STATUS_SUCCESS;
}

// Context for chunk dispatch
typedef struct {
    PSctpAssociation pAssoc;
    SctpAssocOutboundPacketFn outboundFn;
    UINT64 outboundCustomData;
    SctpAssocMessageFn messageFn;
    UINT64 messageCustomData;
} SctpChunkDispatchContext;

static STATUS sctpChunkDispatch(UINT8 type, UINT8 flags, PBYTE pValue, UINT32 valueLen, UINT64 customData)
{
    STATUS retStatus = STATUS_SUCCESS;
    SctpChunkDispatchContext* pCtx = (SctpChunkDispatchContext*) customData;

    switch (type) {
        case SCTP_CHUNK_INIT:
            CHK_STATUS(sctpHandleInit(pCtx->pAssoc, pValue, valueLen, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_INIT_ACK:
            CHK_STATUS(sctpHandleInitAck(pCtx->pAssoc, pValue, valueLen, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_COOKIE_ECHO:
            CHK_STATUS(sctpHandleCookieEcho(pCtx->pAssoc, pValue, valueLen, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_COOKIE_ACK:
            CHK_STATUS(sctpHandleCookieAck(pCtx->pAssoc, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_DATA:
            CHK_STATUS(sctpHandleData(pCtx->pAssoc, flags, pValue, valueLen, pCtx->outboundFn, pCtx->outboundCustomData, pCtx->messageFn,
                                      pCtx->messageCustomData));
            break;
        case SCTP_CHUNK_SACK:
            CHK_STATUS(sctpHandleSack(pCtx->pAssoc, pValue, valueLen, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_FORWARD_TSN:
            CHK_STATUS(sctpHandleForwardTsn(pCtx->pAssoc, pValue, valueLen, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_HEARTBEAT:
            // Respond with HEARTBEAT-ACK (echo the same data)
            {
                UINT32 offset = sctpWriteCommonHeader(pCtx->pAssoc->outPacket, pCtx->pAssoc->localPort, pCtx->pAssoc->remotePort,
                                                      pCtx->pAssoc->peerVerificationTag);
                if (offset + SCTP_CHUNK_HEADER_SIZE + valueLen <= SCTP_MAX_PACKET_SIZE) {
                    pCtx->pAssoc->outPacket[offset] = SCTP_CHUNK_HEARTBEAT_ACK;
                    pCtx->pAssoc->outPacket[offset + 1] = 0;
                    putUnalignedInt16BigEndian(pCtx->pAssoc->outPacket + offset + 2, (INT16) (SCTP_CHUNK_HEADER_SIZE + valueLen));
                    MEMCPY(pCtx->pAssoc->outPacket + offset + SCTP_CHUNK_HEADER_SIZE, pValue, valueLen);
                    offset += SCTP_PAD4(SCTP_CHUNK_HEADER_SIZE + valueLen);
                    sctpSendPacket(pCtx->pAssoc, offset, pCtx->outboundFn, pCtx->outboundCustomData);
                }
            }
            break;
        case SCTP_CHUNK_SHUTDOWN:
            CHK_STATUS(sctpHandleShutdown(pCtx->pAssoc, pValue, valueLen, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_SHUTDOWN_ACK:
            CHK_STATUS(sctpHandleShutdownAck(pCtx->pAssoc, pCtx->outboundFn, pCtx->outboundCustomData));
            break;
        case SCTP_CHUNK_SHUTDOWN_COMPLETE:
            pCtx->pAssoc->state = SCTP_ASSOC_CLOSED;
            DLOGI("SCTP: Shutdown complete");
            break;
        case SCTP_CHUNK_ABORT:
            pCtx->pAssoc->state = SCTP_ASSOC_CLOSED;
            DLOGW("SCTP: Received ABORT");
            break;
        default:
            DLOGD("SCTP: Unhandled chunk type %u", type);
            break;
    }

CleanUp:
    return retStatus;
}

/******************************************************************************
 * Public API
 *****************************************************************************/

VOID sctpAssocInit(PSctpAssociation pAssoc, UINT16 localPort, UINT16 remotePort, UINT16 mtu)
{
    MEMSET(pAssoc, 0, SIZEOF(SctpAssociation));
    pAssoc->state = SCTP_ASSOC_CLOSED;
    pAssoc->localPort = localPort;
    pAssoc->remotePort = remotePort;
    pAssoc->mtu = mtu;
    pAssoc->myVerificationTag = sctpGenerateTag();
    pAssoc->nextTsn = pAssoc->myVerificationTag; // RFC 9260: initial TSN can be same as tag
    pAssoc->cumulativeAckTsn = pAssoc->nextTsn - 1;
    pAssoc->advancedPeerAckPoint = pAssoc->nextTsn - 1;
    pAssoc->cwnd = SCTP_INITIAL_CWND_MTUS * mtu;
    pAssoc->ssthresh = SCTP_DEFAULT_ARWND;
    pAssoc->peerArwnd = SCTP_DEFAULT_ARWND;
    pAssoc->rtoMs = SCTP_RTO_INITIAL_MS;
    pAssoc->srttMs = 0;
    pAssoc->tieTag = ((UINT64) sctpGenerateTag() << 32) | sctpGenerateTag();
}

STATUS sctpAssocConnect(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 offset;

    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, 0); // INIT vtag=0
    offset += sctpWriteInitChunk(pAssoc->outPacket + offset, pAssoc->myVerificationTag, SCTP_DEFAULT_ARWND, SCTP_MAX_STREAMS, SCTP_MAX_STREAMS,
                                 pAssoc->nextTsn);

    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

    pAssoc->state = SCTP_ASSOC_COOKIE_WAIT;
    pAssoc->t1InitExpiry = SCTP_NOW_MS() + pAssoc->rtoMs;
    pAssoc->initRetransmitCount = 0;

    DLOGD("SCTP: Sent INIT, state -> COOKIE_WAIT");
    return STATUS_SUCCESS;
}

STATUS sctpAssocHandlePacket(PSctpAssociation pAssoc, PBYTE pBuf, UINT32 bufLen, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData,
                             SctpAssocMessageFn messageFn, UINT64 messageCustomData)
{
    STATUS retStatus = STATUS_SUCCESS;
    SctpChunkDispatchContext ctx;

    if (bufLen < SCTP_COMMON_HEADER_SIZE) {
        return STATUS_SUCCESS;
    }

    // Validate CRC32c — allow vtag=0 for INIT, or our tag for everything else
    retStatus = sctpValidatePacket(pBuf, bufLen, pAssoc->myVerificationTag);
    if (STATUS_FAILED(retStatus)) {
        // Don't fail the whole session on a bad packet
        DLOGW("SCTP: Packet validation failed, dropping");
        return STATUS_SUCCESS;
    }

    ctx.pAssoc = pAssoc;
    ctx.outboundFn = outboundFn;
    ctx.outboundCustomData = outboundCustomData;
    ctx.messageFn = messageFn;
    ctx.messageCustomData = messageCustomData;

    CHK_STATUS(sctpParseChunks(pBuf, bufLen, sctpChunkDispatch, (UINT64) &ctx));

CleanUp:
    return retStatus;
}

STATUS sctpAssocSend(PSctpAssociation pAssoc, UINT16 streamId, UINT32 ppid, BOOL unordered, PBYTE pPayload, UINT32 payloadLen, UINT16 maxRetransmits,
                     UINT64 lifetimeMs, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 offset;
    UINT32 tsn;
    UINT16 ssn = 0;
    UINT32 i;
    UINT64 nowMs = SCTP_NOW_MS();
    UINT32 maxDataPayload;

    if (pAssoc->state != SCTP_ASSOC_ESTABLISHED) {
        // Queue the message for later delivery (association not yet established)
        if (pAssoc->sendQueueCount < SCTP_MAX_QUEUED_MESSAGES && payloadLen <= SCTP_MAX_QUEUED_PAYLOAD) {
            for (i = 0; i < SCTP_MAX_QUEUED_MESSAGES; i++) {
                if (!pAssoc->sendQueue[i].inUse) {
                    pAssoc->sendQueue[i].streamId = streamId;
                    pAssoc->sendQueue[i].ppid = ppid;
                    pAssoc->sendQueue[i].unordered = unordered;
                    pAssoc->sendQueue[i].maxRetransmits = maxRetransmits;
                    pAssoc->sendQueue[i].lifetimeMs = lifetimeMs;
                    MEMCPY(pAssoc->sendQueue[i].payload, pPayload, payloadLen);
                    pAssoc->sendQueue[i].payloadLen = payloadLen;
                    pAssoc->sendQueue[i].inUse = TRUE;
                    pAssoc->sendQueueCount++;
                    DLOGD("SCTP: Queued message for stream %u (state=%d, queueCount=%u)", streamId, pAssoc->state, pAssoc->sendQueueCount);
                    return STATUS_SUCCESS;
                }
            }
        }
        DLOGW("SCTP: Cannot send in state %d, queue full", pAssoc->state);
        return STATUS_SUCCESS; // Don't fail the session, just drop
    }

    // Enforce congestion window.
    // On a slow device the cwnd starts at 3*MTU (~3.5 KB) and grows only via SACKs.
    // Ignoring this check fills the outstanding table (flightSize → 65536), starves
    // putSctpPacket/sctpSessionTickTimers of their mutex turn, and freezes the session.
    // Instead of dropping, queue the message; sctpDrainPendingQueue() sends it once a
    // SACK opens the window.  If the queue is also full we drop (log + silent discard).
    if (pAssoc->flightSize >= pAssoc->cwnd || pAssoc->flightSize >= pAssoc->peerArwnd) {
        if (pAssoc->pendingQueueCount < SCTP_MAX_PENDING_SENDS) {
            UINT32 slot;
            for (slot = 0; slot < SCTP_MAX_PENDING_SENDS; slot++) {
                if (!pAssoc->pendingQueue[slot].inUse) {
                    break;
                }
            }
            if (slot < SCTP_MAX_PENDING_SENDS) {
                pAssoc->pendingQueue[slot].streamId = streamId;
                pAssoc->pendingQueue[slot].ppid = ppid;
                pAssoc->pendingQueue[slot].unordered = unordered;
                pAssoc->pendingQueue[slot].maxRetransmits = maxRetransmits;
                pAssoc->pendingQueue[slot].lifetimeMs = lifetimeMs;
                pAssoc->pendingQueue[slot].payloadLen = payloadLen;
                pAssoc->pendingQueue[slot].payload = (PBYTE) MEMALLOC(payloadLen);
                if (pAssoc->pendingQueue[slot].payload != NULL) {
                    MEMCPY(pAssoc->pendingQueue[slot].payload, pPayload, payloadLen);
                    pAssoc->pendingQueue[slot].inUse = TRUE;
                    pAssoc->pendingQueueCount++;
                    DLOGD("SCTP: Congestion window full, queued (slot=%u flightSize=%u cwnd=%u)", slot, pAssoc->flightSize, pAssoc->cwnd);
                } else {
                    DLOGW("SCTP: OOM queuing congested message, dropping (stream=%u)", streamId);
                }
            }
        } else {
            DLOGD("SCTP: Congestion window full, pending queue full, dropping (flightSize=%u cwnd=%u peerArwnd=%u)", pAssoc->flightSize, pAssoc->cwnd,
                  pAssoc->peerArwnd);
        }
        return STATUS_SUCCESS;
    }

    // Assign SSN for ordered messages
    if (!unordered) {
        if (streamId < SCTP_MAX_STREAMS) {
            ssn = pAssoc->nextSsn[streamId]++;
        }
    }

    // Fragment if payload exceeds MTU
    maxDataPayload = pAssoc->mtu - SCTP_COMMON_HEADER_SIZE - SCTP_DATA_HEADER_SIZE;

    if (payloadLen <= maxDataPayload) {
        // Single fragment — common case

        // Find a free outstanding slot BEFORE consuming TSN/SSN so that if the table
        // is full we can queue to pendingQueue without leaking sequence numbers.
        for (i = 0; i < SCTP_MAX_OUTSTANDING; i++) {
            if (!pAssoc->outstanding[i].inUse) {
                break;
            }
        }
        if (i >= SCTP_MAX_OUTSTANDING) {
            // Outstanding table full — revert SSN and queue to pending so the message
            // is sent once a SACK frees a slot (same path as congestion window full).
            if (!unordered && streamId < SCTP_MAX_STREAMS) {
                pAssoc->nextSsn[streamId]--;
            }
            if (pAssoc->pendingQueueCount < SCTP_MAX_PENDING_SENDS) {
                UINT32 slot;
                for (slot = 0; slot < SCTP_MAX_PENDING_SENDS; slot++) {
                    if (!pAssoc->pendingQueue[slot].inUse) {
                        break;
                    }
                }
                if (slot < SCTP_MAX_PENDING_SENDS) {
                    pAssoc->pendingQueue[slot].streamId = streamId;
                    pAssoc->pendingQueue[slot].ppid = ppid;
                    pAssoc->pendingQueue[slot].unordered = unordered;
                    pAssoc->pendingQueue[slot].maxRetransmits = maxRetransmits;
                    pAssoc->pendingQueue[slot].lifetimeMs = lifetimeMs;
                    pAssoc->pendingQueue[slot].payloadLen = payloadLen;
                    pAssoc->pendingQueue[slot].payload = (PBYTE) MEMALLOC(payloadLen);
                    if (pAssoc->pendingQueue[slot].payload != NULL) {
                        MEMCPY(pAssoc->pendingQueue[slot].payload, pPayload, payloadLen);
                        pAssoc->pendingQueue[slot].inUse = TRUE;
                        pAssoc->pendingQueueCount++;
                        DLOGD("SCTP: Outstanding table full, queued to pending (slot=%u stream=%u)", slot, streamId);
                    } else {
                        DLOGW("SCTP: OOM queuing outstanding-full message, dropping (stream=%u)", streamId);
                    }
                }
            } else {
                DLOGD("SCTP: Outstanding table full, pending also full, dropping (stream=%u)", streamId);
            }
            return STATUS_SUCCESS;
        }

        tsn = pAssoc->nextTsn++;

        pAssoc->outstanding[i].tsn = tsn;
        pAssoc->outstanding[i].streamId = streamId;
        pAssoc->outstanding[i].ssn = ssn;
        pAssoc->outstanding[i].ppid = ppid;
        pAssoc->outstanding[i].payloadLen = payloadLen;
        pAssoc->outstanding[i].unordered = unordered;
        pAssoc->outstanding[i].sentTime = nowMs;
        pAssoc->outstanding[i].retransmitCount = 0;
        pAssoc->outstanding[i].acked = FALSE;
        pAssoc->outstanding[i].abandoned = FALSE;
        pAssoc->outstanding[i].maxRetransmits = maxRetransmits;
        pAssoc->outstanding[i].lifetimeMs = lifetimeMs;
        pAssoc->outstanding[i].creationTime = nowMs;
        pAssoc->outstanding[i].inUse = TRUE;

        // Copy payload for retransmit
        pAssoc->outstanding[i].payload = (PBYTE) MEMALLOC(payloadLen);
        if (pAssoc->outstanding[i].payload != NULL) {
            MEMCPY(pAssoc->outstanding[i].payload, pPayload, payloadLen);
        }

        pAssoc->outstandingCount++;
        pAssoc->flightSize += payloadLen;

        // Send DATA chunk
        offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
        offset += sctpWriteDataChunk(pAssoc->outPacket + offset, tsn, streamId, ssn, ppid, unordered, pPayload, payloadLen);
        sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

        // Start T3-rtx if not running
        if (pAssoc->t3RtxExpiry == 0) {
            pAssoc->t3RtxExpiry = nowMs + pAssoc->rtoMs;
        }
    } else {
        // Fragmentation — split payload across multiple DATA chunks
        UINT32 remaining = payloadLen;
        UINT32 fragmentOffset = 0;
        BOOL isFirst = TRUE;

        while (remaining > 0) {
            UINT32 fragmentLen = (remaining > maxDataPayload) ? maxDataPayload : remaining;
            BOOL isLast = (remaining - fragmentLen == 0);
            UINT8 fragFlags = 0;

            if (isFirst) {
                fragFlags |= SCTP_DATA_FLAG_BEGIN;
            }
            if (isLast) {
                fragFlags |= SCTP_DATA_FLAG_END;
            }
            if (unordered) {
                fragFlags |= SCTP_DATA_FLAG_UNORDERED;
            }

            tsn = pAssoc->nextTsn++;

            // Store in outstanding
            for (i = 0; i < SCTP_MAX_OUTSTANDING; i++) {
                if (!pAssoc->outstanding[i].inUse) {
                    break;
                }
            }
            if (i >= SCTP_MAX_OUTSTANDING) {
                DLOGW("SCTP: Outstanding table full during fragmentation, dropping (stream=%u)", streamId);
                return STATUS_SUCCESS;
            }

            pAssoc->outstanding[i].tsn = tsn;
            pAssoc->outstanding[i].streamId = streamId;
            pAssoc->outstanding[i].ssn = ssn;
            pAssoc->outstanding[i].ppid = ppid;
            pAssoc->outstanding[i].payloadLen = fragmentLen;
            pAssoc->outstanding[i].unordered = unordered;
            pAssoc->outstanding[i].sentTime = nowMs;
            pAssoc->outstanding[i].retransmitCount = 0;
            pAssoc->outstanding[i].acked = FALSE;
            pAssoc->outstanding[i].abandoned = FALSE;
            pAssoc->outstanding[i].maxRetransmits = maxRetransmits;
            pAssoc->outstanding[i].lifetimeMs = lifetimeMs;
            pAssoc->outstanding[i].creationTime = nowMs;
            pAssoc->outstanding[i].inUse = TRUE;

            pAssoc->outstanding[i].payload = (PBYTE) MEMALLOC(fragmentLen);
            if (pAssoc->outstanding[i].payload != NULL) {
                MEMCPY(pAssoc->outstanding[i].payload, pPayload + fragmentOffset, fragmentLen);
            }

            pAssoc->outstandingCount++;
            pAssoc->flightSize += fragmentLen;

            // Build and send DATA chunk with manual flags
            offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);

            UINT32 chunkLen = SCTP_DATA_HEADER_SIZE + fragmentLen;
            PBYTE pChunk = pAssoc->outPacket + offset;
            pChunk[0] = SCTP_CHUNK_DATA;
            pChunk[1] = fragFlags;
            putUnalignedInt16BigEndian(pChunk + 2, (INT16) chunkLen);
            putUnalignedInt32BigEndian(pChunk + 4, (INT32) tsn);
            putUnalignedInt16BigEndian(pChunk + 8, (INT16) streamId);
            putUnalignedInt16BigEndian(pChunk + 10, (INT16) ssn);
            putUnalignedInt32BigEndian(pChunk + 12, (INT32) ppid);
            MEMCPY(pChunk + SCTP_DATA_HEADER_SIZE, pPayload + fragmentOffset, fragmentLen);
            offset += ((chunkLen + 3) & ~3u);

            sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

            fragmentOffset += fragmentLen;
            remaining -= fragmentLen;
            isFirst = FALSE;
        }

        // Start T3-rtx if not running
        if (pAssoc->t3RtxExpiry == 0) {
            pAssoc->t3RtxExpiry = nowMs + pAssoc->rtoMs;
        }
    }

    return STATUS_SUCCESS;
}

STATUS sctpAssocCheckTimers(PSctpAssociation pAssoc, UINT64 nowMs, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 offset, i;

    // T1-init timer (INIT / COOKIE-ECHO retransmit)
    if (pAssoc->t1InitExpiry != 0 && nowMs >= pAssoc->t1InitExpiry) {
        pAssoc->initRetransmitCount++;
        if (pAssoc->initRetransmitCount > SCTP_MAX_INIT_RETRANS) {
            DLOGW("SCTP: Max INIT retransmissions exceeded");
            pAssoc->state = SCTP_ASSOC_CLOSED;
            pAssoc->t1InitExpiry = 0;
            return STATUS_SUCCESS;
        }

        // Double RTO (exponential backoff)
        pAssoc->rtoMs = pAssoc->rtoMs * 2;
        if (pAssoc->rtoMs > SCTP_RTO_MAX_MS) {
            pAssoc->rtoMs = SCTP_RTO_MAX_MS;
        }

        if (pAssoc->state == SCTP_ASSOC_COOKIE_WAIT) {
            // Retransmit INIT
            offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, 0);
            offset += sctpWriteInitChunk(pAssoc->outPacket + offset, pAssoc->myVerificationTag, SCTP_DEFAULT_ARWND, SCTP_MAX_STREAMS,
                                         SCTP_MAX_STREAMS, pAssoc->nextTsn);
            sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);
            DLOGD("SCTP: Retransmitting INIT (%u)", pAssoc->initRetransmitCount);
        } else if (pAssoc->state == SCTP_ASSOC_COOKIE_ECHOED && pAssoc->cookieEchoValid) {
            // Retransmit COOKIE-ECHO
            offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
            offset += sctpWriteCookieEchoChunk(pAssoc->outPacket + offset, pAssoc->cookieEchoData, SCTP_COOKIE_SIZE);
            sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);
            DLOGD("SCTP: Retransmitting COOKIE-ECHO (%u)", pAssoc->initRetransmitCount);
        }

        pAssoc->t1InitExpiry = nowMs + pAssoc->rtoMs;
    }

    // T3-rtx timer (DATA retransmit)
    if (pAssoc->t3RtxExpiry != 0 && nowMs >= pAssoc->t3RtxExpiry) {
        // RFC 9260: on T3-rtx timeout
        pAssoc->ssthresh = (pAssoc->cwnd / 2 > 2 * pAssoc->mtu) ? pAssoc->cwnd / 2 : 2 * pAssoc->mtu;
        pAssoc->cwnd = pAssoc->mtu;

        // Double RTO
        pAssoc->rtoMs = pAssoc->rtoMs * 2;
        if (pAssoc->rtoMs > SCTP_RTO_MAX_MS) {
            pAssoc->rtoMs = SCTP_RTO_MAX_MS;
        }

        // Retransmit un-acked DATA chunks or abandon them (PR-SCTP)
        for (i = 0; i < SCTP_MAX_OUTSTANDING; i++) {
            if (!pAssoc->outstanding[i].inUse || pAssoc->outstanding[i].acked || pAssoc->outstanding[i].abandoned) {
                continue;
            }

            pAssoc->outstanding[i].retransmitCount++;

            // PR-SCTP: check if should be abandoned
            BOOL shouldAbandon = FALSE;
            if (pAssoc->outstanding[i].maxRetransmits != 0xFFFF && pAssoc->outstanding[i].retransmitCount > pAssoc->outstanding[i].maxRetransmits) {
                shouldAbandon = TRUE;
            }
            if (pAssoc->outstanding[i].lifetimeMs > 0 && nowMs - pAssoc->outstanding[i].creationTime > pAssoc->outstanding[i].lifetimeMs) {
                shouldAbandon = TRUE;
            }

            if (shouldAbandon) {
                pAssoc->outstanding[i].abandoned = TRUE;
                pAssoc->flightSize -= pAssoc->outstanding[i].payloadLen;

                // Advance Advanced.Peer.Ack.Point
                if (TSN_GT(pAssoc->outstanding[i].tsn, pAssoc->advancedPeerAckPoint)) {
                    pAssoc->advancedPeerAckPoint = pAssoc->outstanding[i].tsn;
                }

                if (pAssoc->outstanding[i].payload != NULL) {
                    MEMFREE(pAssoc->outstanding[i].payload);
                    pAssoc->outstanding[i].payload = NULL;
                }
                pAssoc->outstanding[i].inUse = FALSE;
                pAssoc->outstandingCount--;
                continue;
            }

            // Retransmit
            if (pAssoc->outstanding[i].payload != NULL) {
                offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
                offset += sctpWriteDataChunk(pAssoc->outPacket + offset, pAssoc->outstanding[i].tsn, pAssoc->outstanding[i].streamId,
                                             pAssoc->outstanding[i].ssn, pAssoc->outstanding[i].ppid, pAssoc->outstanding[i].unordered,
                                             pAssoc->outstanding[i].payload, pAssoc->outstanding[i].payloadLen);
                sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);
            }
        }

        // Send FORWARD-TSN if any chunks were abandoned
        if (TSN_GT(pAssoc->advancedPeerAckPoint, pAssoc->cumulativeAckTsn)) {
            offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
            offset += sctpWriteForwardTsnChunk(pAssoc->outPacket + offset, pAssoc->advancedPeerAckPoint);
            sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);
        }

        // Restart T3 if still have outstanding
        if (pAssoc->outstandingCount > 0) {
            pAssoc->t3RtxExpiry = nowMs + pAssoc->rtoMs;
        } else {
            pAssoc->t3RtxExpiry = 0;
        }
    }

    return STATUS_SUCCESS;
}

STATUS sctpAssocShutdown(PSctpAssociation pAssoc, SctpAssocOutboundPacketFn outboundFn, UINT64 outboundCustomData)
{
    UINT32 offset;

    if (pAssoc->state != SCTP_ASSOC_ESTABLISHED) {
        return STATUS_SUCCESS;
    }

    offset = sctpWriteCommonHeader(pAssoc->outPacket, pAssoc->localPort, pAssoc->remotePort, pAssoc->peerVerificationTag);
    offset += sctpWriteShutdownChunk(pAssoc->outPacket + offset, pAssoc->peerCumulativeTsnValid ? pAssoc->peerCumulativeTsn : 0);
    sctpSendPacket(pAssoc, offset, outboundFn, outboundCustomData);

    pAssoc->state = SCTP_ASSOC_SHUTDOWN_SENT;
    DLOGD("SCTP: Sent SHUTDOWN");

    return STATUS_SUCCESS;
}

VOID sctpAssocCleanup(PSctpAssociation pAssoc)
{
    UINT32 i;
    for (i = 0; i < SCTP_MAX_OUTSTANDING; i++) {
        if (pAssoc->outstanding[i].inUse && pAssoc->outstanding[i].payload != NULL) {
            MEMFREE(pAssoc->outstanding[i].payload);
            pAssoc->outstanding[i].payload = NULL;
        }
        pAssoc->outstanding[i].inUse = FALSE;
    }
    pAssoc->outstandingCount = 0;
    pAssoc->flightSize = 0;

    for (i = 0; i < SCTP_MAX_PENDING_SENDS; i++) {
        if (pAssoc->pendingQueue[i].inUse && pAssoc->pendingQueue[i].payload != NULL) {
            MEMFREE(pAssoc->pendingQueue[i].payload);
            pAssoc->pendingQueue[i].payload = NULL;
        }
        pAssoc->pendingQueue[i].inUse = FALSE;
    }
    pAssoc->pendingQueueCount = 0;

    if (pAssoc->reassemblyBuf != NULL) {
        MEMFREE(pAssoc->reassemblyBuf);
        pAssoc->reassemblyBuf = NULL;
    }
    pAssoc->reassemblyInProgress = FALSE;
}
