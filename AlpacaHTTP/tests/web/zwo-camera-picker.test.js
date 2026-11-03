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

// Camera picker of the ZWO camera form (zwoCameraOptions / Selection / Hint in
// format.js, #1970). The rig that started it: three cameras added one at a time,
// each alone at SDK slot 0, while the form numbered the index 0, 1, 2 from the
// saved entries, so two entries learned no serial (#1969).
//
// Run: node --test AlpacaHTTP/tests/web/zwo-camera-picker.test.js

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

const format = require(path.join(__dirname, '..', '..', 'web', 'format.js'));
const { zwoCameraOptions, zwoCameraSelection, zwoCameraHint, configureWarning } = format;
const { readWeb } = require('./catalog_form_fields.js');

const app = readWeb('app.js');
const html = readWeb('index.html');

const MM432 = { Index: 0, Name: 'ZWO ASI432MM', Serial: '1a2b3c4d5e6f7081' };
const MINI174 = { Index: 0, Name: 'ZWO ASI174MM Mini', Serial: '0f0e0d0c0b0a0908' };
const MINI120 = { Index: 1, Name: 'ZWO ASI120MM Mini', Serial: '' };

test('one attached camera beside two configured entries is offered at its own index', () => {
    // Two ZWO camera entries are saved (the ASI174MM Mini, and one that never
    // learned a serial); the ASI432MM is the only camera plugged in.
    const taken = ['0f0e0d0c0b0a0908'];
    const model = zwoCameraOptions([MM432], null, '', taken);
    assert.strictEqual(model.selected, '');
    assert.deepStrictEqual(model.options.map(o => [o.value, o.disabled]),
                           [['', true], ['sn:1a2b3c4d5e6f7081', false], ['manual', false]]);
    assert.strictEqual(model.options[1].label, 'ZWO ASI432MM (index 0), serial 1a2b3c4d5e6f7081');
    assert.deepStrictEqual(zwoCameraSelection('sn:1a2b3c4d5e6f7081', [MM432], null),
                           { cameraIndex: 0, serialNumber: '1a2b3c4d5e6f7081', cameraName: 'ZWO ASI432MM' });
    assert.strictEqual(zwoCameraSelection(model.selected, [MM432], null), null);
});

test('a camera another entry holds by serial is shown but cannot be picked', () => {
    const taken = ['0f0e0d0c0b0a0908'];
    const model = zwoCameraOptions([MINI174, { ...MM432, Index: 1 }], null, 'sn:0f0e0d0c0b0a0908', taken);
    assert.strictEqual(model.options[1].label,
                       'ZWO ASI174MM Mini (index 0), serial 0f0e0d0c0b0a0908, used by another camera entry');
    assert.strictEqual(model.options[1].disabled, true);
    assert.strictEqual(model.selected, '');
    // A camera with no serial is never claimed: the serial is what an entry binds by.
    assert.strictEqual(zwoCameraOptions([MINI120], null, '', ['']).options[1].disabled, false);
});

test('a camera with no serial is saved by index and model name', () => {
    assert.deepStrictEqual(zwoCameraSelection('cam:1', [MINI120], null),
                           { cameraIndex: 1, cameraName: 'ZWO ASI120MM Mini' });
    assert.strictEqual(zwoCameraOptions([MINI120], null, '', []).options[1].label,
                       'ZWO ASI120MM Mini (index 1), no serial');
});

test('editing selects the saved camera by serial even after it moved index', () => {
    const saved = { cameraIndex: 2, serialNumber: '1a2b3c4d5e6f7081', cameraName: 'ZWO ASI432MM' };
    assert.strictEqual(zwoCameraOptions([MM432], saved, '', []).selected, 'sn:1a2b3c4d5e6f7081');
});

test('a Refresh that moves the picked camera to another slot keeps that camera picked', () => {
    // Plugging the ASI174MM Mini in puts it at slot 0 and moves the ASI432MM to slot 1.
    const moved = [MINI174, { ...MM432, Index: 1 }];
    const model = zwoCameraOptions(moved, null, 'sn:1a2b3c4d5e6f7081', []);
    assert.strictEqual(model.selected, 'sn:1a2b3c4d5e6f7081');
    assert.deepStrictEqual(zwoCameraSelection(model.selected, moved, null),
                           { cameraIndex: 1, serialNumber: '1a2b3c4d5e6f7081', cameraName: 'ZWO ASI432MM' });
});

