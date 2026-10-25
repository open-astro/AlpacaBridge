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

// Pins the generic half of the edit form (web/catalog_schema.js): editing a
// device fills every form field its catalog descriptor names from the saved
// config, and a field whose appliesWhen discriminator holds another value is
// hidden. A saved value is shown as saved; an absent one shows the form's own
// markup default, except a Discriminator, which shows the catalog default
// (the value the server reads for an absent key, so the form opens on the
// branch the driver runs). The cases below are what the hand-written
// startEditDevice chain showed at upstream main bcb05e4975. The drift check's
// app.js half lives here too, because the per-field chain it forbids goes in
// the same change: startEditDevice fills by hand only the fields marked
// data-config-fill="manual", and fills each of them.
//
// Run: node --test AlpacaHTTP/tests/web/catalog_edit_fill.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');

const { WEB, readWeb, readCatalogFixture, formFields } = require('./catalog_form_fields.js');

const schema = () => require(path.join(WEB, 'catalog_schema.js'));

const catalog = readCatalogFixture();
const html = readWeb('index.html');
const app = readWeb('app.js');
const fields = formFields(html);
const descriptor = (vendor, deviceType) => catalog.find((d) => d.vendor === vendor && d.deviceType === deviceType);

// What each element shows after the generic fill: the assigned value, else
// the element's markup default.
function shown(vendor, deviceType, config) {
    const assigned = new Map(schema().editAssignments(descriptor(vendor, deviceType), config, fields)
        .map((a) => [a.id, a.value]));
    return (id) => {
        const f = fields.find((x) => x.id === id);
        assert.ok(f, `index.html has no #${id}`);
        const value = assigned.has(id) && assigned.get(id) !== null ? assigned.get(id) : f.markupDefault;
        return f.control === 'checkbox' ? value === true : String(value);
    };
}

test('editAssignments fills saved values, leaves absent ones to the markup, and skips record lists, secrets and manual fields', () => {
    const d = {
        vendor: 'acme', deviceType: 'focuser', available: true, fields: [
            { key: 'mode', type: 'string', role: 'Discriminator', default: 'a' },
            { key: 'count', type: 'int', role: 'Plain', default: 1 },
            { key: 'enabled', type: 'bool', role: 'Plain', default: false },
            { key: 'names', type: 'stringList', role: 'Plain' },
            { key: 'ports', type: 'recordList', role: 'Plain', fields: [] },
            { key: 'token', type: 'string', role: 'Secret', default: '' },
            { key: 'host', type: 'string', role: 'Host', default: '', appliesWhen: { key: 'mode', value: 'b' } },
            { key: 'apertureDiameter', type: 'double', role: 'Plain', default: 0.0 },
        ],
    };
    const el = (id, configKey, control, extra = {}) =>
        ({ id, vendor: 'acme', deviceType: 'focuser', configKey, control, manual: false, path: ['acme-config'], ...extra });
    const ff = [
        el('acme-mode', 'mode', 'select'),
        el('acme-count', 'count', 'number'),
        el('acme-enabled', 'enabled', 'checkbox'),
        el('acme-names', 'names', 'textarea'),
        el('acme-port-pwm-0', 'ports', 'checkbox'),
        el('acme-token', 'token', 'text'),
        el('acme-host', 'host', 'text'),
        el('acme-aperture', 'apertureDiameter', 'number', { manual: true }),
        el('other-count', 'count', 'number', { vendor: 'other' }),
    ];
    const config = { mode: 'b', count: 3, enabled: true, names: ['L', 'R'], ports: [{ pwm: true }], token: 'x', host: null,
        apertureDiameter: 0.2 };
    assert.deepEqual(schema().editAssignments(d, config, ff), [
        { id: 'acme-mode', key: 'mode', value: 'b', discriminator: true },
        { id: 'acme-count', key: 'count', value: 3, discriminator: false },
        { id: 'acme-enabled', key: 'enabled', value: true, discriminator: false },
        { id: 'acme-names', key: 'names', value: 'L\nR', discriminator: false },
        { id: 'acme-host', key: 'host', value: null, discriminator: false },
    ]);
});

