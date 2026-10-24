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

// Drift check between the hand-written device form in index.html and the
// device catalog (decision 0004: the form layout stays hand-written and a
// drift check compares its fields to the catalog). Each named form field sits
// in a data-vendor / data-device-type scope and names its catalog field with
// data-config-key; every catalog field has a form field, through a control
// that fits its type and role. The exceptions, each
// with its reason, are in catalog_form_drift.json. The catalog side is the
// committed fixture tests/fixtures/devicecatalog.json, which test_routing.cpp
// holds equal to what the server serves.
//
// Run: node --test AlpacaHTTP/tests/web/catalog_form_drift.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const { readWeb, readCatalogFixture, routerAgnosticKeys, formFields, vendorOptions } =
    require('./catalog_form_fields.js');

const exemptions = JSON.parse(fs.readFileSync(path.join(__dirname, 'catalog_form_drift.json'), 'utf8'));
const catalog = readCatalogFixture();
const html = readWeb('index.html');
const fields = formFields(html);
const agnostic = routerAgnosticKeys();

const fieldKey = (vendor, deviceType, key) => `${vendor}/${deviceType}/${key}`;

// A named form field that maps to nothing: outside every vendor scope with a
// name the router does not keep for every device, or inside one with no
// data-config-key, or with a key its descriptor does not declare.
function namedFieldProblems(formFieldList, descriptors, exempt, agnosticKeys) {
    const problems = [];
    for (const f of formFieldList) {
        if (!f.name) continue;
        const where = `${f.id || '(no id)'} name="${f.name}"`;
        if (f.vendor === null) {
            if (!agnosticKeys.includes(f.name)) problems.push(`${where}: outside every data-vendor scope`);
            continue;
        }
        if (Object.hasOwn(exempt.uiOnly, f.id)) continue;
        if (f.deviceType === null) {
            problems.push(`${where}: no data-device-type in scope`);
            continue;
        }
        if (!f.configKey) {
            problems.push(`${where}: no data-config-key and not listed in uiOnly`);
            continue;
        }
        const d = descriptors.find((x) => x.vendor === f.vendor && x.deviceType === f.deviceType);
        if (!d) {
            problems.push(`${where}: no catalog descriptor ${f.vendor}/${f.deviceType}`);
        } else if (!d.fields.some((c) => c.key === f.configKey) && !agnosticKeys.includes(f.configKey)) {
            problems.push(`${where}: ${fieldKey(f.vendor, f.deviceType, f.configKey)} is not a catalog field`);
        }
    }
    return problems;
}

// A catalog field no form element names, and no exemption explains.
function catalogFieldProblems(formFieldList, descriptors, exempt) {
    const problems = [];
    for (const d of descriptors) {
        if (exempt.testVendors.includes(d.vendor)) continue;
        for (const c of d.fields) {
            const k = fieldKey(d.vendor, d.deviceType, c.key);
            const mapped = formFieldList.some((f) =>
                f.vendor === d.vendor && f.deviceType === d.deviceType && f.configKey === c.key);
            if (!mapped && !Object.hasOwn(exempt.noFormField, k)) {
                problems.push(`${k}: no form field has data-config-key="${c.key}" and noFormField does not list it`);
            }
        }
    }
    return problems;
}

