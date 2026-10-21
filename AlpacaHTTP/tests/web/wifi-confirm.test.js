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

// wifiRefresh is stubbed out; state is seeded by hand. Runs the real WiFi block of app.js (everything from WIFI_BASE to the end
// of the file) against a stub DOM, so the confirmation wiring is tested as shipped.

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const APP_JS = process.env.WIFI_TEST_APP_JS || path.join(__dirname, '../../web/app.js');

function makeEl(props) {
    const handlers = {};
    const classes = new Set();
    return Object.assign({
        value: '', checked: false, style: {}, dataset: {}, handlers,
        classList: {
            toggle(c, on) { if (on) classes.add(c); else classes.delete(c); },
            contains(c) { return classes.has(c); },
        },
        addEventListener(name, fn) { handlers[name] = fn; },
    }, props);
}

async function setup({ apOn, confirmAnswer, host = 'pi.local', status }) {
    const src = fs.readFileSync(APP_JS, 'utf8');
    const block = src.slice(src.indexOf('const WIFI_BASE'));
    const els = {
        'wifi-section': makeEl(),
        'wifi-radio-toggle': makeEl({ checked: true }),
        'wifi-ap-toggle': makeEl({ checked: apOn }),
        'wifi-ap-ssid': makeEl({ value: 'Old Name' }),
        'wifi-ap-pass': makeEl({ value: '' }),
        'wifi-country': makeEl({ value: 'US', options: [1] }),
        'wifi-message': makeEl(),
    };
    const bands = [makeEl({ dataset: { band: 'bg' } }), makeEl({ dataset: { band: 'a' } })];
    bands[0].classList.toggle('active', true);
    const requests = [];
    const prompts = [];
    const ctx = {
        document: {
            getElementById: (id) => els[id] || null,
            querySelectorAll: () => bands,
            querySelector: (sel) => (sel.includes('.active') ? bands.find((b) => b.classList.contains('active')) : null),
            activeElement: null,
        },
        location: { hostname: host },
        confirm: (msg) => { prompts.push(msg); return confirmAnswer.value; },
        alert: () => {},
        fetch: async (url, opts) => {
            requests.push({ url, method: opts.method, body: opts.body ? JSON.parse(opts.body) : undefined });
            return { json: async () => ({ ErrorNumber: 0, Value: {} }) };
        },
        setTimeout: () => 0,
        console,
    };
    vm.createContext(ctx);
    vm.runInContext(block + '\n;wifiRefresh = function () {}; globalThis.__wifiState = wifiState; globalThis.__init = wifiInit;', ctx);
    ctx.__wifiState.status = status || { Ssid: 'HomeNet', DeviceCount: 1, ApActive: apOn };
    ctx.__wifiState.ap = { Configured: true, Ssid: 'Old Name', Band: 'bg' };
    ctx.__init();
    const sent = () => requests.filter((r) => r.method === 'PUT');
    return { els, bands, prompts, sent, ctx };
}

const flush = () => new Promise((r) => setImmediate(r));

test('hotspot off, Cancel: no request, toggle stays on', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false } });
    t.els['wifi-ap-toggle'].checked = false;
    t.els['wifi-ap-toggle'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-ap-toggle'].checked, true);
});

test('hotspot off, OK: sends PUT /ap with Enabled false', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: true } });
    t.els['wifi-ap-toggle'].checked = false;
    t.els['wifi-ap-toggle'].handlers.change();
    await flush();
    assert.equal(t.sent().length, 1);
    assert.equal(t.sent()[0].body.Enabled, false);
    assert.equal(t.sent()[0].body.Ssid, 'Old Name');
});

test('hotspot on confirms exactly once', async () => {
    const t = await setup({ apOn: false, confirmAnswer: { value: true } });
    t.els['wifi-ap-pass'].value = 'password123';
    t.els['wifi-ap-toggle'].checked = true;
    t.els['wifi-ap-toggle'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 1);
    assert.equal(t.sent()[0].body.Enabled, true);
});

test('hotspot on, Cancel: no request, toggle back off', async () => {
    const t = await setup({ apOn: false, confirmAnswer: { value: false } });
    t.els['wifi-ap-pass'].value = 'password123';
    t.els['wifi-ap-toggle'].checked = true;
    t.els['wifi-ap-toggle'].handlers.change();
    await flush();
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-ap-toggle'].checked, false);
});

