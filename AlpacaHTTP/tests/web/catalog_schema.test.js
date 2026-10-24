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
// (web/catalog_schema.js): it parses the management envelope, offers a vendor
// for a device type only when the catalog has an available descriptor for the
// pair, and derives the auto-numbered index fields from the EnumerationIndex
// role instead of a hand-kept table in app.js.
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

// The hand-kept reset block cleared a Gemini vendor for a switch, so editing a
// Gemini Power & Data Hub opened with no vendor selected.
test('a Gemini switch keeps its vendor', () => {
    assert.ok(schema().offeredVendors(catalog, 'switch').includes('gemini'));
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

test('catalog_schema.js writes no markup', () => {
    const source = fs.readFileSync(SCHEMA_PATH, 'utf8');
    assert.doesNotMatch(source, /innerHTML|outerHTML|insertAdjacentHTML|document\.write/);
});

test('app.js keeps no hand-kept INDEX_FIELDS table', () => {
    assert.doesNotMatch(app, /const INDEX_FIELDS = \[/);
    assert.doesNotMatch(app, /\{ fieldId: '[a-z0-9-]+', vendor: '/);
});

test('updateVendorOptions keeps no per-vendor availability block', () => {
    assert.doesNotMatch(app, /vendorSelect\.querySelector\('option\[value="[a-z]+"\]'\)/);
    assert.doesNotMatch(app, /vendorSelect\.value === '[a-z]+'\) \{\s*vendorSelect\.value = '';/);
});

test('app.js reads the catalog from GET /management/v1/devicecatalog', () => {
    assert.match(app, /['"`]\/management\/v1\/devicecatalog/);
});

test('index.html loads catalog_schema.js before app.js', () => {
    const schemaTag = html.indexOf('<script src="/web/catalog_schema.js');
    const appTag = html.indexOf('<script src="/web/app.js');
    assert.ok(schemaTag >= 0, 'no <script src="/web/catalog_schema.js"> tag');
    assert.ok(schemaTag < appTag, 'catalog_schema.js must load before app.js');
});
