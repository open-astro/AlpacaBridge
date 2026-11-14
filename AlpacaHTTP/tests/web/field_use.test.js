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

// Issue #1764: web UI field-use fixes. The pure helpers (deviceStatus,
// nextTabIndex, headerClockText, the header and note constants) are tested
// from format.js; the markup and wiring that need a DOM are pinned by reading
// index.html, app.js, style.css and router.cpp, as hostcheck.test.js does.
//
// Run: node --test AlpacaHTTP/tests/web/field_use.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const WEB = path.join(__dirname, '..', '..', 'web');
const read = (name) => fs.readFileSync(path.join(WEB, name), 'utf8');
const format = require(path.join(WEB, 'format.js'));

test('deviceStatus is green only with a client request in the last 60 s (#1764)', () => {
    const { deviceStatus, CLIENT_REQUEST_FRESH_MS } = format;
    assert.strictEqual(CLIENT_REQUEST_FRESH_MS, 60000);
    const row = (age) => ({ Connected: true, ActiveClients: 1, LastClientRequestAgeMs: age });
    assert.deepStrictEqual(deviceStatus(row(0)), { state: 'connected', text: 'Client connected' });
    assert.strictEqual(deviceStatus(row(60000)).state, 'connected');
    assert.deepStrictEqual(deviceStatus(row(60001)),
                           { state: 'quiet', text: 'Connected, no client request for 60 s' });
    assert.strictEqual(deviceStatus(row(125400)).text, 'Connected, no client request for 125 s');
    // Red is unchanged: a link fault outranks any activity.
    assert.strictEqual(deviceStatus({ ...row(0), LinkFault: 'no reply' }).state, 'error');
});

test('deviceStatus is quiet with no client request in the window, and an older server keeps the plain reading (#1764)', () => {
    const { deviceStatus } = format;
    assert.deepStrictEqual(deviceStatus({ Connected: true, ActiveClients: 0 }),
                           { state: 'quiet', text: 'Connected, no client request in the last 10 min' });
    // A row without ActiveClients comes from a server that does not report activity.
    assert.strictEqual(deviceStatus({ Connected: true }).state, 'connected');
    assert.strictEqual(deviceStatus({ Connected: false, ActiveClients: 0 }).state, 'idle');
    assert.strictEqual(deviceStatus({ Connected: true, ActiveClients: 1, LastClientRequestAgeMs: 'x' }).state,
                       'quiet');
});

test('the web UI marks its device requests with the header the router skips (#1764)', () => {
    const { WEB_UI_CLIENT_HEADER } = format;
    const router = fs.readFileSync(path.join(__dirname, '..', '..', 'src', 'http', 'router.cpp'), 'utf8');
    assert.ok(router.includes(`kWebUiClientHeader = "${WEB_UI_CLIENT_HEADER.name}"`),
              'router.cpp skips a different header name');
    assert.ok(router.includes(`kWebUiClientValue = "${WEB_UI_CLIENT_HEADER.value}"`),
              'router.cpp skips a different header value');
    const app = read('app.js');
    const action = app.slice(app.indexOf('async function calibrationAction('),
                             app.indexOf('function initCalibrationControls('));
    assert.ok(action.includes('[WEB_UI_CLIENT_HEADER.name]: WEB_UI_CLIENT_HEADER.value'),
              'the calibration action is sent without the web UI header');
});

