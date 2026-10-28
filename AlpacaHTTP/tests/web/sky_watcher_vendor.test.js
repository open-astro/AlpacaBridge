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

// Pins the one "Sky-Watcher" vendor entry on the add-device page (#1818).
//
// Run: node --test AlpacaHTTP/tests/web/sky_watcher_vendor.test.js
//
// Two drivers drive Sky-Watcher mounts: synscan (through the hand controller)
// and skywatcher (direct to the motor controller, EQMOD). The catalog gives
// both the vendor label "Sky-Watcher", which is what groups them; the page
// shows one Sky-Watcher entry and a connection choice whose options are the
// two vendor keys, so a saved device keeps the key it was stored with.

const test = require('node:test');
const assert = require('node:assert/strict');

const { readWeb, readCatalogFixture } = require('./catalog_form_fields.js');

const catalog = readCatalogFixture();
const html = readWeb('index.html');

const descriptor = (vendor) => catalog.find((d) => d.vendor === vendor && d.deviceType === 'telescope');

// The <option>s of one <select>, as { value, text }.
function selectOptions(id) {
    const start = html.indexOf(`<select id="${id}"`);
    assert.notEqual(start, -1, `index.html has no <select id="${id}">`);
    const body = html.slice(start, html.indexOf('</select>', start));
    return [...body.matchAll(/<option value="([^"]*)"[^>]*>([^<]*)<\/option>/g)]
        .map((m) => ({ value: m[1], text: m[2].trim() }))
        .filter((o) => o.value !== '');
}

test('the catalog labels both Sky-Watcher telescope descriptors "Sky-Watcher"', () => {
    for (const vendor of ['synscan', 'skywatcher']) {
        const d = descriptor(vendor);
        assert.ok(d, `the catalog fixture has no ${vendor} telescope`);
        assert.equal(d.vendorLabel, 'Sky-Watcher', `${vendor} vendorLabel`);
        assert.match(d.displayName, /^Sky-Watcher /, `${vendor} displayName`);
    }
});

test('the vendor select shows both Sky-Watcher keys as one "Sky-Watcher" name', () => {
    const options = selectOptions('vendor').filter((o) => o.value === 'synscan' || o.value === 'skywatcher');
    assert.deepEqual(options.map((o) => o.value).sort(), ['skywatcher', 'synscan']);
    for (const o of options) {
        assert.equal(o.text, 'Sky-Watcher', `the ${o.value} option text`);
    }
});

test('the Sky-Watcher connection choice maps one option to each driver key', () => {
    const options = selectOptions('sky-watcher-connection');
    assert.deepEqual(options.map((o) => o.value), ['synscan', 'skywatcher']);
    assert.deepEqual(options.map((o) => o.text), ['Hand controller (SynScan)', 'Direct cable (EQMOD)']);
});