test('ZWO USB Limit edits explicit values and leaves default blank without affecting switches', () => {
    const bandwidth = descriptor('zwo', 'camera').fields.find((field) => field.key === 'usbBandwidth');
    const input = html.match(/<input[^>]*id="zwo-usb-bandwidth"[^>]*>/)[0];
    assert.ok(input.includes(`placeholder="Default: ${bandwidth.default}"`));
    assert.ok(html.includes(`Blank uses ${bandwidth.default}, clamped to the camera's range, in manual mode.`));
    for (const value of [0, 40, 65, 100]) {
        assert.equal(shown('zwo', 'camera', { usbBandwidth: value })('zwo-usb-bandwidth'), String(value));
    }
    for (const config of [{}, { usbBandwidth: null }]) {
        assert.equal(shown('zwo', 'camera', config)('zwo-usb-bandwidth'), '');
    }
    assert.ok(!schema().editAssignments(descriptor('zwo', 'switch'), { usbBandwidth: 65 }, fields)
        .some((a) => a.key === 'usbBandwidth'));
    const { zwoUsbBandwidth } = require(path.join(WEB, 'format.js'));
    for (const value of ['', ' ', null, undefined]) assert.equal(zwoUsbBandwidth(value), undefined);
    for (const value of ['0', '40', '65', '100']) assert.equal(zwoUsbBandwidth(value), Number(value));
    for (const value of ['40.5', '40garbage', '-1', '101', 'NaN', 'Infinity']) {
        assert.throws(() => zwoUsbBandwidth(value), /USB Limit/);
    }
});

test('ZWO camera controls are disabled when another vendor is selected', () => {
    const body = functionBody(app, 'updateZwoConfigFields');
    assert.match(body, /setFieldGroupEnabled\(cameraFields,\s*isCamera\s*&&\s*document\.getElementById\('vendor'\)\?\.value\s*===\s*'zwo'\)/);
});

test('ZWO USB Limit submit sends the parsed value and omits blank input', () => {
    assert.ok(Number(html.match(/src="\/web\/format\.js\?v=(\d+)"/)[1]) > 15,
        'invalidate the cached format.js that lacks zwoUsbBandwidth');
    assert.ok(Number(html.match(/src="\/web\/app\.js\?v=(\d+)"/)[1]) > 48,
        'invalidate the cached app.js that does not submit USB Limit');
    const start = app.indexOf("} else if (deviceData.vendor === 'zwo')");
    const branch = app.slice(start, app.indexOf("} else if (deviceData.vendor === 'qhy'", start));
    const camera = branch.slice(branch.indexOf("if (normalizedType === 'camera')"),
        branch.indexOf("if (normalizedType === 'switch' &&"));
    assert.ok(camera.includes("const bandwidth = zwoUsbBandwidth(formData.get('zwoUsbBandwidth'));"));
    assert.match(camera, /if\s*\(bandwidth !== undefined\)\s*deviceData\.usbBandwidth = bandwidth;/);
});

test('an absent or null Discriminator opens on its catalog default', () => {
    const d = descriptor('touptek', 'switch');
    for (const config of [{}, { switchType: null }]) {
        const mode = schema().editAssignments(d, config, fields).find((a) => a.key === 'switchType');
        assert.equal(mode.value, 'stellavita',
            `${JSON.stringify(config)}: a ToupTek switch saved before switchType existed is a StellaVita PowerBox`);
        assert.equal(shown('touptek', 'switch', config)('touptek-switch-type'), 'stellavita');
    }
    assert.equal(shown('zwo', 'switch', {})('zwo-switch-type'), 'dewheater');
});

// A saved false or 0 is a value, not an absent key: the markup default must
// not replace it.
test('a saved false or 0 is shown as saved, not as the markup default', () => {
    const mount = shown('zwo', 'telescope', { syncTimeOnConnect: false });
    assert.equal(mount('zwo-mount-sync-time-on-connect'), false, 'the checkbox markup is checked');
    assert.equal(shown('zwo', 'telescope', {})('zwo-mount-sync-time-on-connect'), true, 'absent: markup default');
    const focuser = shown('qhy', 'focuser', { holdIhold: 0, maxStep: 0 });
    assert.equal(focuser('qhy-focuser-hold-ihold'), '0', 'the markup value is 4');
    assert.equal(focuser('qhy-focuser-max-step'), '0', 'the markup value is 64000');
    const values = new Map(schema().editAssignments(descriptor('qhy', 'focuser'), { holdIhold: 0, reverse: false }, fields)
        .map((a) => [a.id, a.value]));
    assert.strictEqual(values.get('qhy-focuser-hold-ihold'), 0);
    assert.strictEqual(values.get('qhy-focuser-reverse'), false);
});

