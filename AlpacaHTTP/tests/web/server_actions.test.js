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

// Issue #1764 item 1: the action bar keeps only Refresh, and Sync Time, Restart
// Server and Shutdown Server move to a Server Actions card on Server Info, with
// Shutdown in its own row. Pinned by reading index.html, style.css and app.js,
// as field_use.test.js does. The real tap sizes on a phone are a human check.
//
// Run: node --test AlpacaHTTP/tests/web/server_actions.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const WEB = path.join(__dirname, '..', '..', 'web');
const read = (name) => fs.readFileSync(path.join(WEB, name), 'utf8');

// The markup between the opening tag that carries `marker` and its matching
// closing </div>, counting nested divs.
function divBlock(html, marker) {
    const start = html.indexOf(marker);
    assert.ok(start >= 0, 'missing ' + marker);
    const open = html.lastIndexOf('<div', start);
    let depth = 0;
    const tag = /<div\b|<\/div>/g;
    tag.lastIndex = open;
    let m;
    while ((m = tag.exec(html)) !== null) {
        depth += m[0] === '</div>' ? -1 : 1;
        if (depth === 0) return html.slice(open, tag.lastIndex);
    }
    assert.fail('unclosed block for ' + marker);
}

const onclicks = (block) => [...block.matchAll(/onclick="([a-zA-Z]+)\(\)"/g)].map((m) => m[1]);

test('the action bar holds only Refresh (#1764 item 1)', () => {
    const bar = divBlock(read('index.html'), 'class="server-actions global-actions"');
    assert.deepStrictEqual(onclicks(bar), ['globalRefresh']);
});

test('Sync Time, Restart and Shutdown sit in the Server Actions card on Server Info, Shutdown in its own row (#1764 item 1)',
     () => {
         const html = read('index.html');
         const serverTab = divBlock(html, 'id="server-tab"');
         const card = divBlock(serverTab, 'id="server-actions-section"');
         assert.match(card, /<h3>Server Actions<\/h3>/);
         // Not a collapsed card: the actions are one tap away.
         assert.doesNotMatch(card.slice(0, card.indexOf('>')), /collapsed/);
         assert.deepStrictEqual(onclicks(card), ['syncTime', 'restartServer', 'shutdownServer']);
         const danger = divBlock(card, 'class="server-actions server-actions-danger"');
         assert.deepStrictEqual(onclicks(danger), ['shutdownServer']);
         const rest = divBlock(card, 'class="server-actions server-actions-card"');
         assert.deepStrictEqual(onclicks(rest), ['syncTime', 'restartServer']);
         // Each handler has exactly one button on the page, so the confirm steps are not bypassed by a stray copy.
         for (const handler of ['syncTime()', 'restartServer()', 'shutdownServer()']) {
             assert.strictEqual(html.split('onclick="' + handler + '"').length - 1, 1, handler);
         }
     });

test('the action bar has the tab content side padding and Shutdown a separated row (#1764 item 1)', () => {
    const css = read('style.css');
    const rule = (selector) => {
        const at = css.indexOf(selector + ' {');
        assert.ok(at >= 0, 'missing rule ' + selector);
        return css.slice(at, css.indexOf('}', at));
    };
    assert.match(rule('.global-actions'), /padding:\s*14px 18px 0;/);
    assert.match(rule('.server-actions-danger'), /border-top:/);
    assert.match(rule('.server-actions-danger'), /margin-top:\s*22px;/);
});

test('the clock error message names the Server Actions card (#1764 item 1)', () => {
    const js = read('app.js');
    assert.ok(js.includes("use Sync Time in the Server Actions card on the Server Info tab.')"));
    assert.ok(!js.includes('bottom of the page'));
});
