// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Issue #1764 item 5: the stale-data banner. Runs the real device status poll
// block of app.js (from the "Light poll of the Devices listing" comment to
// "Refresh devices") against a stub DOM, a scripted fetch and a fake clock, and
// checks what the page shows: the banner and its text, the stale marker on the
// status dots, and when the next poll goes out.
//
// Run: node --test AlpacaHTTP/tests/web/stale_banner.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const APP_JS = path.join(__dirname, '..', '..', 'web', 'app.js');
const format = require('../../web/format.js');

function makeClassList(el) {
    const names = () => el.className.split(/\s+/).filter(Boolean);
    return {
        contains: (n) => names().includes(n),
        add: (n) => { if (!names().includes(n)) el.className = names().concat(n).join(' '); },
        remove: (n) => { el.className = names().filter((x) => x !== n).join(' '); },
        toggle: (n, on) => {
            const want = on === undefined ? !names().includes(n) : !!on;
            if (want) el.className = names().filter((x) => x !== n).concat(n).join(' ');
            else el.className = names().filter((x) => x !== n).join(' ');
            return want;
        },
    };
}

function makeEl(id, className) {
    const attrs = {};
    const el = {
        id,
        className: className || '',
        hidden: false,
        textContent: '',
        title: '',
        dataset: {},
        setAttribute(name, value) { attrs[name] = String(value); },
        getAttribute(name) { return Object.prototype.hasOwnProperty.call(attrs, name) ? attrs[name] : null; },
        removeAttribute(name) { delete attrs[name]; },
    };
    el.classList = makeClassList(el);
    return el;
}

// The server: 'ok' answers one connected camera, 'refuse' rejects at once (a
// stopped server), 'hang' never answers until the request is aborted, '500' is
// a non-OK answer with a well-formed body, 'bad' is a body that is not JSON, 'alpaca-error' is an
// answer with a non-zero ErrorNumber.
function makeServer(clock) {
    const server = { mode: 'ok', failedMode: 'ok', calls: [], failedCalls: [] };
    // The /faileddevices listing: 'ok' answers no rows, 'hang' never answers
    // until its request is aborted, then answers [] as the real function does.
    server.failed = (signal) => {
        server.failedCalls.push({ at: clock.now });
        if (server.failedMode === 'ok') return Promise.resolve([]);
        return new Promise((resolve) => {
            if (signal) signal.addEventListener('abort', () => resolve([]));
        });
    };
    server.fetch = (url, opts) => {
        server.calls.push({ url, at: clock.now });
        const signal = opts && opts.signal;
        switch (server.mode) {
        case 'ok':
            return Promise.resolve({ ok: true, json: async () => ({ ErrorNumber: 0, Value: [
                { DeviceType: 'Camera', DeviceNumber: 0, Connected: true } ] }) });
        case 'refuse':
            return Promise.reject(new TypeError('Failed to fetch'));
        case 'hang':
            return new Promise((resolve, reject) => {
                if (signal) signal.addEventListener('abort', () => reject(new Error('aborted')));
            });
        case '500':
            // A well-formed body, so only the HTTP status makes it a failure.
            return Promise.resolve({ ok: false, status: 500, json: async () => ({ ErrorNumber: 0, Value: [] }) });
        case 'bad':
            return Promise.resolve({ ok: true, json: async () => { throw new SyntaxError('Unexpected token'); } });
        case 'alpaca-error':
            // Value is an array, so only the ErrorNumber makes it a failure.
            return Promise.resolve({ ok: true, json: async () => ({ ErrorNumber: 1280, ErrorMessage: 'x', Value: [] }) });
        default:
            throw new Error('unknown server mode ' + server.mode);
        }
    };
    return server;
}

const flush = async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); };

