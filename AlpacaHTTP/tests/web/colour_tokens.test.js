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

// Issue #1764 item 6: every colour of the web UI is a token, so a theme is a
// remap of the token blocks and not a rewrite. A colour literal is allowed only
// in the :root block of style.css and in a :root[data-theme="..."] block. The
// markup and the scripts set colours only through var(--token).
//
// Run: node --test AlpacaHTTP/tests/web/colour_tokens.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const WEB = path.join(__dirname, '..', '..', 'web');
const read = (name) => fs.readFileSync(path.join(WEB, name), 'utf8');

// A hex colour (not an id selector such as #add-device), a colour function, or
// a common named colour. transparent, currentColor and inherit name no colour
// of their own, so they are not literals.
const HEX = /#[0-9a-fA-F]{3,8}(?![\w-])/g;
const FUNC = /\b(?:rgba?|hsla?|hwb|lab|lch|oklab|oklch)\(/g;
const NAMED = /\b(?:white|black|red|green|blue|yellow|orange|gray|grey|silver|purple|pink|brown)\b/g;

const lineOf = (text, index) => 'line ' + text.slice(0, index).split('\n').length;

// Every literal in text, as "line N: literal".
const literals = (text, patterns) =>
    patterns.flatMap((re) => [...text.matchAll(re)].map((m) => lineOf(text, m.index) + ': ' + m[0]));

// Comments blanked to spaces of the same length (newlines kept), so line numbers stay right.
const blank = (s) => s.replace(/[^\n]/g, ' ');
const css = read('style.css').replace(/\/\*[\s\S]*?\*\//g, blank);

test('style.css has no colour literal outside its token blocks (#1764 item 6)', () => {
    // The token blocks: :root and :root[data-theme="..."]. Blanked, not removed, so lines stay right.
    // The lookbehind leaves the closing brace before a block unconsumed, so back-to-back blocks all match.
    const tokenBlock = /(?<=^|\})\s*:root(?:\[data-theme="[a-z-]+"\])?\s*\{[^{}]*\}/g;
    assert.ok(css.match(tokenBlock), 'style.css has no :root token block');
    const outside = css.replace(tokenBlock, blank);
    // Named colours only in declaration values, so a property such as white-space is not one.
    const named = [...outside.matchAll(/([\w-]+)\s*:([^;{}]+)/g)]
        .flatMap((m) => [...m[2].matchAll(NAMED)].map((n) => m[1] + ': ' + n[0]));
    const found = [...literals(outside, [HEX, FUNC]), ...named];
    assert.deepStrictEqual(found, [], 'use a var(--token) defined in :root for each colour');
});

test('index.html sets no inline colour literal (#1764 item 6)', () => {
    const html = read('index.html').replace(/<!--[\s\S]*?-->/g, blank);
    // Inline style attributes, <style> blocks and the presentational colour attributes.
    const found = [];
    for (const m of html.matchAll(/style="[^"]*"|<style[^>]*>[\s\S]*?<\/style>|\b(?:fill|stroke|color|bgcolor)="[^"]*"/g)) {
        for (const re of [HEX, FUNC, NAMED]) {
            for (const hit of m[0].matchAll(re)) found.push(lineOf(html, m.index) + ': ' + hit[0]);
        }
    }
    assert.deepStrictEqual(found, [], 'use var(--token) in the markup');
    // A cached style.css without the tokens would leave every var(--token) above unset.
    const version = (re) => Number(html.match(re)[1]);
    assert.ok(version(/href="\/web\/style\.css\?v=(\d+)"/) > 38, 'invalidate the cached style.css without the colour tokens');
    assert.ok(version(/src="\/web\/app\.js\?v=(\d+)"/) > 56, 'invalidate the cached app.js with the hex error colour');
});

test('the page scripts set no colour literal (#1764 item 6)', () => {
    // A string that is a colour, or a string of inline CSS that sets one. An issue
    // number such as '#9432' in a message is neither.
    const colourString = /^.\s*(?:#[0-9a-fA-F]{3,8}|(?:rgba?|hsla?)\([^)]*\))\s*.$/;
    const inlineCss = /(?:color|background|border|fill|stroke|shadow)[\w-]*\s*:[^;]*(?:#[0-9a-fA-F]{3,8}(?![\w-])|(?:rgba?|hsla?)\()/;
    for (const name of ['app.js', 'format.js', 'catalog_schema.js']) {
        const src = read(name).replace(/\/\*[\s\S]*?\*\//g, blank);
        const found = [...src.matchAll(/'[^'\n]*'|"[^"\n]*"|`[^`]*`/g)]
            .filter((m) => colourString.test(m[0]) || inlineCss.test(m[0]))
            .map((m) => lineOf(src, m.index) + ': ' + m[0]);
        assert.deepStrictEqual(found, [], name + ': use var(--token) for colours');
    }
});
