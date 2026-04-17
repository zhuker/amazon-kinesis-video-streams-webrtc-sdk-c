let pc = null;
let dc = null, dcInterval = null;

function negotiate() {
    pc.addTransceiver('video', {direction: 'recvonly'});
    pc.addTransceiver('audio', {direction: 'recvonly'});
    return pc.createOffer().then((offer) => {
        return pc.setLocalDescription(offer);
    }).then(() => {
        // wait for ICE gathering to complete
        return new Promise((resolve) => {
            if (pc.iceGatheringState === 'complete') {
                resolve();
            } else {
                const checkState = () => {
                    if (pc.iceGatheringState === 'complete') {
                        pc.removeEventListener('icegatheringstatechange', checkState);
                        resolve();
                    }
                };
                pc.addEventListener('icegatheringstatechange', checkState);
            }
        });
    }).then(() => {
        const offer = pc.localDescription;
        return fetch('/offer', {
            body: JSON.stringify({
                sdp: offer.sdp,
                type: offer.type,
            }),
            headers: {
                'Content-Type': 'application/json'
            },
            method: 'POST'
        });
    }).then((response) => {
        return response.json();
    }).then((answer) => {
        return pc.setRemoteDescription(answer);
    }).catch((e) => {
        alert(e);
    });
}

async function start() {
    let config = {
        sdpSemantics: 'unified-plan'
    };

    // Fetch ICE servers from the server
    const response = await fetch('/ice-servers');
    const iceServers = await response.json();
    console.log("Using ICE servers: ", iceServers);
    config.iceServers = iceServers;

    pc = new RTCPeerConnection(config)


    const dataChannelLog = document.getElementById('data-channel'),
        iceConnectionLog = document.getElementById('ice-connection-state'),
        iceGatheringLog = document.getElementById('ice-gathering-state'),
        signalingLog = document.getElementById('signaling-state');

    pc.addEventListener('icegatheringstatechange', function () {
        iceGatheringLog.textContent += ' -> ' + pc.iceGatheringState;
    }, false);
    iceGatheringLog.textContent = pc.iceGatheringState;

    pc.addEventListener('iceconnectionstatechange', function () {
        iceConnectionLog.textContent += ' -> ' + pc.iceConnectionState;
    }, false);
    iceConnectionLog.textContent = pc.iceConnectionState;

    pc.addEventListener('signalingstatechange', function () {
        signalingLog.textContent += ' -> ' + pc.signalingState;
    }, false);
    signalingLog.textContent = pc.signalingState;

    // connect audio / video
    pc.addEventListener('track', (evt) => {
        if (evt.track.kind === 'video') {
            document.getElementById('video').srcObject = evt.streams[0];
        } else {
            document.getElementById('audio').srcObject = evt.streams[0];
        }
    });

    dc = pc.createDataChannel('chat', {});
    dc.onclose = function () {
        clearInterval(dcInterval);
        dataChannelLog.textContent += '- close\n';
    };
    dc.onopen = function () {
        dataChannelLog.textContent += '- open\n';
        dcInterval = setInterval(function () {
            const message = 'ping ' + current_stamp();
            dataChannelLog.textContent += '> ' + message + '\n';
            dc.send(message);
        }, 2000);
    };
    dc.onmessage = function (evt) {
        dataChannelLog.textContent += '< ' + evt.data + '\n';
    };

    document.getElementById('start').style.display = 'none';
    negotiate();
    document.getElementById('stop').style.display = 'inline-block';
}

function stop() {
    document.getElementById('stop').style.display = 'none';
    document.getElementById('start').style.display = 'inline-block';

    // close data channel
    if (dc) {
        dc.close();
    }

    // close peer connection
    setTimeout(() => {
        pc.close();
    }, 500);
}

function current_stamp() {
    const now = new Date();
    let h = now.getHours();
    if (h < 10) {
        h = '0' + h;
    }
    let m = now.getMinutes();
    if (m < 10) {
        m = '0' + m;
    }
    let s = now.getSeconds();
    if (s < 10) {
        s = '0' + s;
    }
    let ms = now.getMilliseconds();
    if (ms < 10) {
        ms = '00' + ms;
    } else if (ms < 100) {
        ms = '0' + ms;
    }
    return h + ':' + m + ':' + s + '.' + ms;
}
