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
// startEditDevice chain showed at upstream main bcb05e4975.
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

test('an absent Discriminator opens on its catalog default', () => {
    const d = descriptor('touptek', 'switch');
    const mode = schema().editAssignments(d, {}, fields).find((a) => a.key === 'switchType');
    assert.equal(mode.value, 'stellavita', 'a ToupTek switch saved before switchType existed is a StellaVita PowerBox');
    assert.equal(shown('touptek', 'switch', {})('touptek-switch-type'), 'stellavita');
    assert.equal(shown('zwo', 'switch', {})('zwo-switch-type'), 'dewheater');
});

test('every Discriminator\'s catalog default is an option of its select', () => {
    const problems = [];
    for (const d of catalog) {
        for (const c of d.fields.filter((x) => x.role === 'Discriminator')) {
            for (const f of fields.filter((x) => x.vendor === d.vendor && x.deviceType === d.deviceType && x.configKey === c.key)) {
                const select = html.slice(html.indexOf(`id="${f.id}"`), html.indexOf('</select>', html.indexOf(`id="${f.id}"`)));
                if (!select.includes(`<option value="${c.default}"`)) problems.push(`${f.id}: no option "${c.default}"`);
            }
        }
    }
    assert.deepEqual(problems, []);
    assert.ok(fields.some((f) => f.configKey === 'connectionType'), 'no select names a Discriminator');
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

test('the fill stays inside the edited device\'s vendor and type', () => {
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

test('startEditDevice keeps no per-field fill for catalog fields', () => {
    for (const id of ['camera-index', 'gemini-flatpanel-v2-port-path', 'qhy-cfw3-port-path', 'wandererastro-box-index',
        'weewx-url', 'touptek-focuser-id', 'skywatcher-backlash-steps-ra', 'gemini-pdh-baud-rate']) {
        assert.ok(!app.includes(`setFormValue('${id}'`), `app.js still fills #${id} by hand`);
    }
});
