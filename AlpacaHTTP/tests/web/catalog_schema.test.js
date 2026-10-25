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

const { WEB, readWeb, readCatalogFixture, formFields } = require('./catalog_form_fields.js');

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
    assert.deepEqual(schema().vendorSelectState(null, 'camera', 'zwo'), { offered: [], value: '', disabled: true });
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
