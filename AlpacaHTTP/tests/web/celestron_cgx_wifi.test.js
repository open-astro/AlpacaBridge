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

// The Celestron CGX form's USB or SkyPortal WiFi selector and its "Find adapter" button
// (cgxConnectionType / skyPortalAdapterOptions in format.js). The adapter answers 1.2.3.4 on its own hotspot
// network; GET /management/v1/networkadapters?vendor=celestron listens 5 s for its beacon.
//
// Run: node --test AlpacaHTTP/tests/web/celestron_cgx_wifi.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');

const format = require(path.join(__dirname, '..', '..', 'web', 'format.js'));
const { CGX_SKYPORTAL_DEFAULT_HOST, cgxConnectionType, skyPortalAdapterOptions } = format;
const { readWeb } = require('./catalog_form_fields.js');

const app = readWeb('app.js');
const html = readWeb('index.html');

const AFFC = { Host: '1.2.3.4', Ssid: 'Celestron-AFFC', Module: 'Telit WE310G4' };
const SECOND = { Host: '127.0.0.5', Ssid: 'Celestron-BBBB', Module: '' };

test('a CGX entry is WiFi only when it says network; anything else is its USB port', () => {
    assert.equal(cgxConnectionType('network'), 'network');
    for (const saved of ['serial', 'auto', '', undefined, null, 'usb']) {
        assert.equal(cgxConnectionType(saved), 'serial', String(saved));
    }
    // The edit fill and the submit both go through it, so an entry saved before WiFi stays USB.
    assert.ok(app.includes("'cgx-aux' ? cgxConnectionType(config.connectionType) : (config.connectionType || 'auto');"));
    assert.ok(app.includes("? cgxConnectionType(formData.get('celestronConnectionType'))"));
});

test('the CGX form offers USB and WiFi on the shared connection select, never auto-detect', () => {
    // The select is outside the NexStar-only block (which the CGX hides), so the CGX shows it too: before the
    // select, every <div> from the block's own on is closed except the select's form-group.
    const start = html.indexOf('<div id="celestron-nexstar-only">');
    const before = html.slice(start, html.indexOf('id="celestron-connection-type"'));
    assert.ok(start > 0 && before.includes('celestron-alignment-mode'));
    assert.equal((before.match(/<div\b/g) || []).length - (before.match(/<\/div>/g) || []).length, 1);
    assert.match(app, /autoOption\.hidden = directUsb;\s*autoOption\.disabled = directUsb;/);
    assert.ok(app.includes('celestronConnectionType.value = cgxConnectionType(celestronConnectionType.value);'));
});

test('CGX WiFi starts at the adapter hotspot address and port 2000', () => {
    assert.equal(CGX_SKYPORTAL_DEFAULT_HOST, '1.2.3.4');
    assert.ok(app.includes('if (cgxWifi && host && !host.value.trim()) host.value = CGX_SKYPORTAL_DEFAULT_HOST;'));
    assert.match(html, /id="celestron-tcp-port" name="celestronTcpPort" data-config-key="tcpPort" value="2000"/);
});

test('Find adapter calls the networkadapters route and its list never reaches the submitted form', () => {
    assert.ok(app.includes("'/management/v1/networkadapters?vendor=celestron'"));
    assert.match(html, /<button id="celestron-cgx-find-button" class="btn btn-secondary btn-small" type="button">Find adapter<\/button>/);
    const select = html.match(/<select id="celestron-cgx-adapters"[^>]*>/);
    assert.ok(select, 'no adapter select');
    assert.ok(!select[0].includes('name='), 'the adapter select must have no name');
    assert.ok(app.includes('if (celestronAdapterSelect.value) celestronHostInput.value = celestronAdapterSelect.value;'));
    assert.ok(app.includes('if (model.fill) celestronHostInput.value = model.fill;'));
    assert.ok(app.includes("const celestronHostInput = document.getElementById('celestron-host');"));
});

test('one adapter fills the address at once', () => {
    const model = skyPortalAdapterOptions([AFFC]);
    assert.equal(model.fill, '1.2.3.4');
    assert.deepEqual(model.options, []);
    assert.equal(model.status, 'Found 1.2.3.4 (adapter Celestron-AFFC, Telit WE310G4); its address is filled in.');
});

test('two adapters are listed for the operator to pick, and none is chosen for them', () => {
    const model = skyPortalAdapterOptions([AFFC, SECOND]);
    assert.equal(model.fill, '');
    assert.deepEqual(model.options.map(o => [o.value, o.label, o.disabled]), [
        ['', 'Choose the adapter (2 answered)', true],
        ['1.2.3.4', '1.2.3.4 (adapter Celestron-AFFC, Telit WE310G4)', false],
        ['127.0.0.5', '127.0.0.5 (adapter Celestron-BBBB)', false],
    ]);
    assert.equal(model.status, '2 adapters answered; pick the one on this mount.');
});

test('no adapter and a failed search say what to do and fill nothing', () => {
    const none = skyPortalAdapterOptions([]);
    assert.equal(none.fill, '');
    assert.deepEqual(none.options, []);
    assert.match(none.status, /^No SkyPortal adapter answered in 5 s\. /);
    const failed = skyPortalAdapterOptions(null, 'Celestron SkyPortal beacon: cannot bind UDP port 55555');
    assert.equal(failed.fill, '');
    assert.equal(failed.status, 'Search failed: Celestron SkyPortal beacon: cannot bind UDP port 55555');
    assert.equal(skyPortalAdapterOptions(null).status, 'Search failed: no answer');
});

test('a station-mode adapter is named as the adapter, not as the network it joined', () => {
    // The beacon's Ssid is the adapter's own access point name even while it is a station on another network
    // (rig 2026-11-22: on this board's hotspot it still said Celestron-AFFC).
    const model = skyPortalAdapterOptions([{ Host: '10.0.0.20', Ssid: 'Celestron-AFFC', Module: 'Telit WE310G4' }]);
    assert.equal(model.status, 'Found 10.0.0.20 (adapter Celestron-AFFC, Telit WE310G4); its address is filled in.');
    // With none found, joining the adapter's own network is not the only way: the same network as this board works.
    const none = skyPortalAdapterOptions([]).status;
    assert.match(none, /adapter's own WiFi \(Celestron-XXXX\)/);
    assert.match(none, /same network as this board/);
    assert.match(none, /hotspot/);
});

test('an adapter with only a module is labelled by it', () => {
    assert.equal(skyPortalAdapterOptions([{ Host: '10.0.0.9', Ssid: '', Module: 'Telit WE310G4' }]).status,
                 'Found 10.0.0.9 (Telit WE310G4); its address is filled in.');
});