test('an edited entry whose camera is not attached keeps its saved binding', () => {
    const saved = { cameraIndex: 2, serialNumber: '1a2b3c4d5e6f7081', cameraName: 'ZWO ASI432MM' };
    const model = zwoCameraOptions([MINI174], saved, '', []);
    assert.strictEqual(model.selected, 'saved');
    assert.strictEqual(model.options.find(o => o.value === 'saved').label,
                       'Saved camera, serial 1a2b3c4d5e6f7081 (not attached)');
    assert.deepStrictEqual(zwoCameraSelection('saved', [MINI174], saved), saved);
    assert.strictEqual(zwoCameraHint([MINI174], 'saved', '', saved), 'The saved camera is not attached. Saving keeps it.');
});

test('an entry that never learned a serial says so and can be rebound by picking', () => {
    const saved = { cameraIndex: 1 };
    const model = zwoCameraOptions([MM432], saved, '', []);
    assert.strictEqual(model.selected, 'saved');
    assert.strictEqual(model.options.find(o => o.value === 'saved').label, 'Saved entry, index 1, no serial learned');
    assert.strictEqual(zwoCameraHint([MM432], 'saved', '', saved),
                       "This entry never learned its camera's serial, so it binds whichever camera sits at index 1. " +
                       'Pick the camera to bind it by serial.');
    assert.deepStrictEqual(zwoCameraSelection('saved', [MM432], saved), { cameraIndex: 1 });
    assert.strictEqual(zwoCameraOptions([MM432], saved, 'sn:1a2b3c4d5e6f7081', []).selected, 'sn:1a2b3c4d5e6f7081');
    assert.strictEqual(zwoCameraOptions([MM432], { cameraId: 4 }, '', []).options.find(o => o.value === 'saved').label,
                       'Saved entry, camera ID 4, no serial learned');
});

test('the by-hand fallback is always offered, and reads the index boxes', () => {
    assert.deepStrictEqual(zwoCameraSelection('manual', [], null), { manual: true });
    assert.strictEqual(zwoCameraOptions(null, null, '', []).options.at(-1).value, 'manual');
    assert.match(zwoCameraHint([], 'manual', '', null), /^For a camera that is not plugged in yet\./);
});

test('the hint explains an empty or failed listing', () => {
    assert.strictEqual(zwoCameraHint([], '', '', null),
                       'No ZWO camera is attached. Plug it in and press Refresh, or enter the index by hand.');
    assert.strictEqual(zwoCameraHint(null, '', 'no usb', null),
                       'Could not list the attached cameras: no usb. Plug the camera in and press Refresh, ' +
                       'or enter the index by hand.');
    assert.strictEqual(zwoCameraHint([MM432], '', '', null), 'Choose the camera this entry is for.');
    assert.deepStrictEqual(zwoCameraOptions(null, null, '', []).options.map(o => o.value), ['', 'manual']);
});

test('configureWarning reads the warning out of a configuredevice reply', () => {
    assert.strictEqual(configureWarning('Device registered successfully. Warning: no serial'), 'no serial');
    assert.strictEqual(configureWarning('Device registered successfully'), '');
    assert.strictEqual(configureWarning(undefined), '');
});

// The app.js functions below run over element stubs, as in sky_watcher_vendor.test.js.
function wholeFunction(name) {
    const start = app.indexOf(`function ${name}(`);
    assert.ok(start >= 0, `app.js has no function ${name}`);
    return `${app.slice(start, app.indexOf('\n}\n', start))}\n}\n`;
}

// The data-* attributes of the element with `id` in index.html, as its dataset.
function htmlDataset(id) {
    const tag = html.match(new RegExp(`<[a-z]+ [^>]*id="${id}"[^>]*>`));
    assert.ok(tag, `index.html has no element with id="${id}"`);
    const dataset = {};
    for (const [, key, value] of tag[0].matchAll(/ data-([a-z-]+)="([^"]*)"/g)) {
        dataset[key.replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = value;
    }
    return dataset;
}