test('a device whose vendor and type have no descriptor fills and hides nothing', () => {
    assert.deepEqual(schema().editAssignments(undefined, { cameraIndex: 1 }, fields), []);
    assert.deepEqual(schema().applicability(undefined, fields, {}), {});
});

test('editing a Gemini flat panel v2 on serial loads the v2 block', () => {
    const at = shown('gemini', 'covercalibrator',
        { flatPanelModel: 'v2', connectionType: 'serial', portPath: '/dev/ttyUSB1', baudRate: 9600 });
    assert.equal(at('gemini-flatpanel-model'), 'v2');
    assert.equal(at('gemini-flatpanel-v2-connection-type'), 'serial');
    assert.equal(at('gemini-flatpanel-v2-port-path'), '/dev/ttyUSB1');
    assert.equal(at('gemini-flatpanel-v2-baud-rate'), '9600');
});

test('editing a QHY CFW3 on serial loads the CFW3 block and the filter names', () => {
    const at = shown('qhy', 'filterwheel',
        { wheelType: 'cfw3-usb', connectionType: 'serial', portPath: '/dev/ttyACM0', filterNames: ['L', 'R', 'G', 'B'] });
    assert.equal(at('qhy-cfw-wheel-type'), 'cfw3-usb');
    assert.equal(at('qhy-cfw3-connection-type'), 'serial');
    assert.equal(at('qhy-cfw3-port-path'), '/dev/ttyACM0');
    assert.equal(at('qhy-filter-names'), 'L\nR\nG\nB');
});

test('a QHY focuser with only an index shows the form defaults for the rest', () => {
    const at = shown('qhy', 'focuser', { connectionType: 'auto', focuserIndex: 1 });
    assert.equal(at('qhy-focuser-index'), '1');
    assert.equal(at('qhy-focuser-max-step'), '64000');
    assert.equal(at('qhy-focuser-speed'), '1');
    assert.equal(at('qhy-focuser-temperature-source'), 'external');
    assert.equal(at('qhy-focuser-hold-ihold'), '4');
    assert.equal(at('qhy-focuser-hold-irun'), '8');
    assert.equal(at('qhy-focuser-reverse'), false);
    assert.equal(at('qhy-focuser-hold-force'), false);
});

test('a Sky-Watcher mount shows its saved site and backlash, and a limit it does not save stays blank', () => {
    const at = shown('skywatcher', 'telescope', {
        connectionType: 'network', host: '192.0.2.10', udpPort: 11880,
        siteLatitude: -33.856784, siteLongitude: 151.215297, siteElevation: 42,
        backlashEnabledRA: true, backlashStepsRA: 120, meridianLimitMinutes: null,
    });
    assert.equal(at('skywatcher-connection-type'), 'network');
    assert.equal(at('skywatcher-host'), '192.0.2.10');
    assert.equal(at('skywatcher-site-latitude'), '-33.856784');
    assert.equal(at('skywatcher-site-longitude'), '151.215297');
    assert.equal(at('skywatcher-site-elevation'), '42');
    assert.equal(at('skywatcher-backlash-enabled-ra'), true);
    assert.equal(at('skywatcher-backlash-steps-ra'), '120');
    assert.equal(at('skywatcher-backlash-enabled-dec'), false);
    assert.equal(at('skywatcher-backlash-steps-dec'), '0');
    assert.equal(at('skywatcher-min-altitude'), '', 'absent = limit off = blank (open-astro#436)');
    assert.equal(at('skywatcher-meridian-limit'), '', 'null = limit off = blank (open-astro#436)');
    assert.equal(at('skywatcher-dec-axis-sense'), 'auto');
});

test('Bisque and gphoto fall back to the values the form shows for a new device', () => {
    const bisque = shown('bisque', 'telescope', {});
    assert.equal(bisque('bisque-host'), 'localhost');
    assert.equal(bisque('bisque-tcp-port'), '3040');
    const gphoto = shown('gphoto', 'camera', { cameraIndex: 2 });
    assert.equal(gphoto('gphoto-camera-index'), '2');
    assert.equal(gphoto('gphoto-mirror-lockup-settle-ms'), '2000');
    assert.equal(gphoto('gphoto-long-exposure-nr'), 'camera');
});

