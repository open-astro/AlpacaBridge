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

// Issue #1764 item 6: the red night theme. Its palette is the one
// :root[data-theme="night"] block of style.css (palette D of the issue's
// evidence comment). These cases compute the WCAG contrast and the hue from
// that block, read the status shapes and the logo rule, and run the toggle and
// the first-paint script with a local storage that throws. How the palette
// looks on a phone at night is a human check.
//
// Run: node --test AlpacaHTTP/tests/web/night_theme.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const WEB = path.join(__dirname, '..', '..', 'web');
const read = (name) => fs.readFileSync(path.join(WEB, name), 'utf8');
const format = require(path.join(WEB, 'format.js'));

const css = read('style.css').replace(/\/\*[\s\S]*?\*\//g, '');
const tokens = (selector) => {
    const block = css.match(new RegExp(selector.replace(/[[\]"]/g, '\\$&') + '\\s*\\{([^{}]*)\\}'));
    assert.ok(block, 'missing block ' + selector);
    return Object.fromEntries([...block[1].matchAll(/--([\w-]+):\s*([^;]+);/g)].map((m) => [m[1], m[2].trim()]));
};
// Empty when the block is missing, so each case fails on its own.
const night = css.includes(':root[data-theme="night"]') ? tokens(':root[data-theme="night"]') : {};

// [r, g, b] (0-255) of a hex or rgb()/rgba() colour, or null for anything else.
const COLOUR = /^(?:#[0-9a-fA-F]{3}|#[0-9a-fA-F]{6}|rgba?\([^)]*\))$/;
const rgb = (value) => {
    if (!COLOUR.test(value)) return null;
    if (value.startsWith('rgb')) return value.match(/[\d.]+/g).slice(0, 3).map(Number);
    const hex = value.length === 4 ? [...value.slice(1)].map((c) => c + c).join('') : value.slice(1);
    return [0, 2, 4].map((i) => parseInt(hex.slice(i, i + 2), 16));
};
const colour = (name) => {
    const value = night[name];
    assert.ok(value, 'the night block does not set --' + name);
    const c = rgb(value);
    assert.ok(c, '--' + name + ' is not a hex or rgb colour in the night block: ' + value);
    return c;
};

// WCAG 2.x relative luminance and contrast ratio.
const luminance = ([r, g, b]) => {
    const lin = (v) => { v /= 255; return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; };
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
};
const contrast = (a, b) => {
    const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
    return (hi + 0.05) / (lo + 0.05);
};

// The surfaces text sits on in the night theme.
const SURFACES = ['bg', 'panel', 'panel-2', 'panel-hover', 'inset'];

test('night text and dim text meet 4.5:1 on every surface (#1764 item 6)', () => {
    assert.strictEqual(night.bg, '#000000', 'palette D background');
    assert.strictEqual(night.panel, '#080000', 'palette D panel');
    assert.strictEqual(night.text, '#f3170b', 'palette D text');
    for (const fg of ['text', 'dim', 'danger-text', 'primary-text']) {
        for (const bg of SURFACES) {
            const ratio = contrast(colour(fg), colour(bg));
            assert.ok(ratio >= 4.5, `--${fg} on --${bg} is ${ratio.toFixed(2)}:1, under 4.5:1`);
        }
    }
});

test('night control borders and the focus ring meet 3:1 on the background and the panels (#1764 item 6)', () => {
    for (const edge of ['control-line', 'danger-line', 'accent-2']) {
        for (const bg of ['bg', 'panel', 'panel-2']) {
            const ratio = contrast(colour(edge), colour(bg));
            assert.ok(ratio >= 3, `--${edge} on --${bg} is ${ratio.toFixed(2)}:1, under 3:1`);
        }
    }
    // Controls are outlined: no fill of their own.
    assert.strictEqual(night['control-bg'], 'transparent');
    assert.strictEqual(night['primary-bg'], 'transparent');
});

// A deep red or black: green and blue each at most 32 of 255, and red at
// least four times the larger of them. #f3170b is (243, 23, 11).
const MAX_GREEN_BLUE = 32;

test('every night colour is a deep red or black, and the night block overrides every colour token (#1764 item 6)', () => {
    for (const [name, value] of Object.entries(night)) {
        if (value === 'transparent') continue;
        const c = rgb(value);
        assert.ok(c, '--' + name + ' must be a hex or rgb colour in the night block, not ' + value);
        const [r, g, b] = c;
        assert.ok(g <= MAX_GREEN_BLUE && b <= MAX_GREEN_BLUE, `--${name} ${value} has green or blue over ${MAX_GREEN_BLUE}`);
        assert.ok(Math.max(g, b) === 0 || r >= 4 * Math.max(g, b), `--${name} ${value} is not a deep red`);
    }
    // A colour token the night block forgot keeps its default blue, green or amber.
    const missing = Object.entries(tokens(':root'))
        .filter(([name, value]) => (COLOUR.test(value) || value.startsWith('var(')) && !(name in night))
        .map(([name]) => '--' + name);
    assert.deepStrictEqual(missing, [], 'the night block must set these tokens');
});

test('night status is a shape as well as a colour: solid connected, hollow idle and quiet, square error (#1764 item 6)', () => {
    const rule = (selector) => {
        const m = css.match(new RegExp('(?:^|\\})([^{}]*' + selector.replace(/[.[\]"]/g, '\\$&') + '[^{}]*)\\{([^{}]*)\\}'));
        assert.ok(m, 'missing rule ' + selector);
        return m[2];
    };
    const hollow = rule(':root[data-theme="night"] .status-dot.status-idle');
    assert.ok(css.match(/:root\[data-theme="night"\] \.status-dot\.status-quiet[^{]*\{[^}]*background:\s*transparent/),
              'quiet is hollow like idle');
    assert.match(hollow, /background:\s*transparent;/);
    assert.match(hollow, /box-shadow:\s*inset 0 0 0 2px var\(--status-dot-color\);/);
    assert.match(rule(':root[data-theme="night"] .status-dot.status-error'), /border-radius:\s*[0-2]px;/);
    // Connected keeps the solid round dot: no night rule changes it.
    assert.doesNotMatch(css, /:root\[data-theme="night"\][^{]*\.status-connected/);
    // The stale marker (a hollow dot and a "Stale" label) still applies in every theme.
    assert.match(rule('.status-dot.stale'), /background:\s*transparent;/);
});

test('the logo is a red silhouette in the night theme, and the favicon is unchanged (#1764 item 6)', () => {
    const logo = css.match(/:root\[data-theme="night"\] \.logo\s*\{([^}]*)\}/);
    assert.ok(logo, 'missing the night .logo rule');
    assert.match(logo[1], /background:\s*var\(--text\);/);
    assert.match(logo[1], /(?:^|[^-])mask:\s*url\("?\/web\/assets\/oa512\.png"?\)/);
    assert.match(logo[1], /-webkit-mask:\s*url\("?\/web\/assets\/oa512\.png"?\)/);
    // The colour image is moved out of its box, so only the silhouette shows.
    assert.match(logo[1], /object-position:/);
    const html = read('index.html');
    assert.strictEqual((html.match(/<img\b/g) || []).length, 1, 'a new image needs a night rule too');
    assert.match(html, /<link rel="icon" type="image\/x-icon" href="\/web\/favicon\.ico">/);
});

// An element with just the attribute calls the theme code makes.
const element = () => {
    const attrs = new Map();
    return {
        attrs,
        setAttribute: (k, v) => attrs.set(k, String(v)),
        removeAttribute: (k) => attrs.delete(k),
        getAttribute: (k) => (attrs.has(k) ? attrs.get(k) : null),
    };
};
const throwingStorage = {
    getItem() { throw new Error('SecurityError'); },
    setItem() { throw new Error('QuotaExceededError'); },
};

test('the night toggle sets the theme attribute and aria-pressed, and stores the choice (#1764 item 6)', () => {
    const { setNightTheme, THEME_STORAGE_KEY } = format;
    const root = element();
    const button = element();
    const saved = new Map();
    const storage = { getItem: (k) => saved.get(k) ?? null, setItem: (k, v) => saved.set(k, v) };
    setNightTheme(root, button, storage, true);
    assert.strictEqual(root.getAttribute('data-theme'), 'night');
    assert.strictEqual(button.getAttribute('aria-pressed'), 'true');
    assert.strictEqual(saved.get(THEME_STORAGE_KEY), 'night');
    setNightTheme(root, button, storage, false);
    assert.strictEqual(root.getAttribute('data-theme'), null);
    assert.strictEqual(button.getAttribute('aria-pressed'), 'false');
    assert.strictEqual(saved.get(THEME_STORAGE_KEY), 'default');
});

test('the night toggle works when local storage throws or is missing (#1764 item 6)', () => {
    const { setNightTheme } = format;
    for (const storage of [throwingStorage, null]) {
        const root = element();
        const button = element();
        assert.doesNotThrow(() => setNightTheme(root, button, storage, true));
        assert.strictEqual(root.getAttribute('data-theme'), 'night');
        assert.strictEqual(button.getAttribute('aria-pressed'), 'true');
    }
});

test('a stored night theme is applied in <head> before the first paint, and a throwing storage leaves the default (#1764 item 6)', () => {
    const html = read('index.html');
    const head = html.slice(0, html.indexOf('</head>'));
    const script = head.match(/<script>([\s\S]*?)<\/script>/);
    assert.ok(script, 'no inline theme script in <head>');
    assert.ok(head.indexOf('<script>') < head.indexOf('rel="stylesheet"'), 'run the theme script before the stylesheet loads');
    const run = (localStorage) => {
        const root = element();
        const ctx = { document: { documentElement: root } };
        Object.defineProperty(ctx, 'localStorage', { get: localStorage });
        vm.runInNewContext(script[1], ctx);
        return root.getAttribute('data-theme');
    };
    const stored = (value) => () => ({ getItem: (k) => (k === format.THEME_STORAGE_KEY ? value : null) });
    assert.strictEqual(run(stored('night')), 'night');
    assert.strictEqual(run(stored('default')), null);
    assert.strictEqual(run(stored(null)), null);
    // Blocked storage throws on the localStorage getter itself, or on getItem.
    assert.strictEqual(run(() => { throw new Error('SecurityError'); }), null);
    assert.strictEqual(run(() => throwingStorage), null);
});

test('the header has a labelled night toggle button of at least the tap size, wired in app.js (#1764 item 6)', () => {
    const html = read('index.html');
    const header = html.slice(html.indexOf('<header>'), html.indexOf('</header>'));
    const button = header.match(/<button\b[^>]*\bid="theme-toggle"[^>]*>([^<]*)<\/button>/);
    assert.ok(button, 'no #theme-toggle button in the header');
    assert.match(button[0], /type="button"/);
    assert.match(button[0], /aria-pressed="false"/);
    assert.match(button[0], /class="btn[ "]/, 'a .btn, so it is at least --tap high');
    assert.match(button[1], /\S/, 'the button has a visible label');
    const app = read('app.js');
    assert.match(app, /getElementById\('theme-toggle'\)/);
    assert.match(app, /setNightTheme\(/);
    // Invalidate cached files without the theme.
    const version = (re) => Number(html.match(re)[1]);
    assert.ok(version(/href="\/web\/style\.css\?v=(\d+)"/) > 39, 'invalidate the cached style.css without the night block');
    assert.ok(version(/src="\/web\/format\.js\?v=(\d+)"/) > 22, 'invalidate the cached format.js without setNightTheme');
    assert.ok(version(/src="\/web\/app\.js\?v=(\d+)"/) > 57, 'invalidate the cached app.js without the toggle');
});
