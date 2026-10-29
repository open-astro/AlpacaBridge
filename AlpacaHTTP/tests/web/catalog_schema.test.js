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

// Pins the web UI's reader of GET /management/v1/devicecatalog
// (web/catalog_schema.js) and its wiring in app.js: the catalog is fetched
// once per page load, before the devices, and a failed fetch is reported and
// retried on the next form open (fail closed: no vendor offered, the vendor
// select disabled). A vendor is offered for a device type only when the
// catalog has an available descriptor for the pair, and the auto-numbered
// index fields come from the EnumerationIndex role instead of a hand-kept
// table in app.js.
//
// Run: node --test AlpacaHTTP/tests/web/catalog_schema.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const { WEB, readWeb, readCatalogFixture, deviceFormHtml, formFields } = require('./catalog_form_fields.js');

const SCHEMA_PATH = path.join(WEB, 'catalog_schema.js');
// Required inside each case, so a missing module fails each case on its own.
const schema = () => require(SCHEMA_PATH);

const catalog = readCatalogFixture();
const html = readWeb('index.html');
const app = readWeb('app.js');

const withAvailable = (vendor, deviceType, available) =>
    catalog.map((d) => (d.vendor === vendor && d.deviceType === deviceType ? { ...d, available } : d));

// Index fields compare without key order, array order or absent idFieldId.
const normalized = (fields) =>
    JSON.parse(JSON.stringify(fields)).sort((a, b) => a.fieldId.localeCompare(b.fieldId));

test('catalogFromResponse returns the descriptors from the management envelope', () => {
    const payload = { Value: catalog, ErrorNumber: 0, ErrorMessage: '', ClientTransactionID: 0, ServerTransactionID: 7 };
    assert.deepEqual(schema().catalogFromResponse(payload), catalog);
});

test('catalogFromResponse throws the server message when ErrorNumber is not 0', () => {
    const { catalogFromResponse } = schema();
    const payload = { Value: [], ErrorNumber: 1035, ErrorMessage: 'catalog unavailable' };
    assert.throws(() => catalogFromResponse(payload), /catalog unavailable/);
});

test('catalogFromResponse throws when Value is not a descriptor array', () => {
    // Resolved outside the thunk: a missing module must not count as the throw.
    const { catalogFromResponse } = schema();
    for (const Value of [undefined, null, '[]', {}]) {
        assert.throws(() => catalogFromResponse({ Value, ErrorNumber: 0, ErrorMessage: '' }), Error,
            JSON.stringify(Value));
    }
});

const envelope = (Value) => ({ Value, ErrorNumber: 0, ErrorMessage: '' });

test('catalogLoader fetches the catalog once and shares it', async () => {
    let calls = 0;
    const load = schema().catalogLoader(async () => {
        calls += 1;
        return envelope(catalog);
    });
    const [first, second] = await Promise.all([load(), load()]);
    assert.equal(calls, 1, 'two calls in flight make one fetch');
    assert.deepEqual(first, catalog);
    assert.strictEqual(second, first);
    assert.strictEqual(await load(), first);
    assert.equal(calls, 1, 'a loaded catalog is not fetched again');
});

test('catalogLoader surfaces a failed load and fetches again on the next call', async () => {
    const answers = [
        () => Promise.reject(new Error('HTTP 503')),
        () => Promise.resolve({ Value: [], ErrorNumber: 1035, ErrorMessage: 'catalog unavailable' }),
        () => Promise.resolve(envelope(catalog)),
    ];
    let calls = 0;
    const load = schema().catalogLoader(() => answers[calls++]());
    await assert.rejects(load(), /HTTP 503/);
    await assert.rejects(load(), /catalog unavailable/, 'an error envelope is a failed load too');
    assert.deepEqual(await load(), catalog);
    await load();
    assert.equal(calls, 3, 'each failure is retried once, the success is kept');
});