function load({ hidden = false, devicesTabActive = true } = {}) {
    const src = fs.readFileSync(APP_JS, 'utf8');
    const start = src.indexOf('// Light poll of the Devices listing');
    const end = src.indexOf('// Refresh devices');
    assert.ok(start >= 0 && end > start, 'app.js no longer has the status poll block between its comments');

    const clock = { now: 0 };
    const timers = [];
    let nextId = 1;
    const schedule = (fn, ms, every) => {
        const id = nextId++;
        timers.push({ id, fn, at: clock.now + Math.max(0, ms || 0), every });
        return id;
    };
    const cancel = (id) => {
        const i = timers.findIndex((t) => t.id === id);
        if (i >= 0) timers.splice(i, 1);
    };

    const banner = makeEl('link-banner', 'link-banner');
    banner.hidden = true;
    const devicesTab = makeEl('devices-tab', devicesTabActive ? 'tab-content active' : 'tab-content');
    const dot = makeEl('', 'status-dot status-connected');
    dot.dataset.deviceType = 'Camera';
    dot.dataset.deviceNumber = '0';
    dot.title = 'Client connected';
    dot.setAttribute('aria-label', 'Client connected');
    // A row the listing no longer carries: only the stale marker's own restore reaches it.
    const goneDot = makeEl('', 'status-dot status-idle');
    goneDot.dataset.deviceType = 'Focuser';
    goneDot.dataset.deviceNumber = '1';
    goneDot.title = 'Idle, no client connected';
    goneDot.setAttribute('aria-label', 'Idle, no client connected');
    const byId = { 'link-banner': banner, 'devices-tab': devicesTab };
    const document = {
        hidden,
        getElementById: (id) => byId[id] || null,
        querySelectorAll: (sel) => {
            assert.strictEqual(sel, '.status-dot');
            return [dot, goneDot];
        },
    };

    const server = makeServer(clock);
    const ctx = {
        document,
        console,
        API_BASE: '',
        fetch: server.fetch,
        // The real fetchFailedDevices answers [] on any error, an abort included.
        fetchFailedDevices: (signal) => server.failed(signal),
        deviceStatus: format.deviceStatus,
        linkStatus: format.linkStatus,
        STATUS_POLL_MS: format.STATUS_POLL_MS,
        STATUS_POLL_TIMEOUT_MS: format.STATUS_POLL_TIMEOUT_MS,
        AbortController,
        Date: { now: () => clock.now },
        setTimeout: (fn, ms) => schedule(fn, ms, null),
        clearTimeout: cancel,
        setInterval: (fn, ms) => schedule(fn, ms, ms),
        clearInterval: cancel,
    };
    vm.createContext(ctx);
    vm.runInContext(src.slice(start, end) + '\n;globalThis.__poll = { startDeviceStatusPoll };', ctx);

    // Run every timer due up to clock.now + ms, in time order, letting each
    // fetch settle before the next timer fires.
    async function advance(ms) {
        const target = clock.now + ms;
        for (let runs = 0; ; runs++) {
            assert.ok(runs < 10000, 'timers keep firing at the same instant (a zero or undefined delay?)');
            timers.sort((a, b) => a.at - b.at || a.id - b.id);
            const t = timers[0];
            if (!t || t.at > target) break;
            clock.now = t.at;
            if (t.every) t.at += t.every; else timers.shift();
            t.fn();
            await flush();
        }
        clock.now = target;
        await flush();
    }

    return { poll: ctx.__poll, clock, server, banner, dot, goneDot, document, devicesTab, advance };
}

// Advance in 100 ms steps until the banner shows; the time it took, or null.
async function msUntilBanner(t, limitMs) {
    for (let waited = 0; waited <= limitMs; waited += 100) {
        if (!t.banner.hidden) return waited;
        await t.advance(100);
    }
    return null;
}

test('a stopped server shows the banner within 10 s, with the age of the last data (#1764)', async () => {
    const t = load();
    t.poll.startDeviceStatusPoll();
    await t.advance(5000);  // one good poll at 5 s
    assert.strictEqual(t.server.calls.length, 1);
    assert.strictEqual(t.banner.hidden, true, 'the banner shows while the bridge answers');
    await t.advance(1);     // the server stops just after a good poll: the worst case
    t.server.mode = 'refuse';
    const waited = await msUntilBanner(t, 10000);
    assert.ok(waited !== null && waited <= 10000, `banner after ${waited} ms`);
    assert.match(t.banner.textContent, /^Connection to the bridge lost, retrying\. Last update \d+ s ago\.$/);
});

test('a hung poll is aborted after 4 s and still shows the banner within 10 s (#1764)', async () => {
    const t = load();
    t.poll.startDeviceStatusPoll();
    await t.advance(5000);
    await t.advance(1);
    t.server.mode = 'hang';
    const waited = await msUntilBanner(t, 10000);
    assert.ok(waited !== null && waited <= 10000, `banner after ${waited} ms`);
    // The aborted request no longer holds the in-flight guard: the bridge comes
    // back and the next poll goes out and clears the banner.
    t.server.mode = 'ok';
    const before = t.server.calls.length;
    await t.advance(5000);
    assert.ok(t.server.calls.length > before, 'no poll went out after the hung one');
    assert.strictEqual(t.banner.hidden, true);
});

