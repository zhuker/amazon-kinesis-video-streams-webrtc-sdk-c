// DataChannel browser test helpers
// Puppeteer calls these via page.evaluate()

(function() {
    'use strict';

    let pc = null;
    const channels = {};       // name -> RTCDataChannel (browser-created)
    const remoteChannels = {}; // name -> RTCDataChannel (server-created)
    const messageQueues = {};  // name -> [{data, isBinary}]
    const openPromises = {};   // name -> {resolve, reject, promise}
    const remoteChannelPromises = {}; // name -> {resolve, reject, promise}

    function logMsg(msg) {
        const el = document.getElementById('log');
        if (el) el.textContent += msg + '\n';
        console.log('[DC-TEST]', msg);
    }

    function makeOpenPromise(name) {
        if (openPromises[name]) return openPromises[name].promise;
        let resolve, reject;
        const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
        openPromises[name] = { resolve, reject, promise };
        return promise;
    }

    function makeRemoteChannelPromise(name) {
        if (remoteChannelPromises[name]) return remoteChannelPromises[name].promise;
        let resolve, reject;
        const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
        remoteChannelPromises[name] = { resolve, reject, promise };
        return promise;
    }

    function setupChannelHandlers(ch, name) {
        if (!messageQueues[name]) messageQueues[name] = [];

        ch.onopen = () => {
            logMsg('channel opened: ' + name);
            if (openPromises[name]) openPromises[name].resolve();
        };

        ch.onclose = () => {
            logMsg('channel closed: ' + name);
        };

        ch.onerror = (e) => {
            logMsg('channel error: ' + name + ' ' + (e.message || e));
        };

        ch.onmessage = (evt) => {
            const isBinary = evt.data instanceof ArrayBuffer || evt.data instanceof Blob;
            if (isBinary && evt.data instanceof Blob) {
                evt.data.arrayBuffer().then(ab => {
                    messageQueues[name].push({ data: new Uint8Array(ab), isBinary: true });
                });
            } else if (isBinary) {
                messageQueues[name].push({ data: new Uint8Array(evt.data), isBinary: true });
            } else {
                messageQueues[name].push({ data: evt.data, isBinary: false });
            }
        };

        // If channel is already open, resolve immediately
        if (ch.readyState === 'open') {
            logMsg('channel already open: ' + name);
            if (openPromises[name]) openPromises[name].resolve();
        }
    }

    async function connect(testName, browserChannelConfigs) {
        browserChannelConfigs = browserChannelConfigs || [];

        pc = new RTCPeerConnection();

        // Handle server-created channels
        pc.ondatachannel = (evt) => {
            const ch = evt.channel;
            const name = ch.label;
            logMsg('remote channel received: ' + name);
            remoteChannels[name] = ch;
            makeOpenPromise(name);
            setupChannelHandlers(ch, name);
            if (remoteChannelPromises[name]) {
                remoteChannelPromises[name].resolve(name);
            }
        };

        // Create browser-side channels
        for (const cfg of browserChannelConfigs) {
            const opts = {};
            if (cfg.ordered !== undefined) opts.ordered = cfg.ordered;
            if (cfg.maxRetransmits !== undefined) opts.maxRetransmits = cfg.maxRetransmits;
            if (cfg.maxPacketLifeTime !== undefined) opts.maxPacketLifeTime = cfg.maxPacketLifeTime;
            if (cfg.protocol !== undefined) opts.protocol = cfg.protocol;

            const ch = pc.createDataChannel(cfg.name, opts);
            channels[cfg.name] = ch;
            makeOpenPromise(cfg.name);
            setupChannelHandlers(ch, cfg.name);
        }

        // If no browser channels, create a dummy for the SCTP m-line
        if (browserChannelConfigs.length === 0) {
            const ch = pc.createDataChannel('_control');
            channels['_control'] = ch;
            makeOpenPromise('_control');
            setupChannelHandlers(ch, '_control');
        }

        // Create offer and wait for ICE gathering
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);

        await new Promise((resolve) => {
            if (pc.iceGatheringState === 'complete') {
                resolve();
            } else {
                const check = () => {
                    if (pc.iceGatheringState === 'complete') {
                        pc.removeEventListener('icegatheringstatechange', check);
                        resolve();
                    }
                };
                pc.addEventListener('icegatheringstatechange', check);
            }
        });

        // Send offer to server
        const resp = await fetch('/offer?test=' + encodeURIComponent(testName || 'echo'), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                sdp: pc.localDescription.sdp,
                type: pc.localDescription.type
            })
        });

        if (!resp.ok) {
            throw new Error('Signaling failed: ' + resp.status + ' ' + (await resp.text()));
        }

        const answer = await resp.json();
        await pc.setRemoteDescription(answer);
        logMsg('connected, test=' + testName);
    }

    async function disconnect() {
        if (pc) {
            pc.close();
            pc = null;
        }
        // Clear state
        for (const k in channels) delete channels[k];
        for (const k in remoteChannels) delete remoteChannels[k];
        for (const k in messageQueues) delete messageQueues[k];
        for (const k in openPromises) delete openPromises[k];
        for (const k in remoteChannelPromises) delete remoteChannelPromises[k];

        try {
            await fetch('/reset', { method: 'POST' });
        } catch (e) {
            // ignore
        }
    }

    async function waitForOpen(channelName, timeoutMs) {
        timeoutMs = timeoutMs || 15000;
        makeOpenPromise(channelName);
        const ch = channels[channelName] || remoteChannels[channelName];
        if (ch && ch.readyState === 'open') return;

        return Promise.race([
            openPromises[channelName].promise,
            new Promise((_, reject) =>
                setTimeout(() => reject(new Error('waitForOpen timeout: ' + channelName)), timeoutMs)
            )
        ]);
    }

    async function waitForRemoteChannel(name, timeoutMs) {
        timeoutMs = timeoutMs || 15000;
        if (remoteChannels[name]) return name;
        makeRemoteChannelPromise(name);
        return Promise.race([
            remoteChannelPromises[name].promise,
            new Promise((_, reject) =>
                setTimeout(() => reject(new Error('waitForRemoteChannel timeout: ' + name)), timeoutMs)
            )
        ]);
    }

    function sendText(channelName, message) {
        const ch = channels[channelName] || remoteChannels[channelName];
        if (!ch) throw new Error('No channel: ' + channelName);
        if (ch.readyState !== 'open') throw new Error('Channel not open: ' + channelName);
        ch.send(message);
    }

    function sendBinary(channelName, uint8Array) {
        const ch = channels[channelName] || remoteChannels[channelName];
        if (!ch) throw new Error('No channel: ' + channelName);
        if (ch.readyState !== 'open') throw new Error('Channel not open: ' + channelName);
        ch.send(uint8Array.buffer);
    }

    async function collectMessages(channelName, count, timeoutMs) {
        timeoutMs = timeoutMs || 15000;
        if (!messageQueues[channelName]) messageQueues[channelName] = [];
        const queue = messageQueues[channelName];

        const deadline = Date.now() + timeoutMs;
        while (queue.length < count && Date.now() < deadline) {
            await new Promise(r => setTimeout(r, 50));
        }

        return queue.splice(0, count);
    }

    function getChannelState(channelName) {
        const ch = channels[channelName] || remoteChannels[channelName];
        if (!ch) return null;
        return ch.readyState;
    }

    function getChannelProperties(channelName) {
        const ch = channels[channelName] || remoteChannels[channelName];
        if (!ch) return null;
        return {
            label: ch.label,
            ordered: ch.ordered,
            maxRetransmits: ch.maxRetransmits,
            maxPacketLifeTime: ch.maxPacketLifeTime,
            protocol: ch.protocol,
            readyState: ch.readyState,
            id: ch.id,
            binaryType: ch.binaryType
        };
    }

    function getAllChannelNames() {
        const names = new Set([...Object.keys(channels), ...Object.keys(remoteChannels)]);
        return Array.from(names);
    }

    function getMessageCount(channelName) {
        return (messageQueues[channelName] || []).length;
    }

    function clearMessages(channelName) {
        if (messageQueues[channelName]) messageQueues[channelName] = [];
    }

    async function waitForClose(channelName, timeoutMs) {
        timeoutMs = timeoutMs || 15000;
        const ch = channels[channelName] || remoteChannels[channelName];
        if (!ch) throw new Error('No channel: ' + channelName);
        if (ch.readyState === 'closed') return;

        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('waitForClose timeout: ' + channelName)), timeoutMs);
            const origClose = ch.onclose;
            ch.onclose = () => {
                clearTimeout(timer);
                if (origClose) origClose();
                resolve();
            };
        });
    }

    function getSctpMaxMessageSize() {
        if (!pc || !pc.sctp) return null;
        return pc.sctp.maxMessageSize;
    }

    // Expose to Puppeteer
    window.dcTest = {
        connect,
        disconnect,
        waitForOpen,
        waitForRemoteChannel,
        sendText,
        sendBinary,
        collectMessages,
        getChannelState,
        getChannelProperties,
        getAllChannelNames,
        getMessageCount,
        clearMessages,
        waitForClose,
        getSctpMaxMessageSize,
        log: logMsg
    };
})();
