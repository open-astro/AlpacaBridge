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

// Pins the SynScan and Celestron "Alignment Mode" setting (#860): the select
// offers the three values the server keeps, the form submits it under the
// key the router reads, and an edit shows a saved value (unknown -> auto).
//
// Run: node --test AlpacaHTTP/tests/web/alignment_mode.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const { normalizeAlignmentMode } = require('../../web/format.js');

const web = path.join(__dirname, '..', '..', 'web');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const app = fs.readFileSync(path.join(web, 'app.js'), 'utf8');

for (const vendor of ['synscan', 'celestron']) {
    test(`${vendor} form offers auto, altaz and equatorial`, () => {
        const select = html.match(
            new RegExp(`<select id="${vendor}-alignment-mode" name="${vendor}AlignmentMode"[^>]*>([\\s\\S]*?)</select>`));
        assert.ok(select, `no ${vendor}-alignment-mode select`);
        const values = [...select[1].matchAll(/<option value="([^"]+)"/g)].map((m) => m[1]);
        assert.deepEqual(values, ['auto', 'altaz', 'equatorial']);
    });

    test(`${vendor} alignment mode is loaded on edit and sent on submit`, () => {
        assert.ok(app.includes(
            `setFormValue('${vendor}-alignment-mode', normalizeAlignmentMode(config.alignmentMode));`));
        assert.ok(app.includes(
            `deviceData.alignmentMode = normalizeAlignmentMode(formData.get('${vendor}AlignmentMode'));`));
    });
}

test('Celestron protocol picker distinguishes the handset and direct CGX USB transports', () => {
    const select = html.match(
        /<select id="celestron-protocol"[^>]*name="celestronProtocol"[^>]*>([\s\S]*?)<\/select>/);
    assert.ok(select, 'no Celestron protocol selector');
    const values = [...select[1].matchAll(/<option value="([^"]+)"/g)].map((m) => m[1]);
    assert.deepEqual(values, ['nexstar', 'cgx-aux']);
    assert.ok(html.includes('CGX / CGX-L direct USB (no hand controller)'));
    assert.ok(html.includes('id="celestron-cgx-site-latitude"'));
    assert.ok(html.includes('id="celestron-cgx-site-longitude"'));
});

test('Celestron direct USB config persists protocol and uses the fixed serial transport', () => {
    assert.ok(app.includes("setFormValue('celestron-protocol', protocol);"));
    assert.ok(app.includes("deviceData.protocol = formData.get('celestronProtocol') || 'nexstar';"));
    assert.ok(app.includes("deviceData.protocol === 'cgx-aux'"));
    assert.ok(app.includes("baudGroup.style.display = directUsb ? 'none' : 'block';"));
    assert.ok(app.includes("setFormValue('celestron-cgx-site-latitude', config.siteLatitude);"));
    assert.ok(app.includes("Site latitude and longitude are required for direct CGX USB."));
});

test('Celestron visibility is recalculated whenever its form section is reselected', () => {
    assert.match(app, /vendor === 'celestron'\) \{\s*document\.getElementById\('celestron-config'\)\.style\.display = 'block';\s*updateCelestronConnectionFields\(\);/);
    assert.match(app, /function resetDeviceForm\(\)[\s\S]*?updateVendorOptions\(\);/);
});

test('known alignment modes pass through', () => {
    assert.equal(normalizeAlignmentMode('auto'), 'auto');
    assert.equal(normalizeAlignmentMode('altaz'), 'altaz');
    assert.equal(normalizeAlignmentMode('equatorial'), 'equatorial');
});

test('absent or unknown alignment mode reads as auto', () => {
    assert.equal(normalizeAlignmentMode(undefined), 'auto');
    assert.equal(normalizeAlignmentMode(null), 'auto');
    assert.equal(normalizeAlignmentMode('wedge'), 'auto');
    assert.equal(normalizeAlignmentMode('Equatorial'), 'auto');
});
