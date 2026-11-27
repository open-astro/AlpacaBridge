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

// Pins the Remote Alpaca form layout in index.html: every per-device-type
// section (remotealpaca-<type>-fields) is a direct child of #remotealpaca-config.
// updateRemoteAlpacaConfigFields() hides every section but the chosen type's,
// so a section nested inside another one never shows (#2707 review: the
// Focuser section sat inside the SafetyMonitor one).
//
// Run: node --test AlpacaHTTP/tests/web/remotealpaca_form.test.js

const test = require('node:test');
const assert = require('node:assert/strict');

const { readWeb } = require('./catalog_form_fields.js');

// The div depth of each id="remotealpaca-*-fields" opening tag, relative to
// #remotealpaca-config, from a scan of <div ...> and </div> tokens.
function sectionDepths(html) {
    const start = html.indexOf('id="remotealpaca-config"');
    assert.notEqual(start, -1, 'index.html has #remotealpaca-config');
    const open = html.lastIndexOf('<div', start);
    const tokens = /<div\b[^>]*>|<\/div>/g;
    tokens.lastIndex = open;
    const depths = new Map();
    let depth = 0;
    for (let m = tokens.exec(html); m !== null; m = tokens.exec(html)) {
        if (m[0] === '</div>') {
            depth -= 1;
            if (depth === 0) break;  // #remotealpaca-config closed
            continue;
        }
        depth += 1;
        const id = /\bid="(remotealpaca-[a-z]+-fields)"/.exec(m[0]);
        if (id) depths.set(id[1], depth);
    }
    return depths;
}

test('every Remote Alpaca device-type section is a direct child of #remotealpaca-config', () => {
    const depths = sectionDepths(readWeb('index.html'));
    assert.ok(depths.size >= 2, 'found the Remote Alpaca sections');
    for (const [id, depth] of depths) {
        assert.equal(depth, 2, `${id} is nested ${depth - 2} level(s) inside another section`);
    }
});