// What updateVendorOptions offered per device type at upstream main bcb05e4975,
// with the Gemini switch kept (see the next case).
const OFFERED = {
    camera: ['altair', 'gphoto', 'iguider', 'ioptron', 'playerone', 'qhy', 'svbony', 'touptek', 'zwo'],
    covercalibrator: ['gemini', 'wandererastro'],
    dome: [],
    filterwheel: ['ioptron', 'playerone', 'qhy', 'touptek', 'wandererastro', 'zwo'],
    focuser: ['astroasis', 'celestron', 'gemini', 'ioptron', 'qhy', 'touptek', 'zwo'],
    observingconditions: ['weewx'],
    rotator: ['wandererastro', 'zwo'],
    safetymonitor: [],
    switch: ['gemini', 'ioptron', 'playerone', 'touptek', 'wandererastro', 'zwo'],
    telescope: ['bisque', 'celestron', 'ioptron', 'onstep', 'skywatcher', 'synscan', 'zwo'],
};

test('offeredVendors offers each available vendor for the device types it has a descriptor for', () => {
    for (const [deviceType, vendors] of Object.entries(OFFERED)) {
        assert.deepEqual([...schema().offeredVendors(catalog, deviceType)].sort(), vendors, deviceType);
    }
    assert.deepEqual([...schema().offeredVendors(catalog, 'Switch')].sort(), OFFERED.switch, 'device type case');
});

test('offeredVendors drops a descriptor the build does not include', () => {
    assert.ok(!schema().offeredVendors(catalog, 'focuser').includes('zzz'), 'zzz focuser is available: false');
    const noZwoCamera = withAvailable('zwo', 'camera', false);
    assert.ok(!schema().offeredVendors(noZwoCamera, 'camera').includes('zwo'));
    assert.ok(schema().offeredVendors(noZwoCamera, 'focuser').includes('zwo'));
});

test('offeredVendors keeps the vendor of the edited device when the build lacks it', () => {
    const noZwoCamera = withAvailable('zwo', 'camera', false);
    assert.ok(schema().offeredVendors(noZwoCamera, 'camera', 'zwo').includes('zwo'));
    assert.ok(!schema().offeredVendors(catalog, 'camera', 'weewx').includes('weewx'),
        'a vendor with no descriptor for the type is never kept');
});

test('with no catalog loaded no vendor is offered', () => {
    assert.deepEqual([...schema().offeredVendors(null, 'camera')], []);
});

// The hand-kept reset block cleared a Gemini vendor for a switch, so editing a
// Gemini Power & Data Hub opened with no vendor selected.
test('vendorSelectState clears a vendor the device type does not offer, and keeps a Gemini switch', () => {
    const { vendorSelectState } = schema();
    const state = vendorSelectState(catalog, 'Switch', 'gemini');
    assert.deepEqual([...state.offered].sort(), OFFERED.switch);
    assert.equal(state.value, 'gemini');
    assert.equal(state.disabled, false);
    assert.equal(vendorSelectState(catalog, 'camera', 'weewx').value, '', 'weewx has no camera');
    assert.equal(vendorSelectState(catalog, 'camera', 'zwo').value, 'zwo');
    assert.equal(vendorSelectState(withAvailable('zwo', 'camera', false), 'camera', 'zwo').value, '',
        'a vendor the build lacks is cleared');
    assert.equal(vendorSelectState(withAvailable('zwo', 'camera', false), 'camera', 'zwo', 'zwo').value, 'zwo',
        'unless it is the vendor of the edited device');
});

test('with no catalog loaded the vendor select is disabled and empty', () => {
    assert.deepEqual(schema().vendorSelectState(null, 'camera', 'zwo'), { offered: [], shown: [], group: [], value: '', disabled: true });
});

