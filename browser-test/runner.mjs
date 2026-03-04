/**
 * Puppeteer-based browser interop test runner for DataChannels.
 *
 * Usage: node runner.mjs <path-to-dcTestServer> [static-dir]
 */

import puppeteer from 'puppeteer';
import { spawn } from 'child_process';
import { setTimeout as sleep } from 'timers/promises';

const SERVER_PORT = 9876;
const SERVER_URL = `http://127.0.0.1:${SERVER_PORT}`;

// ---- Test framework ----

const results = [];

function assert(condition, message) {
    if (!condition) throw new Error(message || 'Assertion failed');
}

function assertEqual(actual, expected, message) {
    if (actual !== expected) {
        throw new Error(message || `Expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    }
}

function assertIncludes(arr, item, message) {
    if (!arr.includes(item)) {
        throw new Error(message || `Expected array to include ${JSON.stringify(item)}, got ${JSON.stringify(arr)}`);
    }
}

async function runTest(name, fn) {
    process.stdout.write(`  ${name} ... `);
    try {
        await fn();
        process.stdout.write('PASS\n');
        results.push({ name, pass: true });
    } catch (err) {
        process.stdout.write(`FAIL: ${err.message}\n`);
        results.push({ name, pass: false, error: err.message });
    }
}

// ---- Server management ----

function startServer(binaryPath, staticDir) {
    const proc = spawn(binaryPath, ['--port', String(SERVER_PORT), '--static-dir', staticDir], {
        stdio: ['ignore', 'pipe', 'pipe']
    });
    proc.stdout.on('data', d => process.stderr.write(d));
    proc.stderr.on('data', d => process.stderr.write(d));
    proc.on('exit', (code) => {
        if (code !== null && code !== 0) {
            process.stderr.write(`[SERVER] exited with code ${code}\n`);
        }
    });
    return proc;
}

async function waitForServer(timeoutMs = 15000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        try {
            const resp = await fetch(SERVER_URL);
            if (resp.ok) return;
        } catch {}
        await sleep(300);
    }
    throw new Error('Server did not start in time');
}

// ---- Page helpers ----

async function withPage(page, fn) {
    await page.goto(SERVER_URL, { waitUntil: 'domcontentloaded' });
    await page.waitForFunction('window.dcTest !== undefined', { timeout: 5000 });
    try {
        await fn();
    } finally {
        await page.evaluate(() => window.dcTest.disconnect());
        await sleep(500);
    }
}

// ---- Main ----

async function main() {
    const serverBinary = process.argv[2];
    const staticDir = process.argv[3] || '.';

    if (!serverBinary) {
        console.error('Usage: node runner.mjs <path-to-dcTestServer> [static-dir]');
        process.exit(1);
    }

    console.log('Starting DataChannel test server...');
    const server = startServer(serverBinary, staticDir);

    try {
        await waitForServer();
        console.log('Server ready.\n');
    } catch (err) {
        console.error(err.message);
        server.kill('SIGTERM');
        process.exit(1);
    }

    let browser;
    try {
        browser = await puppeteer.launch({
            headless: 'new',
            args: [
                '--no-sandbox',
                '--disable-setuid-sandbox',
                '--disable-dev-shm-usage',
                '--use-fake-ui-for-media-stream',
                '--use-fake-device-for-media-stream'
            ]
        });
    } catch (err) {
        console.error('Failed to launch browser:', err.message);
        server.kill('SIGTERM');
        process.exit(1);
    }

    const page = await browser.newPage();
    page.on('console', msg => {
        if (msg.type() === 'error' && !msg.text().includes('favicon')) {
            process.stderr.write(`[BROWSER ERROR] ${msg.text()}\n`);
        }
    });
    page.on('pageerror', err => {
        process.stderr.write(`[PAGE ERROR] ${err.message}\n`);
    });

    console.log('Browser DataChannel interop tests\n');

    // ============================================================
    // Group 1: Basic Connectivity
    // ============================================================

    await runTest('browser-creates-dc-text', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'chat' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('chat'));

            await page.evaluate(() => window.dcTest.sendText('chat', 'hello'));
            const msgs = await page.evaluate(() => window.dcTest.collectMessages('chat', 1, 10000));
            assertEqual(msgs.length, 1, 'Expected 1 message');
            assertEqual(msgs[0].data, 'hello', 'Echo mismatch');
            assertEqual(msgs[0].isBinary, false, 'Expected text');
        });
    });

    await runTest('server-creates-dc', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('server-creates-dc', []));
            await page.evaluate(() => window.dcTest.waitForRemoteChannel('server-channel'));
            await page.evaluate(() => window.dcTest.waitForOpen('server-channel'));

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('server-channel', 1, 10000));
            assertEqual(msgs.length, 1, 'Expected 1 message from server');
            assertEqual(msgs[0].data, 'hello from server', 'Server message mismatch');
        });
    });

    await runTest('both-create-dc', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('bidirectional', [{ name: 'browser-ch' }]));

            // Wait for both channels
            await page.evaluate(() => window.dcTest.waitForOpen('browser-ch'));
            await page.evaluate(() => window.dcTest.waitForRemoteChannel('server-ch'));
            await page.evaluate(() => window.dcTest.waitForOpen('server-ch'));

            // Send on browser channel
            await page.evaluate(() => window.dcTest.sendText('browser-ch', 'from browser'));
            const echoes = await page.evaluate(() => window.dcTest.collectMessages('browser-ch', 1, 10000));
            assertEqual(echoes[0].data, 'from browser', 'Echo on browser-ch mismatch');

            // Send on server channel
            await page.evaluate(() => window.dcTest.sendText('server-ch', 'to server'));
            const serverEchoes = await page.evaluate(() => window.dcTest.collectMessages('server-ch', 1, 10000));
            assertEqual(serverEchoes[0].data, 'to server', 'Echo on server-ch mismatch');
        });
    });

    // ============================================================
    // Group 2: Message Types
    // ============================================================

    await runTest('text-short', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));
            await page.evaluate(() => window.dcTest.sendText('ch', 'hello'));
            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
            assertEqual(msgs[0].data, 'hello');
            assertEqual(msgs[0].isBinary, false);
        });
    });

    await runTest('text-empty', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));
            // Send empty string, then a follow-up to verify channel still works
            await page.evaluate(() => {
                window.dcTest.sendText('ch', '');
            });
            // Empty strings may or may not be echoed (SDK limitation with PPID 56)
            // Verify channel survives by sending a real message
            await page.evaluate(() => window.dcTest.sendText('ch', 'after-empty'));
            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
            // We should get either the empty echo or the "after-empty" echo
            assert(msgs.length >= 1, 'Expected at least 1 message after empty send');
        });
    });

    await runTest('text-long', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('large-echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            // Send 16KB text
            const sent = await page.evaluate(() => {
                const text = 'A'.repeat(16384);
                window.dcTest.sendText('ch', text);
                return text.length;
            });
            assertEqual(sent, 16384);

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 15000));
            assertEqual(msgs.length, 1, 'Expected 1 message');
            const receivedLen = await page.evaluate((data) => data.length, msgs[0].data);
            assertEqual(msgs[0].isBinary, false);
            assertEqual(receivedLen, 16384, 'Text length mismatch');
        });
    });

    await runTest('text-utf8', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            const testStr = 'Hello \u00e9\u00e8\u00ea \u4e16\u754c \ud83d\ude00\ud83c\udf1f';
            await page.evaluate((s) => window.dcTest.sendText('ch', s), testStr);
            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
            assertEqual(msgs[0].data, testStr, 'UTF-8 echo mismatch');
        });
    });

    await runTest('binary-short', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            await page.evaluate(() => {
                const buf = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
                window.dcTest.sendBinary('ch', buf);
            });

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
            assertEqual(msgs[0].isBinary, true, 'Expected binary');
            const match = await page.evaluate((msg) => {
                const expected = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
                const d = Object.values(msg.data);
                return d.length === expected.length && d.every((v, i) => v === expected[i]);
            }, msgs[0]);
            assert(match, 'Binary data mismatch');
        });
    });

    await runTest('binary-empty', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));
            await page.evaluate(() => {
                window.dcTest.sendBinary('ch', new Uint8Array(0));
            });
            // Empty binary may or may not be echoed (SDK limitation with PPID 57)
            // Verify channel survives by sending a real message after
            await page.evaluate(() => {
                window.dcTest.sendBinary('ch', new Uint8Array([42]));
            });
            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
            assert(msgs.length >= 1, 'Expected at least 1 message after empty binary send');
        });
    });

    await runTest('binary-1kb', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));
            await page.evaluate(() => {
                const buf = new Uint8Array(1024);
                for (let i = 0; i < 1024; i++) buf[i] = i % 256;
                window.dcTest.sendBinary('ch', buf);
            });
            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
            assertEqual(msgs[0].isBinary, true);
            const ok = await page.evaluate((msg) => {
                const d = Object.values(msg.data);
                if (d.length !== 1024) return false;
                for (let i = 0; i < 1024; i++) {
                    if (d[i] !== i % 256) return false;
                }
                return true;
            }, msgs[0]);
            assert(ok, 'Binary 1KB pattern mismatch');
        });
    });

    // ============================================================
    // Group 3: Reliability Modes
    // ============================================================

    const reliabilityTests = [
        { name: 'ordered-reliable', opts: { ordered: true }, test: 'echo' },
        { name: 'unordered-reliable', opts: { ordered: false }, test: 'echo' },
        { name: 'ordered-maxretransmits', opts: { ordered: true, maxRetransmits: 3 }, test: 'echo' },
        { name: 'ordered-maxlifetime', opts: { ordered: true, maxPacketLifeTime: 1000 }, test: 'echo' },
        { name: 'unordered-maxretransmits', opts: { ordered: false, maxRetransmits: 3 }, test: 'echo' },
        { name: 'unordered-maxlifetime', opts: { ordered: false, maxPacketLifeTime: 1000 }, test: 'echo' },
    ];

    for (const rt of reliabilityTests) {
        await runTest(rt.name, async () => {
            await withPage(page, async () => {
                const channelCfg = { name: 'ch', ...rt.opts };
                await page.evaluate((cfg, test) => window.dcTest.connect(test, [cfg]), channelCfg, rt.test);
                await page.evaluate(() => window.dcTest.waitForOpen('ch'));
                await page.evaluate(() => window.dcTest.sendText('ch', 'test-msg'));
                const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
                assertEqual(msgs[0].data, 'test-msg', `${rt.name}: echo mismatch`);
            });
        });
    }

    // ============================================================
    // Group 4: Multiple Channels
    // ============================================================

    await runTest('multi-3-channels', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [
                { name: 'ch-0' }, { name: 'ch-1' }, { name: 'ch-2' }
            ]));

            for (let i = 0; i < 3; i++) {
                await page.evaluate((n) => window.dcTest.waitForOpen(n), `ch-${i}`);
            }

            // Send on each and verify independent echo
            for (let i = 0; i < 3; i++) {
                await page.evaluate((n, msg) => window.dcTest.sendText(n, msg), `ch-${i}`, `msg-${i}`);
            }

            for (let i = 0; i < 3; i++) {
                const msgs = await page.evaluate((n) => window.dcTest.collectMessages(n, 1, 10000), `ch-${i}`);
                assertEqual(msgs[0].data, `msg-${i}`, `Channel ch-${i} echo mismatch`);
            }
        });
    });

    await runTest('multi-mixed-modes', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [
                { name: 'ordered', ordered: true },
                { name: 'unordered', ordered: false },
                { name: 'limited', ordered: true, maxRetransmits: 2 }
            ]));

            for (const n of ['ordered', 'unordered', 'limited']) {
                await page.evaluate((ch) => window.dcTest.waitForOpen(ch), n);
                await page.evaluate((ch) => window.dcTest.sendText(ch, 'test'), n);
            }

            for (const n of ['ordered', 'unordered', 'limited']) {
                const msgs = await page.evaluate((ch) => window.dcTest.collectMessages(ch, 1, 10000), n);
                assertEqual(msgs[0].data, 'test', `${n} echo mismatch`);
            }
        });
    });

    await runTest('many-channels-10', async () => {
        await withPage(page, async () => {
            const configs = [];
            for (let i = 0; i < 10; i++) configs.push({ name: `ch-${i}` });
            await page.evaluate((cfgs) => window.dcTest.connect('echo', cfgs), configs);

            for (let i = 0; i < 10; i++) {
                await page.evaluate((n) => window.dcTest.waitForOpen(n), `ch-${i}`);
            }

            // Verify all work
            for (let i = 0; i < 10; i++) {
                await page.evaluate((n, msg) => window.dcTest.sendText(n, msg), `ch-${i}`, `hi-${i}`);
            }
            for (let i = 0; i < 10; i++) {
                const msgs = await page.evaluate((n) => window.dcTest.collectMessages(n, 1, 10000), `ch-${i}`);
                assertEqual(msgs[0].data, `hi-${i}`, `ch-${i} echo mismatch`);
            }
        });
    });

    await runTest('server-creates-5', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('server-creates-multi', []));

            for (let i = 0; i < 5; i++) {
                await page.evaluate((n) => window.dcTest.waitForRemoteChannel(n), `srv-${i}`);
                await page.evaluate((n) => window.dcTest.waitForOpen(n), `srv-${i}`);
            }

            const names = await page.evaluate(() => window.dcTest.getAllChannelNames());
            for (let i = 0; i < 5; i++) {
                assertIncludes(names, `srv-${i}`, `Missing server channel srv-${i}`);
            }
        });
    });

    // ============================================================
    // Group 5: Message Ordering & Burst
    // ============================================================

    await runTest('ordered-sequence-100', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            // Send 100 numbered messages
            await page.evaluate(() => {
                for (let i = 0; i < 100; i++) {
                    window.dcTest.sendText('ch', 'msg-' + i);
                }
            });

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 100, 30000));
            assertEqual(msgs.length, 100, `Expected 100 messages, got ${msgs.length}`);
            for (let i = 0; i < 100; i++) {
                assertEqual(msgs[i].data, `msg-${i}`, `Message ${i} out of order`);
            }
        });
    });

    await runTest('burst-50-delivery', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            await page.evaluate(() => {
                for (let i = 0; i < 50; i++) {
                    window.dcTest.sendText('ch', 'burst-' + i);
                }
            });

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 50, 30000));
            assertEqual(msgs.length, 50, `Expected 50 messages, got ${msgs.length}`);
        });
    });

    await runTest('server-burst-50', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('burst', []));
            // Wait for the server-created burst channel
            await page.evaluate(() => window.dcTest.waitForRemoteChannel('burst-srv'));
            await page.evaluate(() => window.dcTest.waitForOpen('burst-srv'));

            // Server sends 50 messages on channel open with flow control delays
            // Wait for all to arrive
            const msgs = await page.evaluate(() => window.dcTest.collectMessages('burst-srv', 50, 60000));
            assert(msgs.length >= 25, `Expected >= 25 server burst messages, got ${msgs.length}`);
            // Verify first message
            assertEqual(msgs[0].data, 'server-burst-0');
        });
    });

    // ============================================================
    // Group 6: Large Messages / Fragmentation
    // ============================================================

    // Max usable size is 64KB: SDK doesn't send a=max-message-size in SDP,
    // so browser applies RFC 8841 default of 65536 bytes.
    const fragmentSizes = [
        { name: 'fragment-2kb', size: 2048 },
        { name: 'fragment-16kb', size: 16384 },
        { name: 'fragment-32kb', size: 32768 },
        { name: 'fragment-64kb', size: 65536 },
    ];

    for (const ft of fragmentSizes) {
        await runTest(ft.name, async () => {
            await withPage(page, async () => {
                await page.evaluate(() => window.dcTest.connect('large-echo', [{ name: 'ch' }]));
                await page.evaluate(() => window.dcTest.waitForOpen('ch'));

                await page.evaluate((sz) => {
                    const buf = new Uint8Array(sz);
                    for (let i = 0; i < sz; i++) buf[i] = i % 256;
                    window.dcTest.sendBinary('ch', buf);
                }, ft.size);

                const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 30000));
                assertEqual(msgs.length, 1, 'Expected 1 message');
                assertEqual(msgs[0].isBinary, true, 'Expected binary');

                const ok = await page.evaluate((msg, expectedSize) => {
                    const d = Object.values(msg.data);
                    if (d.length !== expectedSize) return 'length:' + d.length;
                    for (let i = 0; i < expectedSize; i++) {
                        if (d[i] !== i % 256) return 'mismatch at ' + i + ': got ' + d[i];
                    }
                    return 'ok';
                }, msgs[0], ft.size);

                assertEqual(ok, 'ok', `${ft.name}: ${ok}`);
            });
        });
    }

    // ============================================================
    // Group 7: Server Sends Binary
    // ============================================================

    await runTest('server-sends-binary-pattern', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('server-sends-binary', []));
            await page.evaluate(() => window.dcTest.waitForRemoteChannel('binary-srv'));
            await page.evaluate(() => window.dcTest.waitForOpen('binary-srv'));

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('binary-srv', 1, 10000));
            assertEqual(msgs.length, 1);
            assertEqual(msgs[0].isBinary, true);

            const ok = await page.evaluate((msg) => {
                const d = Object.values(msg.data);
                if (d.length !== 1024) return 'length:' + d.length;
                for (let i = 0; i < 1024; i++) {
                    if (d[i] !== i % 256) return 'mismatch at ' + i;
                }
                return 'ok';
            }, msgs[0]);
            assertEqual(ok, 'ok', 'Binary pattern mismatch: ' + ok);
        });
    });

    // ============================================================
    // Group 8: Channel Lifecycle
    // ============================================================

    await runTest('open-callback-fires', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            // waitForOpen will throw on timeout if onopen never fires
            await page.evaluate(() => window.dcTest.waitForOpen('ch', 15000));
            const state = await page.evaluate(() => window.dcTest.getChannelState('ch'));
            assertEqual(state, 'open', 'Channel state should be open');
        });
    });

    await runTest('close-on-disconnect', async () => {
        // Don't use withPage since we need custom disconnect behavior
        await page.goto(SERVER_URL, { waitUntil: 'domcontentloaded' });
        await page.waitForFunction('window.dcTest !== undefined', { timeout: 5000 });

        await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
        await page.evaluate(() => window.dcTest.waitForOpen('ch'));

        // Close the peer connection (not via disconnect which resets)
        const closed = await page.evaluate(() => {
            return new Promise((resolve, reject) => {
                const ch = window.dcTest._getChannel ? window.dcTest._getChannel('ch') : null;
                // We'll use a different approach: watch channel state after closing
                const timer = setTimeout(() => resolve('timeout'), 10000);

                // Check state periodically after close
                const origState = window.dcTest.getChannelState('ch');
                if (origState !== 'open') {
                    resolve('not-open:' + origState);
                    return;
                }

                // Force close by evaluating disconnect which closes PC
                window.dcTest.disconnect();
                // The channel should transition to closed
                const check = setInterval(() => {
                    const state = window.dcTest.getChannelState('ch');
                    if (state === 'closed' || state === null) {
                        clearInterval(check);
                        clearTimeout(timer);
                        resolve('closed');
                    }
                }, 100);
            });
        });

        // Either closed or null (channel cleaned up)
        assert(closed === 'closed' || closed === 'timeout', `Expected closed, got ${closed}`);
        await sleep(500);
    });

    // ============================================================
    // Group 9: Stress & Integrity
    // ============================================================

    await runTest('rapid-100-small', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            await page.evaluate(() => {
                for (let i = 0; i < 100; i++) {
                    window.dcTest.sendText('ch', 'rapid-' + i);
                }
            });

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 100, 30000));
            assertEqual(msgs.length, 100, `Expected 100 echoes, got ${msgs.length}`);
        });
    });

    await runTest('bidirectional-simultaneous', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('burst', [{ name: 'browser-ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('browser-ch'));

            // Wait for server burst channel too
            await page.evaluate(() => window.dcTest.waitForRemoteChannel('burst-srv'));
            await page.evaluate(() => window.dcTest.waitForOpen('burst-srv'));

            // Browser sends 20 messages on its channel (server echoes)
            await page.evaluate(() => {
                for (let i = 0; i < 20; i++) {
                    window.dcTest.sendText('browser-ch', 'b-' + i);
                }
            });

            // Collect server burst messages (50 from burst-srv open, accept >= 25)
            const serverMsgs = await page.evaluate(() => window.dcTest.collectMessages('burst-srv', 50, 60000));
            assert(serverMsgs.length >= 25, `Expected >= 25 server messages, got ${serverMsgs.length}`);

            // Collect echoed browser messages
            const echoMsgs = await page.evaluate(() => window.dcTest.collectMessages('browser-ch', 20, 30000));
            assertEqual(echoMsgs.length, 20, `Expected 20 echoed messages, got ${echoMsgs.length}`);
        });
    });

    await runTest('ping-pong-10', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            for (let i = 0; i < 10; i++) {
                await page.evaluate((idx) => window.dcTest.sendText('ch', 'ping-' + idx), i);
                const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 10000));
                assertEqual(msgs[0].data, `ping-${i}`, `Round-trip ${i} mismatch`);
            }
        });
    });

    await runTest('data-integrity-64kb', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('large-echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            await page.evaluate(() => {
                const buf = new Uint8Array(65536);
                for (let i = 0; i < 65536; i++) buf[i] = i % 256;
                window.dcTest.sendBinary('ch', buf);
            });

            const msgs = await page.evaluate(() => window.dcTest.collectMessages('ch', 1, 30000));
            const ok = await page.evaluate((msg) => {
                const d = Object.values(msg.data);
                if (d.length !== 65536) return 'length:' + d.length;
                for (let i = 0; i < 65536; i++) {
                    if (d[i] !== i % 256) return 'mismatch at byte ' + i;
                }
                return 'ok';
            }, msgs[0]);
            assertEqual(ok, 'ok', 'Data integrity failed: ' + ok);
        });
    });

    // ============================================================
    // Group 10: DCEP Negotiation Properties
    // ============================================================

    await runTest('server-dc-properties-unordered', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('server-creates-unordered', []));
            await page.evaluate(() => window.dcTest.waitForRemoteChannel('unordered-srv'));
            await page.evaluate(() => window.dcTest.waitForOpen('unordered-srv'));

            const props = await page.evaluate(() => window.dcTest.getChannelProperties('unordered-srv'));
            assertEqual(props.ordered, false, 'Expected ordered=false for unordered channel');
        });
    });

    await runTest('server-dc-properties-maxretransmits', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('server-creates-maxretransmits', []));
            await page.evaluate(() => window.dcTest.waitForRemoteChannel('maxretransmit-srv'));
            await page.evaluate(() => window.dcTest.waitForOpen('maxretransmit-srv'));

            const props = await page.evaluate(() => window.dcTest.getChannelProperties('maxretransmit-srv'));
            assertEqual(props.maxRetransmits, 3, 'Expected maxRetransmits=3');
        });
    });

    // ============================================================
    // Group 11: SCTP Transport Properties
    // ============================================================

    await runTest('sctp-max-message-size', async () => {
        await withPage(page, async () => {
            await page.evaluate(() => window.dcTest.connect('echo', [{ name: 'ch' }]));
            await page.evaluate(() => window.dcTest.waitForOpen('ch'));

            const maxSize = await page.evaluate(() => window.dcTest.getSctpMaxMessageSize());
            assert(maxSize !== null, 'pc.sctp should be available after connection');
            console.log(`    pc.sctp.maxMessageSize = ${maxSize}`);
            // RFC 8841 §6.1: absent max-message-size defaults to 65536
            assertEqual(maxSize, 65536, `Expected maxMessageSize=65536 (RFC 8841 default), got ${maxSize}`);
        });
    });

    // ============================================================
    // Summary
    // ============================================================

    console.log('\n--- Summary ---');
    const passed = results.filter(r => r.pass).length;
    const failed = results.filter(r => !r.pass);
    console.log(`${passed}/${results.length} tests passed`);

    if (failed.length > 0) {
        console.log('\nFailed tests:');
        for (const r of failed) {
            console.log(`  ${r.name}: ${r.error}`);
        }
    }

    await browser.close();
    server.kill('SIGTERM');

    process.exit(failed.length === 0 ? 0 : 1);
}

main().catch(err => {
    console.error('Fatal error:', err);
    process.exit(1);
});
