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

// Pins the ASIAIR port form's duplicate-GPIO check. The server refuses a line
// used on two ports (router.cpp, "each of 12, 13, 26, 18 at most once"); the
// form reports it before submit.

const test = require('node:test');
const assert = require('node:assert/strict');

const { asiairDuplicateGpioError } = require('../../web/format.js');

test('four distinct GPIO lines pass', () => {
    assert.equal(asiairDuplicateGpioError([12, 13, 26, 18]), null);
    assert.equal(asiairDuplicateGpioError([18, 26, 13, 12]), null);
});

test('one duplicated line is named in the message', () => {
    const message = asiairDuplicateGpioError([12, 26, 26, 18]);
    assert.match(message, /GPIO 26/);
    assert.match(message, /ports 2 and 3/);
});

test('legacy load: saved [13, 5, ...] leaves row 1 on its default 13', () => {
    // Row 0 loads 13; row 1's saved 5 is not offered, so the select keeps
    // its default 13; rows 2 and 3 keep their defaults.
    const message = asiairDuplicateGpioError([13, 13, 26, 18]);
    assert.match(message, /GPIO 13/);
    assert.match(message, /ports 1 and 2/);
});

test('blank or non-numeric rows are ignored', () => {
    assert.equal(asiairDuplicateGpioError([12, NaN, NaN, 18]), null);
});