test('deriveIndexFields takes EnumerationIndex number inputs and pairs the nearest DeviceId, else PortPath, field of the same appliesWhen', () => {
    const acme = (deviceType, fields) => ({ vendor: 'acme', deviceType, available: true, fields });
    const cat = [
        acme('camera', [
            { key: 'cameraIndex', type: 'int', role: 'EnumerationIndex' },
            { key: 'cameraId', type: 'string', role: 'DeviceId' },
            { key: 'portPath', type: 'string', role: 'PortPath' },
            { key: 'mountIndex', type: 'int', role: 'EnumerationIndex' },
        ]),
        acme('focuser', [
            { key: 'mode', type: 'string', role: 'Discriminator' },
            { key: 'focuserIndex', type: 'int', role: 'EnumerationIndex', appliesWhen: { key: 'mode', value: 'usb' } },
            { key: 'hidPath', type: 'string', role: 'PortPath' },
            { key: 'usbPath', type: 'string', role: 'PortPath', appliesWhen: { key: 'mode', value: 'usb' } },
        ]),
        acme('covercalibrator', [
            { key: 'panelIndex', type: 'int', role: 'EnumerationIndex' },
            { key: 'panelId', type: 'string', role: 'DeviceId' },
        ]),
        acme('switch', [{ key: 'cameraIndex', type: 'int', role: 'EnumerationIndex' }]),
    ];
    const el = (id, deviceType, configKey, control, path) => ({ id, vendor: 'acme', deviceType, configKey, control, path });
    const ff = [
        el('acme-camera-index', 'camera', 'cameraIndex', 'number', ['acme-config', 'acme-camera-fields']),
        el('acme-camera-id', 'camera', 'cameraId', 'text', ['acme-config', 'acme-camera-fields']),
        el('acme-camera-port', 'camera', 'portPath', 'text', ['acme-config', 'acme-camera-fields']),
        el('acme-camera-label', 'camera', null, 'number', ['acme-config', 'acme-camera-fields']),
        el('acme-focuser-index', 'focuser', 'focuserIndex', 'number', ['acme-config', 'acme-focuser-fields']),
        el('acme-focuser-hid', 'focuser', 'hidPath', 'text', ['acme-config', 'acme-focuser-fields']),
        el('acme-focuser-usb', 'focuser', 'usbPath', 'text', ['acme-config', 'acme-focuser-fields']),
        el('acme-lite-index', 'covercalibrator', 'panelIndex', 'number', ['acme-config', 'acme-cover-fields', 'acme-lite']),
        el('acme-lite-id', 'covercalibrator', 'panelId', 'text', ['acme-config', 'acme-cover-fields', 'acme-lite']),
        el('acme-pro-index', 'covercalibrator', 'panelIndex', 'number', ['acme-config', 'acme-cover-fields', 'acme-pro']),
        el('acme-pro-id', 'covercalibrator', 'panelId', 'text', ['acme-config', 'acme-cover-fields', 'acme-pro']),
        el('acme-switch-camera', 'switch', 'cameraIndex', 'select', ['acme-config', 'acme-switch-fields']),
    ];
    const entry = (fieldId, deviceType, configKey, idFieldId) => ({ fieldId, vendor: 'acme', deviceType, configKey, idFieldId });
    assert.deepEqual(normalized(schema().deriveIndexFields(cat, ff)), normalized([
        entry('acme-camera-index', 'camera', 'cameraIndex', 'acme-camera-id'),
        entry('acme-focuser-index', 'focuser', 'focuserIndex', 'acme-focuser-usb'),
        entry('acme-lite-index', 'covercalibrator', 'panelIndex', 'acme-lite-id'),
        entry('acme-pro-index', 'covercalibrator', 'panelIndex', 'acme-pro-id'),
    ]));
});

