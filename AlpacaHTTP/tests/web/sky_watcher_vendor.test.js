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

// Pins the one "Sky-Watcher" vendor entry on the add-device page (#1818).
//
// Run: node --test AlpacaHTTP/tests/web/sky_watcher_vendor.test.js
//
// Two drivers drive Sky-Watcher mounts: synscan (through the hand controller)
// and skywatcher (direct to the motor controller, EQMOD). The catalog gives
// both the vendor label "Sky-Watcher", which is what groups them; the page
// shows one Sky-Watcher entry and a connection choice whose options are the
// two vendor keys, so a saved device keeps the key it was stored with.
//
// Of the two #vendor options only one is shown and enabled at a time: a
// browser that ignores `hidden` on <option> (the iOS picker) would otherwise
// list two identical entries. The connection choice selects the other one;
// the select.value setter selects a disabled option, and the selected one is
// always the shown one, so FormData still carries the key. The app.js
// functions run here over element stubs, as in catalog_schema.test.js.

const test = require('node:test');
const assert = require('node:assert/strict');

const path = require('node:path');

const { WEB, readWeb, readCatalogFixture } = require('./catalog_form_fields.js');

// Required inside each case, so a missing export fails each case on its own.
const schema = () => require(path.join(WEB, 'catalog_schema.js'));

const catalog = readCatalogFixture();
const html = readWeb('index.html');
const app = readWeb('app.js');

const withAvailable = (vendor, deviceType, available) =>
    catalog.map((d) => (d.vendor === vendor && d.deviceType === deviceType ? { ...d, available } : d));

const descriptor = (vendor) => catalog.find((d) => d.vendor === vendor && d.deviceType === 'telescope');

// The <option>s of one <select>, as { value, text }.
function selectOptions(id) {
    const start = html.indexOf(`<select id="${id}"`);
    assert.notEqual(start, -1, `index.html has no <select id="${id}">`);
    const body = html.slice(start, html.indexOf('</select>', start)).replace(/<!--[\s\S]*?-->/g, '');
    return [...body.matchAll(/<option value="([^"]*)"[^>]*>([^<]*)<\/option>/g)]
        .map((m) => ({ value: m[1], text: m[2].trim() }))
        .filter((o) => o.value !== '');
}

test('the catalog labels both Sky-Watcher telescope descriptors "Sky-Watcher"', () => {
    for (const vendor of ['synscan', 'skywatcher']) {
        const d = descriptor(vendor);
        assert.ok(d, `the catalog fixture has no ${vendor} telescope`);
        assert.equal(d.vendorLabel, 'Sky-Watcher', `${vendor} vendorLabel`);
        assert.match(d.displayName, /^Sky-Watcher /, `${vendor} displayName`);
    }
});

test('the vendor select shows both Sky-Watcher keys as one "Sky-Watcher" name', () => {
    const options = selectOptions('vendor').filter((o) => o.value === 'synscan' || o.value === 'skywatcher');
    assert.deepEqual(options.map((o) => o.value).sort(), ['skywatcher', 'synscan']);
    for (const o of options) {
        assert.equal(o.text, 'Sky-Watcher', `the ${o.value} option text`);
    }
});

test('the Sky-Watcher connection choice maps one option to each driver key', () => {
    const options = selectOptions('sky-watcher-connection');
    assert.deepEqual(options.map((o) => o.value), ['synscan', 'skywatcher']);
    assert.deepEqual(options.map((o) => o.text), ['Hand controller (SynScan)', 'Direct cable (EQMOD)']);
    // Unnamed, so FormData never carries it: #vendor alone holds the key.
    const tag = html.match(/<select id="sky-watcher-connection"[^>]*>/)[0];
    assert.doesNotMatch(tag, /\bname=/, 'the connection select has a name');
    const group = html.indexOf('<div id="sky-watcher-connection-group"');
    assert.ok(group >= 0 && group < html.indexOf(tag), 'the select is not inside #sky-watcher-connection-group');
});

// The #vendor option order: SynScan is listed first, so it is the one shown
// until a Sky-Watcher driver is chosen.
const vendorOrder = () => selectOptions('vendor').map((o) => o.value);

