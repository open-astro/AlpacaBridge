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

const test = require('node:test');
const assert = require('node:assert');
const { wifiSsidKey, wifiSsidLabel } = require('../../web/format.js');

test('SSID identity uses exact bytes while duplicate display labels show the encoding', () => {
    const first = { Ssid: '\uFFFD', SsidHex: 'ff' };
    const second = { Ssid: '\uFFFD', SsidHex: 'fe' };
    const literalReplacementCharacter = { Ssid: '\uFFFD', SsidHex: 'efbfbd' };

    assert.notEqual(wifiSsidKey(first), wifiSsidKey(second));
    assert.notEqual(wifiSsidKey(first), wifiSsidKey(literalReplacementCharacter));
    assert.equal(wifiSsidKey({ Ssid: 'Cafe', SsidHex: '43616665' }), '43616665');
    assert.equal(wifiSsidLabel(first, 3), '\uFFFD (ff)');
    assert.equal(wifiSsidLabel(second, 3), '\uFFFD (fe)');
    assert.equal(wifiSsidLabel(literalReplacementCharacter, 3), '\uFFFD (efbfbd)');
    assert.equal(wifiSsidLabel({ Ssid: 'Cafe', SsidHex: '43616665' }, 1), 'Cafe');
    assert.equal(wifiSsidKey({ Ssid: 'legacy' }), 'legacy');
});
