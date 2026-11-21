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

// Issue #856: the Client adapter select. The option list is a pure helper in
// format.js; the select's wiring runs the real WiFi block of app.js (from
// WIFI_BASE to the end of the file) against a stub DOM, as wifi-confirm.test.js does.

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const WEB = path.join(__dirname, '..', '..', 'web');
const { wifiAdapterOptions } = require(path.join(WEB, 'format.js'));

const DUAL = {
    Device: 'wlan0',
    PreferredMac: '',
    Adapters: [
        { Interface: 'ap0', Mac: 'aa:aa:aa:aa:aa:01', IsAp: true },
        { Interface: 'wlan0', Mac: 'aa:aa:aa:aa:aa:01', IsAp: false },
        { Interface: 'wlan1', Mac: 'bb:bb:bb:bb:bb:02', IsAp: false },
        { Interface: 'p2p0', Mac: '', IsAp: false },
    ],
};

test('wifiAdapterOptions lists each adapter by MAC, marks the hotspot and the one in use, and offers Automatic (#856)',
     () => {
         const { options, selected } = wifiAdapterOptions(DUAL);
         assert.deepStrictEqual(options, [
             { value: '', label: 'Automatic', disabled: false },
             { value: 'none:ap0', label: 'ap0 (aa:aa:aa:aa:aa:01), hotspot', disabled: true },
             { value: 'aa:aa:aa:aa:aa:01', label: 'wlan0 (aa:aa:aa:aa:aa:01), in use', disabled: false },
             { value: 'bb:bb:bb:bb:bb:02', label: 'wlan1 (bb:bb:bb:bb:bb:02)', disabled: false },
             { value: 'none:p2p0', label: 'p2p0 (no MAC)', disabled: true },
         ]);
         assert.strictEqual(selected, '');
         assert.strictEqual(wifiAdapterOptions({ ...DUAL, PreferredMac: 'bb:bb:bb:bb:bb:02' }).selected,
                            'bb:bb:bb:bb:bb:02');
         // A stored MAC no listed adapter reports (an unplugged dongle) shows as Automatic.
         assert.strictEqual(wifiAdapterOptions({ ...DUAL, PreferredMac: 'cc:cc:cc:cc:cc:03' }).selected, '');
         // A single-radio board shows its one radio, in use.
         const single = wifiAdapterOptions({ Device: 'wlan0', Adapters: [{ Interface: 'wlan0', Mac: 'aa:aa:aa:aa:aa:01' }] });
         assert.strictEqual(single.options[1].label, 'wlan0 (aa:aa:aa:aa:aa:01), in use');
         assert.deepStrictEqual(wifiAdapterOptions(undefined).options, [{ value: '', label: 'Automatic', disabled: false }]);
     });

function makeEl(props) {
    const handlers = {};
    return Object.assign({
        value: '', style: {}, dataset: {}, handlers, children: [],
        classList: { toggle() {}, contains() { return false; }, remove() {} },
        addEventListener(name, fn) { handlers[name] = fn; },
        appendChild(child) { this.children.push(child); },
    }, props);
}

function setup(confirmAnswer) {
    const src = fs.readFileSync(path.join(WEB, 'app.js'), 'utf8');
    const block = src.slice(src.indexOf('const WIFI_BASE'));
    const els = { 'wifi-section': makeEl(), 'wifi-adapter': makeEl(), 'wifi-message': makeEl() };
    const requests = [];
    const prompts = [];
    const ctx = {
        document: {
            getElementById: (id) => els[id] || null,
            querySelectorAll: () => [],
            querySelector: () => null,
            createElement: () => makeEl(),
            activeElement: null,
        },
        location: { hostname: 'pi.local' },
        confirmDialog: async (opts) => { prompts.push(opts); return confirmAnswer; },
        wifiAdapterOptions,
        fetch: async (url, opts) => {
            requests.push({ url, method: opts.method, body: opts.body ? JSON.parse(opts.body) : undefined });
            return { json: async () => ({ ErrorNumber: 0, Value: {} }) };
        },
        setTimeout: () => 0,
        console,
    };
    vm.createContext(ctx);
    vm.runInContext(block + '\n;wifiRefresh = function () {}; globalThis.__render = wifiRenderAdapters; globalThis.__init = wifiInit;', ctx);
    ctx.__init();
    ctx.__render({ ...DUAL, PreferredMac: 'aa:aa:aa:aa:aa:01' });
    return { els, prompts, sent: () => requests.filter((r) => r.method === 'PUT') };
}

const flush = () => new Promise((r) => setImmediate(r));

test('changing the client adapter asks first and sends PUT /adapter with the MAC (#856)', async () => {
    const t = setup(true);
    const sel = t.els['wifi-adapter'];
    assert.strictEqual(sel.children.length, 5);
    assert.strictEqual(sel.value, 'aa:aa:aa:aa:aa:01');
    sel.value = 'bb:bb:bb:bb:bb:02';
    sel.handlers.change();
    await flush();
    assert.strictEqual(t.prompts.length, 1);
    assert.match(t.prompts[0].body, /bb:bb:bb:bb:bb:02/);
    assert.match(t.prompts[0].body, /saved network/);
    assert.deepStrictEqual(t.sent(), [{ url: '/management/v1/wifi/adapter', method: 'PUT', body: { Mac: 'bb:bb:bb:bb:bb:02' } }]);
});

test('Cancel sends nothing and puts the previous client adapter back (#856)', async () => {
    const t = setup(false);
    const sel = t.els['wifi-adapter'];
    sel.value = '';
    sel.handlers.change();
    await flush();
    assert.strictEqual(t.prompts.length, 1);
    assert.strictEqual(t.sent().length, 0);
    assert.strictEqual(sel.value, 'aa:aa:aa:aa:aa:01');
});