test('hotspot name change, Cancel: no request, previous name restored', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false } });
    t.els['wifi-ap-ssid'].value = 'New Name';
    t.els['wifi-ap-ssid'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-ap-ssid'].value, 'Old Name');
});

test('hotspot password change, Cancel: no request, field cleared', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false } });
    t.els['wifi-ap-pass'].value = 'newpassword';
    t.els['wifi-ap-pass'].handlers.change();
    await flush();
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-ap-pass'].value, '');
});

test('hotspot name change, OK: sends PUT /ap with the new name', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: true } });
    t.els['wifi-ap-ssid'].value = 'New Name';
    t.els['wifi-ap-ssid'].handlers.change();
    await flush();
    assert.equal(t.sent().length, 1);
    assert.equal(t.sent()[0].body.Ssid, 'New Name');
    assert.equal(t.sent()[0].body.Enabled, true);
});

test('band click, Cancel: no request, previous band stays selected', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false } });
    t.bands[1].handlers.click();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
    assert.equal(t.bands[0].classList.contains('active'), true);
    assert.equal(t.bands[1].classList.contains('active'), false);
});

test('band click, OK: sends PUT /ap with the new band', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: true } });
    t.bands[1].handlers.click();
    await flush();
    assert.equal(t.sent().length, 1);
    assert.equal(t.sent()[0].body.Band, 'a');
});

test('hotspot edits while the hotspot is off save without a prompt', async () => {
    const t = await setup({ apOn: false, confirmAnswer: { value: false } });
    t.els['wifi-ap-ssid'].value = 'New Name';
    t.els['wifi-ap-ssid'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 0);
    assert.equal(t.sent().length, 1);
    assert.equal(t.sent()[0].body.Enabled, false);
});

test('WiFi radio off, Cancel: no request, toggle stays on', async () => {
    const t = await setup({ apOn: false, confirmAnswer: { value: false } });
    t.els['wifi-radio-toggle'].checked = false;
    t.els['wifi-radio-toggle'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-radio-toggle'].checked, true);
});

test('WiFi radio off, OK: sends PUT /radio with Enabled false', async () => {
    const t = await setup({ apOn: false, confirmAnswer: { value: true } });
    t.els['wifi-radio-toggle'].checked = false;
    t.els['wifi-radio-toggle'].handlers.change();
    await flush();
    assert.equal(t.sent().length, 1);
    assert.match(t.sent()[0].url, /\/radio$/);
    assert.equal(t.sent()[0].body.Enabled, false);
});

test('WiFi radio on saves without a prompt', async () => {
    const t = await setup({ apOn: false, confirmAnswer: { value: false } });
    t.els['wifi-radio-toggle'].checked = true;
    t.els['wifi-radio-toggle'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 0);
    assert.equal(t.sent().length, 1);
    assert.equal(t.sent()[0].body.Enabled, true);
});

// Page served from the hotspot address on a dual-interface board: the client
// switch skip must NOT apply to actions that take the hotspot or radio away.
const ON_HOTSPOT = { host: '172.24.1.1', status: { Ssid: 'HomeNet', DeviceCount: 2, ApActive: true } };

test('on the hotspot address: hotspot off prompts once, Cancel sends nothing', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false }, ...ON_HOTSPOT });
    t.els['wifi-ap-toggle'].checked = false;
    t.els['wifi-ap-toggle'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-ap-toggle'].checked, true);
});

test('on the hotspot address: hotspot name edit prompts once, Cancel sends nothing', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false }, ...ON_HOTSPOT });
    t.els['wifi-ap-ssid'].value = 'New Name';
    t.els['wifi-ap-ssid'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-ap-ssid'].value, 'Old Name');
});

test('on the hotspot address: radio off prompts once, Cancel sends nothing', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false }, ...ON_HOTSPOT });
    t.els['wifi-radio-toggle'].checked = false;
    t.els['wifi-radio-toggle'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
    assert.equal(t.els['wifi-radio-toggle'].checked, true);
});

test('hotspot-only board (ApActive, no Ssid) on a LAN host: hotspot off prompts', async () => {
    const t = await setup({ apOn: true, confirmAnswer: { value: false }, status: { DeviceCount: 1, ApActive: true } });
    t.els['wifi-ap-toggle'].checked = false;
    t.els['wifi-ap-toggle'].handlers.change();
    await flush();
    assert.equal(t.prompts.length, 1);
    assert.equal(t.sent().length, 0);
});