// INDEX_FIELDS in app.js at upstream main bcb05e4975, entry for entry.
const INDEX_FIELDS_AT_BASE = [
    { fieldId: 'camera-index', vendor: 'zwo', deviceType: 'camera', configKey: 'cameraIndex', idFieldId: 'camera-id' },
    { fieldId: 'filterwheel-index', vendor: 'zwo', deviceType: 'filterwheel', configKey: 'filterwheelIndex', idFieldId: 'filterwheel-id' },
    { fieldId: 'focuser-index', vendor: 'zwo', deviceType: 'focuser', configKey: 'focuserIndex', idFieldId: 'focuser-id' },
    { fieldId: 'rotator-index', vendor: 'zwo', deviceType: 'rotator', configKey: 'rotatorIndex', idFieldId: 'rotator-id' },
    { fieldId: 'qhy-camera-index', vendor: 'qhy', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'qhy-cfw-camera-index', vendor: 'qhy', deviceType: 'filterwheel', configKey: 'cameraIndex', idFieldId: 'qhy-cfw-camera-id' },
    { fieldId: 'qhy-cfw3-index', vendor: 'qhy', deviceType: 'filterwheel', configKey: 'filterwheelIndex' },
    { fieldId: 'qhy-focuser-index', vendor: 'qhy', deviceType: 'focuser', configKey: 'focuserIndex' },
    { fieldId: 'svbony-camera-index', vendor: 'svbony', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'altair-camera-index', vendor: 'altair', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'gphoto-camera-index', vendor: 'gphoto', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'touptek-camera-index', vendor: 'touptek', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'touptek-focuser-index', vendor: 'touptek', deviceType: 'focuser', configKey: 'focuserIndex', idFieldId: 'touptek-focuser-id' },
    { fieldId: 'touptek-filterwheel-index', vendor: 'touptek', deviceType: 'filterwheel', configKey: 'filterwheelIndex', idFieldId: 'touptek-filterwheel-id' },
    { fieldId: 'playerone-camera-index', vendor: 'playerone', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'playerone-filterwheel-index', vendor: 'playerone', deviceType: 'filterwheel', configKey: 'filterwheelIndex' },
    { fieldId: 'astroasis-focuser-index', vendor: 'astroasis', deviceType: 'focuser', configKey: 'focuserIndex', idFieldId: 'astroasis-hid-path' },
    { fieldId: 'gemini-focuser-index', vendor: 'gemini', deviceType: 'focuser', configKey: 'focuserIndex' },
    { fieldId: 'ioptron-ieaf-focuser-index', vendor: 'ioptron', deviceType: 'focuser', configKey: 'focuserIndex' },
    { fieldId: 'ioptron-camera-index', vendor: 'ioptron', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'ioptron-filterwheel-index', vendor: 'ioptron', deviceType: 'filterwheel', configKey: 'filterwheelIndex' },
    { fieldId: 'gemini-flatpanel-index', vendor: 'gemini', deviceType: 'covercalibrator', configKey: 'panelIndex' },
    { fieldId: 'gemini-flatpanel-v2-index', vendor: 'gemini', deviceType: 'covercalibrator', configKey: 'panelIndex' },
    { fieldId: 'gemini-flatpanel-pro-index', vendor: 'gemini', deviceType: 'covercalibrator', configKey: 'panelIndex' },
    { fieldId: 'gemini-pdh-index', vendor: 'gemini', deviceType: 'switch', configKey: 'hubIndex' },
    { fieldId: 'wandererastro-cover-index', vendor: 'wandererastro', deviceType: 'covercalibrator', configKey: 'coverIndex' },
    { fieldId: 'wandererastro-rotator-index', vendor: 'wandererastro', deviceType: 'rotator', configKey: 'rotatorIndex' },
    { fieldId: 'wandererastro-filterwheel-index', vendor: 'wandererastro', deviceType: 'filterwheel', configKey: 'wandererFilterwheelIndex' },
    { fieldId: 'wandererastro-box-index', vendor: 'wandererastro', deviceType: 'switch', configKey: 'boxIndex' },
];

test('the index fields derived from index.html and the catalog are the ones app.js kept by hand', () => {
    assert.deepEqual(normalized(schema().deriveIndexFields(catalog, formFields(html))), normalized(INDEX_FIELDS_AT_BASE));
});

// The body of a top-level function declaration in app.js (its closing brace
// is the first "}" at column 0).
function functionBody(source, name) {
    const start = source.search(new RegExp(`^(?:async )?function ${name}\\(`, 'm'));
    assert.ok(start >= 0, `app.js has no function ${name}`);
    return source.slice(start, source.indexOf('\n}\n', start) + 2);
}

const vendors = [...new Set(catalog.map((d) => d.vendor))];