test('a hung failed-devices listing is aborted and the poll goes on (#1764)', async () => {
    const t = load();
    t.poll.startDeviceStatusPoll();
    await t.advance(5000);
    t.server.failedMode = 'hang';  // the first listing answers, the second never does
    await t.advance(5000);
    assert.strictEqual(t.server.failedCalls.length, 2);
    // Each poll now waits out the 4 s abort, then schedules the next 5 s later: 19 s and 28 s.
    const before = t.server.calls.length;
    await t.advance(20000);
    assert.ok(t.server.calls.length >= before + 2, 'a hung /faileddevices held every later poll');
    // The bridge then stops: the next listing fails and the banner shows within 10 s.
    t.server.mode = 'refuse';
    const waited = await msUntilBanner(t, 10000);
    assert.ok(waited !== null && waited <= 10000, `banner after ${waited} ms`);
});

for (const mode of ['500', 'bad', 'alpaca-error']) {
    test(`a ${mode} answer counts as a failed poll (#1764)`, async () => {
        const t = load();
        t.poll.startDeviceStatusPoll();
        await t.advance(5000);
        t.server.mode = mode;
        await t.advance(5000);
        assert.strictEqual(t.banner.hidden, false, `no banner after a ${mode} answer`);
    });
}

test('a failed poll keeps the dot and marks it stale; recovery clears the marker and the banner (#1764)', async () => {
    const t = load();
    t.poll.startDeviceStatusPoll();
    await t.advance(5000);
    assert.ok(t.dot.classList.contains('status-connected'));
    t.server.mode = 'refuse';
    await t.advance(5000);
    assert.ok(t.dot.classList.contains('status-connected'), 'the dot lost its last state');
    assert.ok(t.dot.classList.contains('stale'), 'the dot is not marked stale');
    assert.match(t.dot.getAttribute('aria-label'), /^Stale, the bridge is not answering\. Last known: Client connected$/);
    assert.strictEqual(t.dot.title, t.dot.getAttribute('aria-label'));
    await t.advance(5000);  // a second failure does not stack the prefix
    assert.match(t.dot.getAttribute('aria-label'), /^Stale, the bridge is not answering\. Last known: Client connected$/);
    t.server.mode = 'ok';
    await t.advance(10000);
    assert.strictEqual(t.banner.hidden, true);
    assert.ok(!t.dot.classList.contains('stale'), 'the stale marker survived recovery');
    assert.strictEqual(t.dot.getAttribute('aria-label'), 'Client connected');
    assert.ok(!t.goneDot.classList.contains('stale'), 'a row the listing does not carry stayed stale');
    assert.strictEqual(t.goneDot.getAttribute('aria-label'), 'Idle, no client connected');
    assert.strictEqual(t.goneDot.title, 'Idle, no client connected');
});

test('failed polls back off 5, 5, 10 and 15 s, and a good one resets to 5 s (#1764)', async () => {
    const t = load();
    t.poll.startDeviceStatusPoll();
    await t.advance(5000);
    t.server.mode = 'refuse';
    await t.advance(60000);
    const at = t.server.calls.map((c) => c.at);
    const gaps = at.slice(1).map((v, i) => v - at[i]);
    // First gap: good poll to first failure (5 s). Then 5, 5, 10, 15, 15 ...
    assert.deepStrictEqual(gaps.slice(0, 6), [5000, 5000, 5000, 10000, 15000, 15000]);
    t.server.mode = 'ok';
    const last = at[at.length - 1];
    await t.advance(last + 15000 - t.clock.now);  // the next poll succeeds
    const good = t.server.calls[t.server.calls.length - 1].at;
    await t.advance(5000);
    assert.strictEqual(t.server.calls[t.server.calls.length - 1].at, good + 5000, 'no 5 s poll after recovery');
});

test('the poll and the banner run on every tab while the page is visible (#1764)', async () => {
    const t = load({ devicesTabActive: false });
    t.poll.startDeviceStatusPoll();
    await t.advance(5000);
    assert.strictEqual(t.server.calls.length, 1, 'no poll while another tab is shown');
    t.server.mode = 'refuse';
    await t.advance(5000);
    assert.strictEqual(t.banner.hidden, false);
});

test('a hidden page does not poll (#1764)', async () => {
    const t = load({ hidden: true });
    t.poll.startDeviceStatusPoll();
    await t.advance(30000);
    assert.strictEqual(t.server.calls.length, 0);
    assert.strictEqual(t.banner.hidden, true);
});