// Self-test of the reader and both checks on a literal form, run first in each
// check case so a reader or check that reports nothing cannot pass vacuously
// (decision 0003: extractor logic has literal self-test fixtures).
function assertChecksCatchDrift() {
    const snippet = `
        <form id="device-form">
          <select id="vendor" name="vendor"><option value="">Select vendor...</option></select>
          <!-- <div data-vendor="ghost"> a commented-out block is not a scope -->
          <div id="acme-config" data-vendor="acme">
            <div id="acme-camera-fields" data-device-type="camera">
              <input type="number" id="acme-camera-index" name="acmeCameraIndex" data-config-key="cameraIndex" value="0">
              <input type="text" id="acme-camera-new" name="acmeCameraNew">
            </div>
            <select id="acme-switch-type" name="acmeSwitchType" data-device-type="switch" data-config-key="switchType">
              <option value="a">A</option><option value="b" selected>B</option>
            </select>
          </div>
          <input type="text" id="stray" name="stray">
        </form>`;
    const parsed = formFields(snippet);
    const index = parsed.find((f) => f.id === 'acme-camera-index');
    assert.equal(index.vendor, 'acme');
    assert.equal(index.deviceType, 'camera');
    assert.equal(index.configKey, 'cameraIndex');
    assert.deepEqual(index.path, ['acme-config', 'acme-camera-fields']);
    const switchType = parsed.find((f) => f.id === 'acme-switch-type');
    assert.equal(switchType.deviceType, 'switch', 'the element\'s own data-device-type wins');
    assert.equal(switchType.markupDefault, 'b');
    assert.equal(parsed.find((f) => f.id === 'stray').vendor, null, 'a closed scope does not leak');

    const cat = [
        { vendor: 'acme', deviceType: 'camera', fields: [{ key: 'cameraIndex' }, { key: 'cameraId' }] },
        { vendor: 'acme', deviceType: 'switch', fields: [{ key: 'switchType' }] },
    ];
    const none = { testVendors: [], uiOnly: {}, noFormField: {} };
    assert.deepEqual(namedFieldProblems(parsed, cat, none, ['vendor']), [
        'acme-camera-new name="acmeCameraNew": no data-config-key and not listed in uiOnly',
        'stray name="stray": outside every data-vendor scope',
    ]);
    assert.deepEqual(catalogFieldProblems(parsed, cat, none), [
        'acme/camera/cameraId: no form field has data-config-key="cameraId" and noFormField does not list it',
    ]);
}

// The control a catalog field needs: its type first (bool, stringList), then
// its role (decision 0004: the drift check compares names and roles). null:
// any input or select, but not a checkbox or a textarea.
function expectedControl(c) {
    if (c.type === 'bool') return 'checkbox';
    if (c.type === 'stringList') return 'textarea';
    if (c.role === 'EnumerationIndex') return 'number';
    if (c.role === 'Discriminator') return 'select';
    if (c.role === 'Secret') return 'password';
    return null;
}

// A mapped form field whose control does not fit its catalog field.
function controlProblems(formFieldList, descriptors) {
    const problems = [];
    for (const f of formFieldList) {
        if (!f.configKey || f.vendor === null) continue;
        const d = descriptors.find((x) => x.vendor === f.vendor && x.deviceType === f.deviceType);
        const c = d && d.fields.find((x) => x.key === f.configKey);
        if (!c || c.type === 'recordList') continue;  // unmapped: the named-field check; port tables: filled by hand
        const expected = expectedControl(c);
        const fits = expected ? f.control === expected : !['checkbox', 'textarea'].includes(f.control);
        if (!fits) {
            problems.push(`${f.id} (${fieldKey(f.vendor, f.deviceType, c.key)}, ${c.type} ${c.role}): ` +
                `control ${f.control}, expected ${expected || 'an input or select'}`);
        }
    }
    return problems;
}

// Self-test of controlProblems on a literal form: one field breaks each rule.
function assertControlRulesCatchDrift() {
    const snippet = `
        <form id="device-form">
          <div id="acme-config" data-vendor="acme" data-device-type="focuser">
            <input type="text" id="acme-index" name="acmeIndex" data-config-key="focuserIndex">
            <input type="text" id="acme-mode" name="acmeMode" data-config-key="mode">
            <input type="text" id="acme-token" name="acmeToken" data-config-key="token">
            <select id="acme-reverse" name="acmeReverse" data-config-key="reverse"><option value="1">On</option></select>
            <input type="text" id="acme-names" name="acmeNames" data-config-key="names">
            <textarea id="acme-note" name="acmeNote" data-config-key="note"></textarea>
            <input type="number" id="acme-speed" name="acmeSpeed" data-config-key="speed">
            <input type="password" id="acme-secret" name="acmeSecret" data-config-key="secret">
          </div>
        </form>`;
    const cat = [{ vendor: 'acme', deviceType: 'focuser', fields: [
        { key: 'focuserIndex', type: 'int', role: 'EnumerationIndex' },
        { key: 'mode', type: 'string', role: 'Discriminator' },
        { key: 'token', type: 'string', role: 'Secret' },
        { key: 'reverse', type: 'bool', role: 'Plain' },
        { key: 'names', type: 'stringList', role: 'Plain' },
        { key: 'note', type: 'string', role: 'Plain' },
        { key: 'speed', type: 'int', role: 'Plain' },
        { key: 'secret', type: 'string', role: 'Secret' },
    ] }];
    assert.deepEqual(controlProblems(formFields(snippet), cat), [
        'acme-index (acme/focuser/focuserIndex, int EnumerationIndex): control text, expected number',
        'acme-mode (acme/focuser/mode, string Discriminator): control text, expected select',
        'acme-token (acme/focuser/token, string Secret): control text, expected password',
        'acme-reverse (acme/focuser/reverse, bool Plain): control select, expected checkbox',
        'acme-names (acme/focuser/names, stringList Plain): control text, expected textarea',
        'acme-note (acme/focuser/note, string Plain): control textarea, expected an input or select',
    ]);
}