test('app.js keeps no hand-kept INDEX_FIELDS table and derives the index fields from the catalog', () => {
    assert.doesNotMatch(app, /\bINDEX_FIELDS\b/);
    assert.doesNotMatch(app, /\bfieldId\s*:/, 'an index-field entry written out in app.js');
    assert.match(app, /^let indexFields = \[\];$/m);
    assert.match(functionBody(app, 'ensureCatalog'), /\bindexFields = deriveIndexFields\(/);
    assert.match(functionBody(app, 'maybeAutoFillIndexFields'), /\bindexFields\.forEach\(/);
});

test('updateVendorOptions takes the offered vendors from the catalog', () => {
    const body = functionBody(app, 'updateVendorOptions');
    assert.match(body, /\bvendorSelectState\(/, 'updateVendorOptions does not call vendorSelectState()');
    // The select's disabled flag and value read the vendorSelectState() result,
    // not a constant: `vendorSelect.disabled = false` would fail open.
    const state = body.match(/\b(?:const|let) (\w+) = vendorSelectState\(/);
    assert.ok(state, 'updateVendorOptions does not keep the vendorSelectState() result');
    assert.match(body, new RegExp(`\\bvendorSelect\\.disabled\\s*=\\s*${state[1]}\\.disabled\\b`),
        'updateVendorOptions does not disable #vendor from the state');
    assert.match(body, new RegExp(`\\bvendorSelect\\.value\\s*=\\s*${state[1]}\\.value\\b`),
        'updateVendorOptions does not clear a vendor the state drops');
    const hidden = body.match(/\boption\.hidden\s*=\s*([^;]+);/);
    assert.ok(hidden, 'updateVendorOptions never hides a vendor option');
    assert.ok(body.includes(`option.disabled = ${hidden[1]};`),
        'updateVendorOptions does not disable a vendor option the way it hides it');
    for (const vendor of vendors) {
        assert.doesNotMatch(body, new RegExp(`['"\`]${vendor}['"\`]`), `updateVendorOptions names the vendor ${vendor}`);
    }
});

test('app.js keeps one catalog loader on GET /management/v1/devicecatalog', () => {
    assert.match(app, /^const loadCatalog = catalogLoader\([^;]*\bfetch\(\s*['"`]\/management\/v1\/devicecatalog['"`]/m);
    assert.equal(app.match(/\bcatalogLoader\(/g).length, 1, 'a second catalogLoader is a second cache');
});

test('the page starts loading the catalog before it loads the devices', () => {
    const start = app.indexOf("document.addEventListener('DOMContentLoaded', function() {");
    assert.ok(start >= 0, 'no DOMContentLoaded handler');
    const handler = app.slice(start, app.indexOf('\n});\n', start));
    const catalogAt = handler.indexOf('ensureCatalog(');
    assert.ok(catalogAt >= 0, 'DOMContentLoaded does not start the catalog load');
    assert.ok(catalogAt < handler.indexOf('loadDevices('), 'the catalog load starts after loadDevices()');
});

test('a failed catalog load is shown as text and opening the Add form retries it', () => {
    const body = functionBody(app, 'ensureCatalog');
    assert.match(body, /\bawait loadCatalog\(\)/);
    assert.match(body, /\.textContent\s*=\s*['"`]Could not load the device list: /);
    assert.doesNotMatch(body, /innerHTML|insertAdjacentHTML/);
    assert.match(body, /\bupdateVendorOptions\(\)/, 'ensureCatalog does not re-run updateVendorOptions()');
    assert.match(functionBody(app, 'resetDeviceForm'), /\bensureCatalog\(\)/, 'opening the Add form does not retry');
});

test('startEditDevice awaits the catalog before it fills the form, and fills nothing without one', () => {
    const body = functionBody(app, 'startEditDevice');
    assert.match(body, /^async function startEditDevice\(/);
    const wait = body.match(/const (\w+) = await ensureCatalog\(\);\s*if \(!\1\) \{?\s*return;/);
    assert.ok(wait, 'startEditDevice does not await ensureCatalog() and return without a catalog');
    assert.ok(wait.index < body.indexOf("setFormValue('vendor'"), 'startEditDevice fills the vendor before the catalog');
});

test('startEditDevice keeps the edited vendor offered, and resetDeviceForm clears it before the options are rebuilt', () => {
    const edit = functionBody(app, 'startEditDevice');
    assert.match(edit, /keepVendor = vendor;/);
    assert.ok(edit.indexOf('keepVendor = vendor;') < edit.indexOf('updateVendorOptions()'), 'keepVendor is set after the options are built');
    const reset = functionBody(app, 'resetDeviceForm');
    assert.match(reset, /keepVendor = '';/);
    assert.ok(reset.indexOf("keepVendor = '';") < reset.indexOf('updateVendorOptions()'), 'keepVendor is cleared after the options are built');
});

test('the edit click handler awaits startEditDevice', () => {
    assert.match(app, /^ {4}await startEditDevice\(device\);$/m);
    assert.doesNotMatch(app, /^ *startEditDevice\(/m, 'a startEditDevice() call that is not awaited');
});

test('catalog_schema.js writes no markup', () => {
    const source = fs.readFileSync(SCHEMA_PATH, 'utf8');
    assert.doesNotMatch(source, /innerHTML|outerHTML|insertAdjacentHTML|document\.write/);
});

test('index.html loads catalog_schema.js before app.js', () => {
    const schemaTag = html.indexOf('<script src="/web/catalog_schema.js');
    const appTag = html.indexOf('<script src="/web/app.js');
    assert.ok(schemaTag >= 0, 'no <script src="/web/catalog_schema.js"> tag');
    assert.ok(schemaTag < appTag, 'catalog_schema.js must load before app.js');
});

// A failed catalog load writes its message to #form-message, which sits on the
// Configure tab: the edit click must already be there, not on the Devices tab.
test('startEditDevice opens the Configure tab before it waits for the catalog', () => {
    const body = functionBody(app, 'startEditDevice');
    const open = body.indexOf("showTab('configure', { preserveForm: true });");
    assert.ok(open >= 0, "startEditDevice does not call showTab('configure', { preserveForm: true })");
    assert.ok(open < body.indexOf('await ensureCatalog()'), 'the tab opens after the catalog wait, so the message lands on a hidden tab');
    assert.equal(body.split("showTab('configure'").length - 1, 1, 'the tab is opened once');
});

// The delegated listener replaced 29 per-field ones: typing in an index field
// must mark it user-modified so auto-numbering leaves it alone.
test('the delegated input listener marks only index fields as user-modified', () => {
    const start = app.indexOf("document.getElementById('device-form').addEventListener('input', event => {");
    assert.ok(start >= 0, 'app.js has no delegated input listener on #device-form');
    const body = app.slice(start, app.indexOf('\n});', start));
    assert.match(body, /indexFields\.some\(field => field\.fieldId === event\.target\.id\)/);
    assert.match(body, /event\.target\.dataset\.userModified = 'true'/);
    assert.ok(body.indexOf('indexFields.some(') < body.indexOf('userModified'), 'the mark is not guarded by the index-field check');
});

// A read-only mapped control holds a value the server fixes (the ASIAIR GPIO
// chip and PWM device path): a saved config naming another value must not be
// loaded, or the edit sends it back and the server refuses it.
test('editAssignments skips the read-only server-fixed controls, which keep their markup value', () => {
    const fields = formFields(html);
    const readOnly = fields.filter((f) => f.configKey && f.readOnly);
    assert.deepEqual(readOnly.map((f) => f.id).sort(), ['asiair-gpio-chip', 'asiair-plus-device-path']);
    const zwoSwitch = catalog.find((d) => d.vendor === 'zwo' && d.deviceType === 'switch');
    const asiair = schema().editAssignments(zwoSwitch, { switchType: 'asiair', gpioChip: '/dev/gpiochip4' }, fields);
    assert.equal(asiair.find((a) => a.id === 'asiair-gpio-chip'), undefined);
    assert.ok(asiair.find((a) => a.id === 'zwo-switch-type'), 'the rest of the switch still fills');
    const plus = schema().editAssignments(zwoSwitch, { switchType: 'asiair-plus-rk3568', devicePath: '/dev/other' }, fields);
    assert.equal(plus.find((a) => a.id === 'asiair-plus-device-path'), undefined);
    assert.equal(fields.find((f) => f.id === 'asiair-gpio-chip').markupDefault, '/dev/gpiochip0');
    assert.equal(fields.find((f) => f.id === 'asiair-plus-device-path').markupDefault, '/dev/pwm-gpio-misc');
});

test('a checkbox is checked only by a saved true', () => {
    const d = { vendor: 'acme', deviceType: 'focuser', available: true,
        fields: [{ key: 'enabled', type: 'bool', role: 'Plain', default: false }] };
    const ff = [{ id: 'acme-enabled', vendor: 'acme', deviceType: 'focuser', configKey: 'enabled',
        control: 'checkbox', manual: false, path: [] }];
    for (const saved of ['true', 1, 'yes']) {
        assert.equal(schema().editAssignments(d, { enabled: saved }, ff)[0].value, false, JSON.stringify(saved));
    }
    assert.equal(schema().editAssignments(d, { enabled: true }, ff)[0].value, true);
});

function wholeFunction(name) {
    const start = app.indexOf(`function ${name}(`);
    assert.ok(start >= 0, `app.js has no function ${name}`);
    return app.slice(start, app.indexOf('\n}\n', start));
}

// The togglers show the saved branch only if every Discriminator gets its
// change event, after the fill and before the hiding pass.
test('startEditDevice collects the Discriminators, puts the CGX connection type first and dispatches change on each before hiding', () => {
    const body = wholeFunction('startEditDevice');
    assert.match(body, /if \(assignment\.discriminator\) \{\s*discriminators\.push\(element\);/);
    assert.match(body, /celestronConnection\.value = [^\n]*\n\s*discriminators\.unshift\(celestronConnection\);/);
    const dispatch = body.indexOf("discriminators.forEach(element => element.dispatchEvent(new Event('change')));");
    assert.ok(dispatch >= 0, 'no change dispatch on the Discriminators');
    assert.ok(body.indexOf('editAssignments(') < body.indexOf('discriminators.push('));
    assert.ok(body.indexOf('discriminators.unshift(') < dispatch);
    assert.ok(dispatch < body.indexOf('updateFieldApplicability();'), 'the hiding pass runs before the dispatch');
});

test('startEditDevice restores the markup default for a null assignment and rebuilds the filter slot lists', () => {
    const body = wholeFunction('startEditDevice');
    assert.match(body, /if \(assignment\.value === null\) \{\s*restoreMarkupDefault\(element\);/);
    assert.match(body, /if \(vendor === slotVendor && deviceType === slotType\) \{\s*slotUI\.syncSlotsFromTextarea\(\);/);
    assert.ok(body.indexOf('syncSlotsFromTextarea()') > body.indexOf("discriminators.forEach("));
});

test('updateFieldApplicability hides a form group shared by several fields only when none of them applies', () => {
    const body = wholeFunction('updateFieldApplicability');
    assert.match(body, /groupApplies\.set\(group, groupApplies\.get\(group\) === true \|\| applies\[id\]\);/);
    assert.match(body, /group\.hidden = !shown;/);
});

test('the disabled cue is scoped to the vendor select', () => {
    const css = readWeb('style.css');
    assert.match(css, /\.form-group #vendor:disabled \{/);
    assert.doesNotMatch(css, /\.form-group select:disabled/);
});

// The device form as a tree of element stubs, for running app.js DOM readers:
// each <div>, <input>, <select> and <textarea> knows its tag, attributes,
// dataset and parent, and answers getAttribute, hasAttribute and
// closest('[attr]'). The root stands for the <form id="device-form">.
function deviceFormStub(source) {
    const element = (tagName, attrs, parentElement) => {
        const dataset = {};
        for (const [name, value] of Object.entries(attrs)) {
            if (name.startsWith('data-')) {
                dataset[name.slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = value;
            }
        }
        const node = {
            tagName, id: attrs.id || '', dataset, parentElement,
            getAttribute: (name) => (name in attrs ? attrs[name] : null),
            hasAttribute: (name) => name in attrs,
            closest(selector) {
                const name = selector.match(/^\[([a-z-]+)\]$/)[1];
                for (let n = node; n; n = n.parentElement) {
                    if (n.hasAttribute(name)) return n;
                }
                return null;
            },
        };
        return node;
    };
    const parseAttrs = (text) =>
        Object.fromEntries([...text.matchAll(/([^\s=/]+)(?:="([^"]*)")?/g)].map((m) => [m[1], m[2] ?? '']));
    const form = element('FORM', { id: 'device-form' }, null);
    const controls = [];
    let parent = form;
    for (const [, close, tag, attrs] of deviceFormHtml(source).matchAll(/<(\/?)(div|input|select|textarea)\b([^>]*)>/g)) {
        if (tag === 'div') {
            parent = close ? parent.parentElement : element('DIV', parseAttrs(attrs), parent);
        } else if (!close) {
            controls.push(element(tag.toUpperCase(), parseAttrs(attrs), parent));
        }
    }
    return controls;
}

// The browser half of the fill reads the form with readFormFields; every test
// reads it with the formFields() helper. Running the real readFormFields over
// index.html ties the two: each key the helper reports for a mapped control
// (manual, readOnly, configKey, ...) must come out of app.js the same.
test('readFormFields in app.js reports what the formFields helper reports for every mapped control', () => {
    const helperOnly = ['name', 'tag', 'markupDefault'];
    const controls = deviceFormStub(html);
    const document = {
        querySelectorAll(selector) {
            assert.equal(selector, '#device-form [data-config-key]');
            return controls.filter((c) => c.hasAttribute('data-config-key'));
        },
    };
    const source = `${wholeFunction('readFormFields')}\n}\nreturn readFormFields();`;
    const fromApp = new Function('document', source)(document);
    const expected = formFields(html).filter((f) => f.configKey !== null).map((f) =>
        Object.fromEntries(Object.entries(f).filter(([key]) => !helperOnly.includes(key))));
    assert.ok(expected.length > 100, `only ${expected.length} mapped controls`);
    assert.ok(expected.some((f) => f.manual) && expected.some((f) => f.readOnly));
    assert.deepEqual(JSON.parse(JSON.stringify(fromApp)), expected);
});

const FORM_TYPES = ['camera', 'covercalibrator', 'dome', 'filterwheel', 'focuser', 'observingconditions', 'rotator',
    'switch', 'telescope'];

test('offeredDeviceTypes hides dome, which the fixture catalog has no entry for, and offers the other 8 types', () => {
    const { offeredDeviceTypes } = schema();
    assert.deepEqual(offeredDeviceTypes(catalog, FORM_TYPES), FORM_TYPES.filter((t) => t !== 'dome'));
});

test('offeredDeviceTypes hides a type whose only entries are not available, unless it is the edited type', () => {
    const { offeredDeviceTypes } = schema();
    const only = (type) => catalog.filter((d) => d.deviceType !== type || !d.available)
        .concat(catalog.filter((d) => d.deviceType === type).map((d) => ({ ...d, available: false })));
    const cat = only('focuser');
    assert.ok(!offeredDeviceTypes(cat, FORM_TYPES).includes('focuser'));
    assert.ok(offeredDeviceTypes(cat, FORM_TYPES, 'focuser').includes('focuser'));
});

test('offeredDeviceTypes keeps a hidden edited type', () => {
    const { offeredDeviceTypes } = schema();
    assert.ok(offeredDeviceTypes(catalog, FORM_TYPES, 'Dome').includes('dome'));
    assert.equal(offeredDeviceTypes(catalog, FORM_TYPES, 'dome').length, 9);
});

test('offeredDeviceTypes offers nothing for an empty or failed catalog, even for the edited type', () => {
    const { offeredDeviceTypes } = schema();
    assert.deepEqual(offeredDeviceTypes([], FORM_TYPES, 'camera'), []);
    assert.deepEqual(offeredDeviceTypes(null, FORM_TYPES, 'camera'), []);
});

test('app.js applies offeredDeviceTypes where the catalog loads and before an edit selects its type', () => {
    assert.match(app, /function updateDeviceTypeOptions\(\)[\s\S]*offeredDeviceTypes\(catalogData/);
    assert.match(app, /updateDeviceTypeOptions\(\);\s*updateVendorOptions\(\);\s*return catalogData;/);
    assert.match(app, /keepType = deviceType;\s*updateDeviceTypeOptions\(\);\s*setFormValue\('device-type'/);
});
