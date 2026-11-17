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

// Issue #1764 item 3: a 44 px --tap token for tabs, buttons, info buttons and
// toggles, and a 13/14/17 px type scale with no body or hint text under 13 px.
// Read from style.css; the real tap size on a phone is a human check.
//
// Run: node --test AlpacaHTTP/tests/web/touch_type.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const css = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'style.css'), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '');

// Every rule as {selector, body}; an @media block's rules are read on their own.
const rules = [...css.matchAll(/([^{}]+)\{([^{}]*)\}/g)].map((m) => ({
    selector: m[1].replace(/\s+/g, ' ').trim(),
    body: m[2],
}));
const root = rules.find((r) => r.selector === ':root').body;
const token = (name) => {
    const m = root.match(new RegExp('--' + name + ':\\s*([0-9.]+)px;'));
    assert.ok(m, 'missing token --' + name);
    return Number(m[1]);
};
const ruleFor = (selector) => {
    const found = rules.filter((r) => r.selector.split(',').map((s) => s.trim()).includes(selector));
    assert.ok(found.length > 0, 'missing rule ' + selector);
    return found.map((r) => r.body).join('\n');
};

// The only selectors allowed under 13 px: monospace badges in style.css when
// this test was written (issue #1764). Each stays at 11 px or more. Adding one
// is an edit here, with its reason.
const BADGE_ALLOW_LIST = [
    { selector: '.title .version', reason: 'the header version badge, monospace' },
    { selector: '.title .build-badge', reason: 'the header build badge, monospace' },
    { selector: '.wifi-signal', reason: 'the WiFi signal-strength glyph badge, monospace' },
];

test('the --tap token is 44 px and sizes tabs, buttons, info buttons and toggles (#1764 item 3)', () => {
    assert.strictEqual(token('tap'), 44);
    assert.match(ruleFor('.tab'), /min-height:\s*var\(--tap\);/);
    assert.match(ruleFor('.btn'), /min-height:\s*var\(--tap\);/);
    assert.match(ruleFor('.toast-close'), /min-height:\s*var\(--tap\);/);
    assert.match(ruleFor('.info-sheet-close'), /min-height:\s*var\(--tap\);/);
    // The small controls keep their look and get a --tap hit area.
    for (const control of ['.info-btn', '.device-toggle', '.section-toggle', '.switch', '.oa-switch']) {
        const after = ruleFor(control + '::after');
        assert.match(after, /width:\s*max\(100%, var\(--tap\)\);/, control);
        assert.match(after, /height:\s*max\(100%, var\(--tap\)\);/, control);
        assert.match(ruleFor(control), /position:\s*relative;/, control);
    }
});

test('no body or hint text is under 13 px outside the monospace badge allow-list (#1764 item 3)', () => {
    const scale = { 'fs-small': token('fs-small'), 'fs-body': token('fs-body'), 'fs-large': token('fs-large') };
    const allowed = new Map(BADGE_ALLOW_LIST.map((a) => [a.selector, a]));
    const under = [];
    for (const rule of rules) {
        for (const m of rule.body.matchAll(/font(?:-size)?:\s*(?:var\(--([a-z-]+)\)|([0-9.]+)px)/g)) {
            const px = m[1] ? scale[m[1]] : Number(m[2]);
            assert.ok(px !== undefined, rule.selector + ' uses an unknown size token --' + m[1]);
            if (px >= 13) continue;
            const entry = allowed.get(rule.selector);
            if (!entry) {
                under.push(rule.selector + ' ' + px + 'px');
                continue;
            }
            assert.ok(px >= 11, rule.selector + ' is under 11 px');
            assert.match(rule.body, /font-family:\s*var\(--mono\)|font:[^;]*var\(--mono\)/, rule.selector + ' is not monospace');
        }
    }
    assert.deepStrictEqual(under, []);
    for (const entry of BADGE_ALLOW_LIST) assert.ok(entry.reason.length > 0);
});

test('the type scale is 13, 14 and 17 px and the body text uses it (#1764 item 3)', () => {
    assert.deepStrictEqual([token('fs-small'), token('fs-body'), token('fs-large')], [13, 14, 17]);
    assert.match(ruleFor('body'), /font:\s*var\(--fs-body\)\/1\.45/);
    assert.match(ruleFor('h2'), /font-size:\s*var\(--fs-large\);/);
});