test('every named form field maps to a catalog field, a key the router keeps for every device, or a listed UI-only id', () => {
    assertChecksCatchDrift();
    assert.deepEqual(namedFieldProblems(fields, catalog, exemptions, agnostic), []);
});

test('every catalog field has a form field or a listed reason', () => {
    assertChecksCatchDrift();
    assert.deepEqual(catalogFieldProblems(fields, catalog, exemptions), []);
});

test('the vendor select lists exactly the catalog vendors', () => {
    const catalogVendors = [...new Set(catalog.map((d) => d.vendor))]
        .filter((v) => !exemptions.testVendors.includes(v)).sort();
    assert.deepEqual([...new Set(vendorOptions(html))].sort(), catalogVendors);
});

test('every exemption in catalog_form_drift.json is still needed', () => {
    const stale = [];
    for (const id of Object.keys(exemptions.uiOnly)) {
        const f = fields.find((x) => x.id === id);
        if (!f || !f.name) stale.push(`uiOnly ${id}: no named form field has this id`);
        else if (f.configKey) stale.push(`uiOnly ${id}: has data-config-key="${f.configKey}"`);
    }
    for (const k of Object.keys(exemptions.noFormField)) {
        const [vendor, deviceType, key] = k.split('/');
        const d = catalog.find((x) => x.vendor === vendor && x.deviceType === deviceType);
        if (!d || !d.fields.some((c) => c.key === key)) stale.push(`noFormField ${k}: not a catalog field`);
        else if (fields.some((f) => f.vendor === vendor && f.deviceType === deviceType && f.configKey === key)) {
            stale.push(`noFormField ${k}: a form field now names it`);
        }
    }
    for (const v of exemptions.testVendors) {
        if (!catalog.some((d) => d.vendor === v && d.available === false)) {
            stale.push(`testVendors ${v}: not an unavailable catalog vendor`);
        }
    }
    assert.deepEqual(stale, []);
});

test('each form field control fits its catalog field type and role', () => {
    assertControlRulesCatchDrift();
    assert.deepEqual(controlProblems(fields, catalog), []);
    const mapped = (pick) => fields.some((f) => {
        const d = catalog.find((x) => x.vendor === f.vendor && x.deviceType === f.deviceType);
        const c = d && f.configKey && d.fields.find((x) => x.key === f.configKey);
        return c && pick(c);
    });
    for (const type of ['bool', 'int', 'double', 'string', 'stringList', 'recordList']) {
        assert.ok(mapped((c) => c.type === type), `no form field maps a catalog ${type} field`);
    }
    for (const role of ['EnumerationIndex', 'Discriminator', 'DeviceId', 'PortPath', 'Host']) {
        assert.ok(mapped((c) => c.role === role), `no form field maps a catalog ${role} field`);
    }
});

// The edit form opens an absent Discriminator on its catalog default, so the
// select must offer that value.
test('every Discriminator catalog default is an option of its select', () => {
    const problems = [];
    for (const d of catalog) {
        for (const c of d.fields.filter((x) => x.role === 'Discriminator')) {
            for (const f of fields.filter((x) => x.vendor === d.vendor && x.deviceType === d.deviceType && x.configKey === c.key)) {
                const at = html.indexOf(`id="${f.id}"`);
                const select = html.slice(at, html.indexOf('</select>', at));
                if (!select.includes(`<option value="${c.default}"`)) problems.push(`${f.id}: no option "${c.default}"`);
            }
        }
    }
    assert.deepEqual(problems, []);
    assert.ok(fields.some((f) => f.configKey === 'connectionType'), 'no select names a Discriminator');
});