test('the by-hand ZWO camera index is not auto-numbered from the saved cameras', () => {
    // Two ZWO camera entries are saved; a third is added with "Enter the index by hand".
    const elements = {
        'device-form': { dataset: { editing: 'false' } },
        vendor: { value: 'zwo' },
        'device-type': { value: 'camera' },
        'camera-index': { value: '0', dataset: htmlDataset('camera-index') },
        'other-index': { value: '0', dataset: {} },
    };
    const field = id => ({ fieldId: id, vendor: 'zwo', deviceType: 'camera', configKey: 'cameraIndex' });
    const camera = (n, index) => ({ DeviceType: 'Camera', DeviceNumber: n, Vendor: 'zwo', Config: { cameraIndex: index } });
    const scope = {
        document: { getElementById: id => elements[id] ?? null },
        indexFields: [field('camera-index'), field('other-index')],
        currentDevices: [camera(0, 0), camera(1, 1)],
    };
    const names = ['normalizeDeviceType', 'extractDeviceConfig', 'extractDeviceType', 'extractVendor',
                   'getNextAvailableNumber', 'getNextIndexForField', 'maybeAutoFillIndexFields'];
    const fns = new Function('scope', `with (scope) { ${names.map(wholeFunction).join('')} return { ${names} }; }`)(scope);
    fns.maybeAutoFillIndexFields();
    assert.strictEqual(elements['camera-index'].value, '0');
    // An index box without the opt-out is still numbered past the saved ones.
    assert.strictEqual(elements['other-index'].value, 2);
});

// The device form's submit handler for a new ZWO camera whose picker returns `pick`,
// with the hidden index and ID boxes holding `boxes`. Resolves to the posted bodies
// and the alerts shown.
async function submitZwoCamera(pick, boxes) {
    const fields = { deviceType: 'camera', deviceNumber: '2', vendor: 'zwo', ...boxes };
    class FormData {
        get(name) { return fields[name] ?? null; }
        has(name) { return name in fields; }
    }
    const posts = [];
    const alerts = [];
    const fetch = async (url, init) => {
        posts.push({ url, body: JSON.parse(init.body) });
        return { json: async () => ({ ErrorNumber: 0, Value: 'Device registered successfully' }) };
    };
    const elements = { 'form-message': { style: {} } };
    const marker = "document.getElementById('device-form').addEventListener('submit', ";
    const start = app.indexOf(marker);
    assert.ok(start >= 0, 'app.js has no device-form submit handler');
    const handler = app.slice(start + marker.length, app.indexOf('\n});\n', start) + 2);
    const helpers = ['normalizeDeviceType', 'readOptionalNumber', 'zwoCameraIdentityFields'].map(wholeFunction).join('');
    const scope = {
        ...format, document: { getElementById: id => elements[id] ?? null }, FormData, fetch, API_BASE: '',
        alert: text => alerts.push(text), resetDeviceForm: () => {}, setTimeout: () => {}, loadDevices: () => {},
        showTab: () => {}, zwoCameraPicker: { read: () => pick, missingMessage: 'pick one' },
    };
    const submit = new Function('scope', `with (scope) { ${helpers} return (${handler}); }`)(scope);
    await submit.call({ dataset: { editing: 'false' } }, { preventDefault: () => {} });
    return { posts, alerts };
}

test('a picked ZWO camera replaces the index and ID boxes in the saved entry', async () => {
    const { posts } = await submitZwoCamera(
        { cameraIndex: 0, serialNumber: '1a2b3c4d5e6f7081', cameraName: 'ZWO ASI432MM' },
        { cameraIndex: '2', cameraId: '4' });
    assert.strictEqual(posts.length, 1);
    const body = posts[0].body;
    assert.strictEqual(posts[0].url, '/management/v1/configuredevice');
    assert.strictEqual(body.cameraIndex, 0);
    assert.strictEqual(body.serialNumber, '1a2b3c4d5e6f7081');
    assert.strictEqual(body.cameraName, 'ZWO ASI432MM');
    assert.ok(!('cameraId' in body), 'the hidden camera ID box was sent beside the picked serial');
});

test('the by-hand ZWO camera choice sends the index and ID boxes, and no pick refuses the save', async () => {
    const manual = (await submitZwoCamera({ manual: true }, { cameraIndex: '2', cameraId: '4' })).posts[0].body;
    assert.strictEqual(manual.cameraIndex, 2);
    assert.strictEqual(manual.cameraId, 4);
    assert.ok(!('serialNumber' in manual));
    const refused = await submitZwoCamera(null, { cameraIndex: '0' });
    assert.deepStrictEqual(refused.alerts, ['pick one']);
    assert.strictEqual(refused.posts.length, 0);
});
