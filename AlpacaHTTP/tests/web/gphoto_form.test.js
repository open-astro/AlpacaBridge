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


// Pins the gphoto camera form's mirror lock-up settle and long-exposure NR inputs: each has a unique
// vendor-prefixed name (a bare name collides in FormData with another vendor's hidden block), the bounds and
// options agree with the catalog descriptor, and the edit and submit paths read and write the same fields, so a
// UI edit of a gphoto camera keeps both values.
//
// Run: node --test AlpacaHTTP/tests/web/gphoto_form.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const web = path.join(__dirname, '..', '..', 'web');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const app = fs.readFileSync(path.join(web, 'app.js'), 'utf8');
const catalog = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'fixtures', 'devicecatalog.json'), 'utf8'));
const gphotoEntry = (Array.isArray(catalog) ? catalog : catalog.devices || catalog.Value)
    .find((entry) => entry.vendor === 'gphoto' && entry.deviceType === 'camera');
const field = (key) => gphotoEntry.fields.find((f) => f.key === key);

test('each input has a unique vendor-prefixed name', () => {
    assert.equal((html.match(/name="gphotoMirrorLockupSettleMs"/g) || []).length, 1);
    assert.equal((html.match(/name="gphotoLongExposureNR"/g) || []).length, 1);
});

test('the settle input agrees with the catalog range and default', () => {
    const settle = field('mirrorLockupSettleMs');
    const input = html.match(/<input type="number" id="gphoto-mirror-lockup-settle-ms"[^>]*>/)[0];
    assert.ok(input.includes(`min="${settle.range.min}"`));
    assert.ok(input.includes(`max="${settle.range.max}"`));
    assert.ok(input.includes(`value="${settle.default}"`));
    assert.ok(input.includes('step="1"'), 'integer field must step by 1');
});

test('the long-exposure NR select offers the catalog values and defaults to camera', () => {
    const nr = field('longExposureNR');
    const select = html.match(/<select id="gphoto-long-exposure-nr"[\s\S]*?<\/select>/)[0];
    const values = [...select.matchAll(/<option value="([^"]*)"/g)].map((m) => m[1]);
    assert.deepEqual(values, nr.enum);
    assert.match(select, /<option value="camera" selected>/);
    assert.equal(nr.default, 'camera');
});

test('the LENR help line names the Canon camera menu', () => {
    assert.ok(html.includes('Canon: LENR is set in the camera menu.'));
});

test('edit loads both fields from the saved config', () => {
    assert.ok(app.includes("setFormValue('gphoto-mirror-lockup-settle-ms', config.mirrorLockupSettleMs"));
    assert.ok(app.includes("setFormValue('gphoto-long-exposure-nr', config.longExposureNR || 'camera');"));
});

test('submit sends both fields under their config keys', () => {
    const branch = app.slice(app.indexOf("deviceData.vendor === 'gphoto'"));
    const block = branch.slice(0, branch.indexOf("} else if (deviceData.vendor === 'touptek'"));
    assert.ok(block.includes("readOptionalNumber(formData, 'gphotoMirrorLockupSettleMs')"));
    assert.ok(block.includes('deviceData.mirrorLockupSettleMs = gphotoMirrorLockupSettleMs;'));
    assert.ok(block.includes("deviceData.longExposureNR = formData.get('gphotoLongExposureNR') || 'camera';"));
});
