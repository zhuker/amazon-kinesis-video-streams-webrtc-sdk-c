/**
 * WHIP Client - WebRTC HTTP Ingestion Protocol (RFC 9725)
 * Streams webcam video to WHIP server
 */

let pc = null;
let localStream = null;
let sessionUrl = null;

async function start() {
    const statusEl = document.getElementById('status');
    statusEl.textContent = 'Starting...';

    try {
        // 1. Get webcam access
        console.log('[WHIP] Requesting webcam access...');
        localStream = await navigator.mediaDevices.getUserMedia({
            video: {
                width: { ideal: 1280 }, 
                height: { ideal: 720 },
                frameRate: { ideal: 25 }
            },
            audio: true
        });

        // Show local preview
        document.getElementById('localVideo').srcObject = localStream;
        statusEl.textContent = 'Got webcam access';
        console.log('[WHIP] Webcam access granted');

        // 2. Fetch ICE servers configuration
        const iceResponse = await fetch('/ice-servers');
        const iceServers = await iceResponse.json();
        console.log('[WHIP] Using ICE servers:', iceServers);

        // 3. Create peer connection
        const config = {
            sdpSemantics: 'unified-plan',
            iceServers: iceServers
        };
        pc = new RTCPeerConnection(config);

        // Setup event handlers
        pc.oniceconnectionstatechange = () => {
            document.getElementById('ice-state').textContent = pc.iceConnectionState;
            console.log('[WHIP] ICE state:', pc.iceConnectionState);
        };

        pc.onconnectionstatechange = () => {
            document.getElementById('connection-state').textContent = pc.connectionState;
            console.log('[WHIP] Connection state:', pc.connectionState);

            if (pc.connectionState === 'connected') {
                statusEl.textContent = 'Connected - streaming to server';
            } else if (pc.connectionState === 'failed') {
                statusEl.textContent = 'Connection failed';
            } else if (pc.connectionState === 'disconnected') {
                statusEl.textContent = 'Disconnected';
            }
        };

        pc.onicegatheringstatechange = () => {
            console.log('[WHIP] ICE gathering state:', pc.iceGatheringState);
        };

        // 4. Add tracks with SENDONLY direction (KEY for WHIP)
        localStream.getTracks().forEach(track => {
            pc.addTrack(track, localStream);
            console.log('[WHIP] Added track:', track.kind);
        });

        // Modify transceivers to sendonly (WHIP requirement)
        pc.getTransceivers().forEach(transceiver => {
            transceiver.direction = 'sendonly';
            console.log('[WHIP] Set transceiver direction to sendonly:', transceiver.mid);
        });

        statusEl.textContent = 'Creating offer...';

        // 5. Create SDP offer
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        console.log('[WHIP] Local description set');

        // 6. Wait for ICE gathering to complete
        statusEl.textContent = 'Gathering ICE candidates...';
        await waitForIceGathering(pc);
        console.log('[WHIP] ICE gathering complete');

        // 7. Send offer to WHIP endpoint (POST with application/sdp)
        const finalOffer = pc.localDescription;
        console.log('[WHIP] Sending SDP offer to /whip/endpoint');
        console.log('[WHIP] SDP preview:', finalOffer.sdp.substring(0, 200) + '...');

        statusEl.textContent = 'Sending offer to server...';

        const response = await fetch('/whip/endpoint', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/sdp'
            },
            body: finalOffer.sdp  // Raw SDP, not JSON (per RFC 9725)
        });

        if (response.status !== 201) {
            const errorText = await response.text();
            throw new Error(`WHIP endpoint returned ${response.status}: ${errorText}`);
        }

        // 8. Get session URL from Location header
        sessionUrl = response.headers.get('Location');
        console.log('[WHIP] Session URL:', sessionUrl);
        document.getElementById('session-url').textContent = sessionUrl || 'N/A';

        // 9. Set remote description (SDP answer)
        const answerSdp = await response.text();
        console.log('[WHIP] Received SDP answer');
        console.log('[WHIP] Answer preview:', answerSdp.substring(0, 200) + '...');

        await pc.setRemoteDescription({
            type: 'answer',
            sdp: answerSdp
        });

        statusEl.textContent = 'Connecting...';
        document.getElementById('start').style.display = 'none';
        document.getElementById('stop').style.display = 'inline-block';

    } catch (error) {
        console.error('[WHIP] Error:', error);
        statusEl.textContent = 'Error: ' + error.message;
        stop();
    }
}

function waitForIceGathering(pc) {
    return new Promise((resolve) => {
        if (pc.iceGatheringState === 'complete') {
            resolve();
            return;
        }

        const checkState = () => {
            if (pc.iceGatheringState === 'complete') {
                pc.removeEventListener('icegatheringstatechange', checkState);
                resolve();
            }
        };
        pc.addEventListener('icegatheringstatechange', checkState);

        // Timeout fallback (5 seconds)
        setTimeout(() => {
            pc.removeEventListener('icegatheringstatechange', checkState);
            console.log('[WHIP] ICE gathering timeout, proceeding with current candidates');
            resolve();
        }, 5000);
    });
}

async function stop() {
    document.getElementById('status').textContent = 'Stopping...';

    // Send DELETE to WHIP session URL (RFC 9725 Section 3)
    if (sessionUrl) {
        try {
            console.log('[WHIP] Sending DELETE to:', sessionUrl);
            await fetch(sessionUrl, { method: 'DELETE' });
            console.log('[WHIP] Session terminated');
        } catch (e) {
            console.error('[WHIP] Error terminating session:', e);
        }
        sessionUrl = null;
    }

    // Stop local tracks
    if (localStream) {
        localStream.getTracks().forEach(track => {
            track.stop();
            console.log('[WHIP] Stopped track:', track.kind);
        });
        localStream = null;
    }

    // Close peer connection
    if (pc) {
        pc.close();
        pc = null;
        console.log('[WHIP] Peer connection closed');
    }

    document.getElementById('localVideo').srcObject = null;
    document.getElementById('status').textContent = 'Stopped';
    document.getElementById('ice-state').textContent = '-';
    document.getElementById('connection-state').textContent = '-';
    document.getElementById('session-url').textContent = '-';
    document.getElementById('start').style.display = 'inline-block';
    document.getElementById('stop').style.display = 'none';
}

// Handle page unload - try to terminate session gracefully
window.addEventListener('beforeunload', () => {
    if (sessionUrl) {
        // Use sendBeacon for reliable delivery on page close
        navigator.sendBeacon(sessionUrl + '?_method=DELETE', '');
    }
});