test('the fill stays inside the vendor and type of the edited device', () => {
    const qhy = schema().editAssignments(descriptor('qhy', 'focuser'), { maxStep: 5000 }, fields).map((a) => a.id);
    assert.ok(qhy.includes('qhy-focuser-max-step'));
    assert.ok(!qhy.includes('zwo-focuser-max-step'), 'only a ZWO focuser fills the ZWO max step input');
    const zwo = schema().editAssignments(descriptor('zwo', 'focuser'), { maxStep: 5000 }, fields).map((a) => a.id);
    assert.deepEqual(zwo.filter((id) => id.endsWith('max-step')), ['zwo-focuser-max-step']);
});

test('Discriminators are assigned in descriptor order, so the togglers run outer branch last', () => {
    const order = schema().editAssignments(descriptor('gemini', 'covercalibrator'), {}, fields)
        .filter((a) => a.discriminator).map((a) => a.id);
    assert.deepEqual(order, [
        'gemini-flatpanel-connection-type', 'gemini-flatpanel-v2-connection-type',
        'gemini-flatpanel-pro-connection-type', 'gemini-flatpanel-model',
    ]);
});

test('appliesWhen hides a field when its nearest discriminator holds another value', () => {
    const values = {
        'gemini-flatpanel-model': 'v2',
        'gemini-flatpanel-connection-type': 'serial',
        'gemini-flatpanel-v2-connection-type': 'auto',
        'gemini-flatpanel-pro-connection-type': 'auto',
    };
    const applies = schema().applicability(descriptor('gemini', 'covercalibrator'), fields, values);
    assert.equal(applies['gemini-flatpanel-port-path'], true);
    assert.equal(applies['gemini-flatpanel-v2-port-path'], false);
    assert.equal(applies['gemini-flatpanel-v2-index'], true, 'no appliesWhen: always applies');
});

test('appliesWhen chains: a field under a hidden discriminator is hidden too', () => {
    const qhy = descriptor('qhy', 'filterwheel');
    const integrated = schema().applicability(qhy, fields,
        { 'qhy-cfw-wheel-type': 'integrated', 'qhy-cfw3-connection-type': 'serial' });
    assert.equal(integrated['qhy-cfw3-connection-type'], false);
    assert.equal(integrated['qhy-cfw3-port-path'], false);
    const cfw3 = schema().applicability(qhy, fields,
        { 'qhy-cfw-wheel-type': 'cfw3-usb', 'qhy-cfw3-connection-type': 'serial' });
    assert.equal(cfw3['qhy-cfw3-connection-type'], true);
    assert.equal(cfw3['qhy-cfw3-port-path'], true);
});

// The mapped form fields app.js fills by hand (data-config-fill="manual"),
// each because the catalog cannot express it: the aperture diameter and focal
// length (metres saved, millimetres shown), the alignment mode
// (normalizeAlignmentMode), the Celestron connection type (direct CGX USB
// forces serial), learnSiteFromClient (a key the router keeps for every
// device, absent means on) and the ZWO camera identity (issue #914).
const MANUAL_FIELDS = [
    'aperture-diameter', 'focal-length',
    'bisque-aperture-diameter', 'bisque-focal-length',
    'celestron-aperture-diameter', 'celestron-focal-length',
    'onstep-aperture-diameter', 'onstep-focal-length',
    'skywatcher-aperture-diameter', 'skywatcher-focal-length',
    'synscan-aperture-diameter', 'synscan-focal-length',
    'zwo-mount-aperture-diameter', 'zwo-mount-focal-length',
    'celestron-alignment-mode', 'synscan-alignment-mode',
    'celestron-connection-type',
    'skywatcher-learn-site-from-client', 'zwo-mount-learn-site-from-client',
    'zwo-camera-name', 'zwo-camera-serial',
];

test('the fields app.js fills by hand are exactly the ones marked manual', () => {
    const manual = fields.filter((f) => f.manual);
    assert.deepEqual(manual.map((f) => f.id).sort(), [...MANUAL_FIELDS].sort());
    for (const f of manual) assert.ok(f.configKey, `#${f.id} is marked manual but names no catalog field`);
});

// The body of a top-level function declaration in app.js (its closing brace
// is the first "}" at column 0).
function functionBody(source, name) {
    const start = source.search(new RegExp(`^(?:async )?function ${name}\\(`, 'm'));
    assert.ok(start >= 0, `app.js has no function ${name}`);
    return source.slice(start, source.indexOf('\n}\n', start) + 2);
}