test('vendorSelectState shows one Sky-Watcher option: the selected driver, else the first listed', () => {
    const { vendorSelectState } = schema();
    const none = vendorSelectState(catalog, 'telescope', '', '', vendorOrder());
    assert.ok(none.shown.includes('synscan') && !none.shown.includes('skywatcher'), JSON.stringify(none.shown));
    assert.deepEqual(none.group, []);
    const direct = vendorSelectState(catalog, 'telescope', 'skywatcher', '', vendorOrder());
    assert.deepEqual(direct.shown, direct.offered.filter((v) => v !== 'synscan'), 'only synscan drops out');
    assert.deepEqual(direct.group, ['synscan', 'skywatcher']);
    assert.deepEqual(vendorSelectState(catalog, 'telescope', 'zwo', '', vendorOrder()).group, ['zwo']);
    for (const type of ['camera', 'filterwheel', 'focuser', 'switch']) {
        const state = vendorSelectState(catalog, type, 'playerone', '', vendorOrder());
        assert.deepEqual(state.shown, state.offered, `${type}: no other vendor shares a label`);
    }
});

test('vendorSelectState keeps a one-member group when only one Sky-Watcher driver is offered', () => {
    const { vendorSelectState } = schema();
    const built = withAvailable('skywatcher', 'telescope', false);
    const one = vendorSelectState(built, 'telescope', 'synscan', '', vendorOrder());
    assert.deepEqual(one.group, ['synscan']);
    assert.ok(one.shown.includes('synscan') && !one.shown.includes('skywatcher'));
    // Editing a saved direct device in that build keeps both, the saved one shown.
    const kept = vendorSelectState(built, 'telescope', 'skywatcher', 'skywatcher', vendorOrder());
    assert.deepEqual(kept.group, ['synscan', 'skywatcher']);
    assert.ok(kept.shown.includes('skywatcher') && !kept.shown.includes('synscan'));
});

// The group comes from the catalog's vendorLabel alone: no list of keys.
test('vendorSelectState does not group the Sky-Watcher drivers once the catalog drops their label', () => {
    const { vendorSelectState } = schema();
    const unlabelled = catalog.map((d) => {
        const { vendorLabel, ...rest } = d;
        return d.deviceType === 'telescope' ? rest : d;
    });
    const none = vendorSelectState(unlabelled, 'telescope', '', '', vendorOrder());
    assert.ok(none.shown.includes('synscan') && none.shown.includes('skywatcher'), JSON.stringify(none.shown));
    assert.deepEqual(vendorSelectState(unlabelled, 'telescope', 'skywatcher', '', vendorOrder()).group, ['skywatcher']);
});

test('vendorSelectState groups any two vendors of one device type that share a vendorLabel', () => {
    const { vendorSelectState } = schema();
    const paired = catalog.map((d) => (d.deviceType === 'telescope' && (d.vendor === 'celestron' || d.vendor === 'onstep')
        ? { ...d, available: true, vendorLabel: 'Shared Mounts' } : d));
    const none = vendorSelectState(paired, 'telescope', '', '', vendorOrder());
    assert.ok(none.shown.includes('celestron') && !none.shown.includes('onstep'), JSON.stringify(none.shown));
    const picked = vendorSelectState(paired, 'telescope', 'onstep', '', vendorOrder());
    assert.deepEqual(picked.group, ['celestron', 'onstep']);
    assert.ok(picked.shown.includes('onstep') && !picked.shown.includes('celestron'), JSON.stringify(picked.shown));
});

function wholeFunction(name) {
    const start = app.indexOf(`function ${name}(`);
    assert.ok(start >= 0, `app.js has no function ${name}`);
    return `${app.slice(start, app.indexOf('\n}\n', start))}\n}\n`;
}

class Event {
    constructor(type) {
        this.type = type;
    }
}