test('no native alert, confirm or prompt is left in the web UI (#1764)', () => {
    const native = /(^|[^\w.$])(window\.)?(alert|confirm|prompt)\s*\(/m;
    for (const name of ['app.js', 'format.js', 'catalog_schema.js', 'index.html']) {
        const match = read(name).match(native);
        assert.strictEqual(match, null, `${name} still calls a native dialog: ${match && match[0]}`);
    }
});

test('the in-page dialog: Enter confirms, Cancel is not a submit button (#1764)', () => {
    const html = read('index.html');
    const dialog = html.slice(html.indexOf('<dialog id="ui-dialog"'), html.indexOf('</dialog>'));
    assert.ok(dialog.length > 0, 'index.html has no #ui-dialog');
    assert.match(dialog, /<form method="dialog"/);
    assert.match(dialog, /<button id="ui-dialog-cancel"[^>]*type="button"/);
    assert.match(dialog, /<button id="ui-dialog-confirm"[^>]*type="submit" value="confirm"/);
    assert.match(html, /<div id="toast-region"/);
});

test('nextTabIndex wraps the arrow keys and jumps Home and End (#1764)', () => {
    const { nextTabIndex } = format;
    assert.strictEqual(nextTabIndex(0, 'ArrowRight', 3), 1);
    assert.strictEqual(nextTabIndex(2, 'ArrowRight', 3), 0);
    assert.strictEqual(nextTabIndex(0, 'ArrowLeft', 3), 2);
    assert.strictEqual(nextTabIndex(1, 'Home', 3), 0);
    assert.strictEqual(nextTabIndex(0, 'End', 3), 2);
    assert.strictEqual(nextTabIndex(0, 'Enter', 3), null);
    assert.strictEqual(nextTabIndex(0, 'ArrowRight', 0), null);
});

test('the arrow keys, Home and End open and focus the next tab (#1764)', () => {
    // Runs the real initTabs and showTab from app.js (the "Tab management"
    // block) against stub tabs and panels.
    const src = read('app.js');
    const block = src.slice(src.indexOf('// Tab management'), src.indexOf('// Refresh whatever the active tab shows.'));
    assert.ok(block.includes('function initTabs()') && block.includes('function showTab('));
    const doc = { activeElement: null };
    const classSet = () => {
        const set = new Set();
        return { add: (c) => set.add(c), remove: (c) => set.delete(c), contains: (c) => set.has(c),
                 toggle: (c, on) => (on ? set.add(c) : set.delete(c)) };
    };
    const names = ['devices', 'configure', 'server'];
    const panels = names.map((name) => ({ id: name + '-tab', classList: classSet() }));
    const tabs = names.map((name) => {
        const attrs = {};
        const tab = { dataset: { tab: name }, classList: classSet(), tabIndex: -1, listeners: {},
                      setAttribute: (k, v) => { attrs[k] = v; }, attr: (k) => attrs[k],
                      addEventListener: (type, fn) => { tab.listeners[type] = fn; },
                      focus: () => { doc.activeElement = tab; } };
        return tab;
    });
    doc.querySelectorAll = (sel) => {
        if (sel === '.tabs [role="tab"]' || sel === '.tab') return tabs;
        if (sel === '.tab-content') return panels;
        throw new Error('unexpected selector ' + sel);
    };
    doc.getElementById = (id) => panels.find((p) => p.id === id) || null;
    const resets = [];
    const ctx = { document: doc, nextTabIndex: format.nextTabIndex, resetDeviceForm: () => resets.push(1) };
    vm.createContext(ctx);
    vm.runInContext(block + '\n;initTabs();', ctx);
    const press = (from, key) => {
        let prevented = false;
        tabs[from].listeners.keydown({ key, preventDefault: () => { prevented = true; } });
        return prevented;
    };
    const open = () => names.filter((_, i) => panels[i].classList.contains('active'));
    assert.strictEqual(press(0, 'ArrowRight'), true);
    assert.deepStrictEqual(open(), ['configure']);
    assert.strictEqual(doc.activeElement, tabs[1]);
    assert.deepStrictEqual(tabs.map((t) => t.attr('aria-selected')), ['false', 'true', 'false']);
    assert.deepStrictEqual(tabs.map((t) => t.tabIndex), [-1, 0, -1]);
    assert.strictEqual(resets.length, 1, 'opening Configure from the keyboard did not reset the form');
    press(1, 'End');
    assert.deepStrictEqual(open(), ['server']);
    assert.strictEqual(doc.activeElement, tabs[2]);
    press(2, 'ArrowRight');
    assert.deepStrictEqual(open(), ['devices'], 'ArrowRight on the last tab does not wrap');
    press(0, 'ArrowLeft');
    assert.deepStrictEqual(open(), ['server'], 'ArrowLeft on the first tab does not wrap');
    press(2, 'Home');
    assert.deepStrictEqual(open(), ['devices']);
    assert.strictEqual(doc.activeElement, tabs[0]);
    assert.strictEqual(press(0, 'Enter'), false, 'a key the tablist does not use was swallowed');
    assert.deepStrictEqual(open(), ['devices']);
});

test('the ASIAIR duplicate GPIO error goes under the port that repeats a line (#1764)', () => {
    const { asiairRepeatedGpioPort, asiairDuplicateGpioError } = format;
    assert.strictEqual(asiairRepeatedGpioPort([12, 13, 26, 18]), -1);
    assert.strictEqual(asiairRepeatedGpioPort([12, 13, 12, 18]), 2);
    assert.strictEqual(asiairRepeatedGpioPort([12, 13, 13, 12]), 2, 'the first repeat wins');
    assert.strictEqual(asiairRepeatedGpioPort([NaN, NaN, 26, 18]), -1, 'blank ports never repeat');
    assert.strictEqual(asiairRepeatedGpioPort([NaN, 18, NaN, 18]), 3);
    // The port it picks is the second one the error message names.
    for (const gpios of [[12, 13, 12, 18], [12, 13, 13, 12], [NaN, 18, NaN, 18]]) {
        const port = asiairRepeatedGpioPort(gpios);
        assert.match(asiairDuplicateGpioError(gpios), new RegExp(`for ports \\d and ${port + 1}\\.`));
    }
    assert.ok(read('app.js').includes(
        "showFieldError('asiairPortGpio' + Math.max(asiairRepeatedGpioPort(asiairGpios), 0), duplicateGpio);"));
});

test('the tab bar is a tablist whose tabs control their panels (#1764)', () => {
    const html = read('index.html');
    assert.match(html, /<nav class="tabs" role="tablist"/);
    const tabs = [...html.matchAll(/<button class="tab[^"]*"[^>]*>/g)].map((m) => m[0]);
    assert.strictEqual(tabs.length, 3);
    let selected = 0;
    for (const tab of tabs) {
        assert.match(tab, /role="tab"/);
        const name = tab.match(/data-tab="([a-z]+)"/)[1];
        const id = tab.match(/id="([a-z-]+)"/)[1];
        assert.match(tab, new RegExp(`aria-controls="${name}-tab"`));
        assert.match(html, new RegExp(`<div id="${name}-tab" class="tab-content[^"]*" role="tabpanel" aria-labelledby="${id}"`),
                     `the ${name} panel is not a tabpanel labelled by its tab`);
        if (/aria-selected="true"/.test(tab)) selected += 1;
        else assert.match(tab, /aria-selected="false"[^>]*tabindex="-1"/);
    }
    assert.strictEqual(selected, 1);
    const app = read('app.js');
    assert.ok(app.includes("btn.setAttribute('aria-selected', selected ? 'true' : 'false');"),
              'showTab does not move aria-selected');
});

test('one focus-visible ring and a reduced-motion block for every transition (#1764)', () => {
    const css = read('style.css');
    assert.match(css, /^:focus-visible \{\n {4}outline: 2px solid/m, 'no global :focus-visible rule');
    assert.match(css, /\.switch input:focus-visible \+ \.slider/);
    assert.match(css, /@media \(prefers-reduced-motion: reduce\) \{\n {4}\*,\n {4}\*::before,\n {4}\*::after \{\n {8}transition: none !important;/,
                 'no reduced-motion block for the transitions');
});

test('headerClockText drops the seconds only on a narrow screen (#1764)', () => {
    const { headerClockText } = format;
    assert.strictEqual(headerClockText('2026-09-11 23:30:48 (NZST)', true), '2026-09-11 23:30 (NZST)');
    assert.strictEqual(headerClockText('2026-09-11 23:30:48 (NZST)', false), '2026-09-11 23:30:48 (NZST)');
    assert.strictEqual(headerClockText('--:--:--', true), '--:--:--');
    assert.strictEqual(headerClockText('clock error', true), 'clock error');
    assert.ok(read('app.js').includes(
        'el.textContent = headerClockText(formatServerClock(serverNow, serverTimeZone), narrow);'));
});

test('headerClockText drops the seconds from real formatServerClock output in every zone tier (#1764)', () => {
    // headerClockText matches formatServerClock's exact layout; a change to that
    // layout would otherwise bring the seconds back on a phone with no error.
    const { formatServerClock, headerClockText } = format;
    const date = new Date(Date.UTC(2026, 8, 11, 11, 30, 48));
    for (const zone of ['Pacific/Auckland', 'UTC', 'Not/A_Zone', '']) {
        const wide = formatServerClock(date, zone);
        const narrow = headerClockText(wide, true);
        assert.match(wide, /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:48 \(.+\)$/, `zone ${zone || '(viewer)'}: ${wide}`);
        assert.strictEqual(narrow, wide.replace(':48 (', ' ('), `zone ${zone || '(viewer)'} kept its seconds: ${narrow}`);
        assert.strictEqual(headerClockText(wide, false), wide);
    }
    assert.strictEqual(headerClockText(formatServerClock(new Date(NaN), 'UTC'), true), '--:--:--');
});

test('the empty Devices state is an Add device button that opens Configure (#1764)', () => {
    const app = read('app.js');
    const empty = app.slice(app.indexOf('<div class="empty-state">'), app.indexOf('updateAutoNumbering();'));
    assert.match(empty, /<button type="button" class="btn btn-primary empty-state-add" onclick="showTab\('configure'\)">Add device<\/button>/);
});

test('the Host check toggle warns that other clients using an unlisted name are refused (#1764)', () => {
    const { HOST_CHECK_LOCKOUT_NOTE } = format;
    assert.match(HOST_CHECK_LOCKOUT_NOTE, /NINA/);
    assert.match(HOST_CHECK_LOCKOUT_NOTE, /will be refused/);
    assert.match(HOST_CHECK_LOCKOUT_NOTE, /add every name they use first/);
    assert.ok(!/—/.test(HOST_CHECK_LOCKOUT_NOTE), 'portal copy uses no em dash');
    const app = read('app.js');
    const row = app.slice(app.indexOf('id="server-host-check-toggle"'), app.indexOf('id="server-allowed-hosts-input"'));
    assert.ok(row.includes('aria-describedby="server-host-check-note"'));
    assert.ok(row.includes('<span id="server-host-check-note" class="info-note">${escapeHtml(HOST_CHECK_LOCKOUT_NOTE)}</span>'),
              'the toggle row does not render the lockout warning');
});