// The element ids a piece of app.js sets a value on by literal id:
// setFormValue('<id>', ...) or document.getElementById('<id>').value / .checked = ...
function literalFilledIds(source) {
    const ids = new Set();
    for (const m of source.matchAll(/setFormValue\(\s*'([a-z0-9-]+)'/g)) ids.add(m[1]);
    for (const m of source.matchAll(/getElementById\('([a-z0-9-]+)'\)\??\.(?:value|checked)\s*=(?!=)/g)) ids.add(m[1]);
    return ids;
}

// The element ids a function body sets a value on: the literal patterns, or a
// const / let bound to getElementById('<id>') whose .value / .checked is then
// assigned.
function handFilledIds(body) {
    const ids = literalFilledIds(body);
    for (const m of body.matchAll(/(?:const|let) (\w+) = document\.getElementById\('([a-z0-9-]+)'\);/g)) {
        if (new RegExp(`\\b${m[1]}\\.(?:value|checked)\\s*=(?!=)`).test(body)) ids.add(m[2]);
    }
    return ids;
}

test('the drift check reads app.js: startEditDevice fills by hand only the fields marked manual', () => {
    // Self-test on a literal body (decision 0003: extractor logic has literal fixtures).
    assert.deepEqual([...handFilledIds(`
        setFormValue('a-id', config.a);
        document.getElementById('b-id').checked = config.b === true;
        const cEl = document.getElementById('c-id');
        if (cEl) cEl.value = config.c;
        const dEl = document.getElementById('d-id');
        if (dEl.value === 'x') dEl.dispatchEvent(new Event('change'));
    `)].sort(), ['a-id', 'b-id', 'c-id']);
    // The two literal-id patterns alone, so a fill moved into a helper
    // function is still seen when they run over all of app.js.
    assert.deepEqual([...literalFilledIds("setFormValue('e-id', 1);")], ['e-id']);
    assert.deepEqual([...literalFilledIds("document.getElementById('f-id').value = 1; document.getElementById('g-id')?.checked = true;")]
        .sort(), ['f-id', 'g-id']);
    assert.deepEqual([...literalFilledIds("if (document.getElementById('h-id').value === 'x') {}")], []);

    const body = functionBody(app, 'startEditDevice');
    const handFilled = handFilledIds(body);
    const typeOf = (f) => {
        const d = descriptor(f.vendor, f.deviceType);
        const c = d && d.fields.find((x) => x.key === f.configKey);
        return c ? c.type : null;
    };
    // Port tables (recordList) are filled by hand through computed ids.
    const mapped = fields.filter((f) => f.configKey && f.vendor !== null && typeOf(f) !== 'recordList');
    const generic = mapped.filter((f) => !f.manual).map((f) => f.id);
    for (const id of ['camera-index', 'gemini-flatpanel-v2-port-path', 'qhy-cfw3-port-path', 'wandererastro-box-index',
        'weewx-url', 'touptek-focuser-id', 'skywatcher-backlash-steps-ra', 'zwo-mount-sync-time-on-connect']) {
        assert.ok(generic.includes(id), `#${id} is not a mapped field the generic fill owns`);
    }
    // startEditDevice, any const-bound pattern included, and every literal-id
    // fill anywhere in app.js, so moving the chain into a helper cannot pass.
    const everywhere = new Set([...handFilled, ...literalFilledIds(app)]);
    assert.deepEqual(generic.filter((id) => everywhere.has(id)), [], 'filled by hand but not marked manual');
    assert.deepEqual(mapped.filter((f) => f.manual && !handFilled.has(f.id)).map((f) => f.id), [],
        'marked manual but startEditDevice does not fill it');
});

test('startEditDevice and every discriminator change re-run the appliesWhen hiding', () => {
    const pass = functionBody(app, 'updateFieldApplicability');
    assert.match(pass, /\bapplicability\(/, 'updateFieldApplicability does not call applicability()');
    assert.match(pass, /\.hidden\s*=(?!=)/, 'updateFieldApplicability does not set hidden');
    const edit = functionBody(app, 'startEditDevice');
    const fill = edit.search(/\beditAssignments\(/);
    assert.ok(fill >= 0, 'startEditDevice does not call editAssignments()');
    assert.ok(edit.indexOf('updateFieldApplicability(', fill) > fill,
        'startEditDevice does not re-run updateFieldApplicability() after the fill');
    assert.match(app, /addEventListener\(\s*'change',[^\n]*\n?[^\n]*updateFieldApplicability\b/,
        'no change listener runs updateFieldApplicability');
});