// A <select> stub: setting value selects the option with that value, disabled
// or not, as the HTML value setter does; no such option selects none ('').
function selectStub(values, value = '') {
    const options = values.map((v) => ({ value: v, hidden: false, disabled: false }));
    let current = value;
    const select = {
        options, disabled: false, changes: 0, onChange: () => {},
        get value() { return current; },
        set value(v) { current = options.some((o) => o.value === v) ? v : ''; },
        querySelectorAll(selector) {
            assert.equal(selector, 'option');
            return options;
        },
        dispatchEvent(event) {
            if (event.type === 'change') {
                select.changes += 1;
                select.onChange();
            }
        },
    };
    return select;
}

// The add-device form's vendor controls, and the app.js functions that drive
// them, run over stubs. #vendor's change runs updateVendorConnection, which the
// wiring case below pins as the first line of its handler.
function vendorForm({ catalogData = catalog, keepVendor = '', vendor = '' } = {}) {
    const elements = {
        'device-type': { value: 'telescope' },
        vendor: selectStub(['', ...vendorOrder()], vendor),
        'sky-watcher-connection': selectStub(selectOptions('sky-watcher-connection').map((o) => o.value)),
        'sky-watcher-connection-group': { hidden: true },
    };
    const document = { getElementById: (id) => elements[id] ?? null };
    const names = ['normalizeDeviceType', 'updateVendorOptions', 'updateVendorConnection', 'selectVendorConnection'];
    const scope = { ...schema(), document, catalogData, keepVendor, Event };
    const fns = new Function('scope', `with (scope) { ${names.map(wholeFunction).join('')} return { ${names} }; }`)(scope);
    elements.vendor.onChange = fns.updateVendorConnection;
    const option = (value) => elements.vendor.options.find((o) => o.value === value);
    return { elements, fns, option };
}

test('only the shown Sky-Watcher option is enabled, and the connection choice swaps the two', () => {
    const form = vendorForm();
    form.fns.updateVendorOptions();
    assert.deepEqual([form.option('synscan').hidden, form.option('synscan').disabled], [false, false]);
    assert.deepEqual([form.option('skywatcher').hidden, form.option('skywatcher').disabled], [true, true]);
    assert.equal(form.elements['sky-watcher-connection-group'].hidden, true, 'shown with no vendor picked');

    // The user picks "Sky-Watcher": the connection choice shows SynScan.
    form.elements.vendor.value = 'synscan';
    form.elements.vendor.dispatchEvent(new Event('change'));
    assert.equal(form.elements['sky-watcher-connection-group'].hidden, false);
    assert.equal(form.elements['sky-watcher-connection'].value, 'synscan');

    // Then "Direct cable (EQMOD)": #vendor takes the disabled key, and the two swap.
    const changes = form.elements.vendor.changes;
    form.elements['sky-watcher-connection'].value = 'skywatcher';
    form.fns.selectVendorConnection();
    assert.equal(form.elements.vendor.value, 'skywatcher');
    assert.deepEqual([form.option('skywatcher').hidden, form.option('skywatcher').disabled], [false, false]);
    assert.deepEqual([form.option('synscan').hidden, form.option('synscan').disabled], [true, true]);
    assert.ok(form.elements.vendor.changes > changes, 'no change on #vendor, so the direct section stays hidden');
    assert.equal(form.elements['sky-watcher-connection'].value, 'skywatcher');
    assert.equal(form.elements['sky-watcher-connection-group'].hidden, false);
});

test('the connection choice stays hidden when only one Sky-Watcher driver is offered and none is edited', () => {
    const built = withAvailable('skywatcher', 'telescope', false);
    const form = vendorForm({ catalogData: built, vendor: 'synscan' });
    form.fns.updateVendorOptions();
    assert.equal(form.elements.vendor.value, 'synscan');
    assert.equal(form.elements['sky-watcher-connection-group'].hidden, true);
    // Editing a saved direct device in that build shows it, with that driver chosen.
    const edit = vendorForm({ catalogData: built, keepVendor: 'skywatcher', vendor: 'skywatcher' });
    edit.fns.updateVendorOptions();
    assert.equal(edit.elements['sky-watcher-connection-group'].hidden, false);
    assert.equal(edit.elements['sky-watcher-connection'].value, 'skywatcher');
});

test('the #vendor change handler starts with updateVendorConnection, and the connection select runs selectVendorConnection', () => {
    assert.ok(/document\.getElementById\('vendor'\)\.addEventListener\('change', function\(\) \{\s*updateVendorConnection\(\);/.test(app),
        'the #vendor change handler does not start with updateVendorConnection()');
    assert.ok(/document\.getElementById\('sky-watcher-connection'\)\.addEventListener\('change', selectVendorConnection\);/.test(app),
        'the connection select does not run selectVendorConnection on change');
});

test('the grouping and connection code names no vendor key', () => {
    const sources = {
        'catalog_schema.js': readWeb('catalog_schema.js'),
        updateVendorConnection: wholeFunction('updateVendorConnection'),
        selectVendorConnection: wholeFunction('selectVendorConnection'),
    };
    for (const vendor of new Set(catalog.map((d) => d.vendor))) {
        for (const [where, source] of Object.entries(sources)) {
            assert.doesNotMatch(source, new RegExp(`['"\`]${vendor}['"\`]`), `${where} names the vendor ${vendor}`);
        }
    }
});

// The device form's submit handler, run with `scope` for its free names.
function submitHandler(scope) {
    const marker = "document.getElementById('device-form').addEventListener('submit', ";
    const start = app.indexOf(marker);
    assert.ok(start >= 0, 'app.js has no device-form submit handler');
    const handler = app.slice(start + marker.length, app.indexOf('\n});\n', start) + 2);
    const helpers = ['normalizeDeviceType', 'readOptionalNumber', 'applyOpticsMm'].map(wholeFunction).join('');
    return new Function('scope', `with (scope) { ${helpers} return (${handler}); }`)(scope);
}

// Editing switches the device to the other driver: decision 0018's remove-then-
// configure carries the saved UniqueID to the new key. (A configure the server
// rejects after the remove loses the entry: that holds for any vendor change.)
test('an edit that switches the Sky-Watcher connection submits the new key with the original uniqueId', async () => {
    const form = vendorForm({ keepVendor: 'synscan', vendor: 'synscan' });
    form.fns.updateVendorOptions();
    form.elements['sky-watcher-connection'].value = 'skywatcher';
    form.fns.selectVendorConnection();
    assert.equal(form.elements.vendor.value, 'skywatcher');

    const dataset = {
        editing: 'true', originalDeviceType: 'telescope', originalDeviceNumber: '0', originalVendor: 'synscan',
        originalUniqueId: '6f1c2b9e-0000-4000-8000-000000001818',
    };
    const fields = {
        deviceType: 'telescope', deviceNumber: '0', skywatcherConnectionType: 'auto',
        skywatcherSiteLatitude: '-33.856784', skywatcherSiteLongitude: '151.215297',
    };
    class FormData {
        get(name) { return name === 'vendor' ? form.elements.vendor.value : (fields[name] ?? null); }
        has(name) { return name in fields; }
    }
    const posts = [];
    const fetch = async (url, init) => {
        posts.push({ url, body: JSON.parse(init.body) });
        return { json: async () => ({ ErrorNumber: 0 }) };
    };
    const elements = { 'form-message': { style: {} } };
    const handler = submitHandler({
        document: { getElementById: (id) => elements[id] ?? null }, FormData, fetch, API_BASE: '',
        MM_PER_METER: 1000, alert: (text) => assert.fail(`alert: ${text}`),
        resetDeviceForm: () => {}, setTimeout: () => {}, loadDevices: () => {}, showTab: () => {},
    });
    await handler.call({ dataset }, { preventDefault: () => {} });

    assert.deepEqual(posts.map((p) => p.url), ['/management/v1/removedevice', '/management/v1/configuredevice']);
    assert.deepEqual(posts[0].body, { deviceType: 'telescope', deviceNumber: 0, vendor: 'synscan' });
    assert.equal(posts[1].body.vendor, 'skywatcher');
    assert.equal(posts[1].body.uniqueId, dataset.originalUniqueId);
});
