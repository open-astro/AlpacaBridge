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

// Pins the Celestron focus motor calibration helpers (issue #1411). The status
// strings are the ones the driver's CalibrationStatus action returns.
//
// Run: node --test AlpacaHTTP/tests/web/celestron_calibration.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const f = require('../../web/format.js');

const app = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'app.js'), 'utf8');

test('each driver status string parses to its state', () => {
    assert.equal(f.parseCalibrationStatus('Calibrating'), 'calibrating');
    assert.equal(f.parseCalibrationStatus('Calibrated'), 'calibrated');
    assert.equal(f.parseCalibrationStatus('NotCalibrated'), 'not-calibrated');
});

test('anything else parses to unknown', () => {
    for (const v of ['', 'calibrating', 'Idle', 'toString', undefined, null, 5, {}]) {
        assert.equal(f.parseCalibrationStatus(v), 'unknown');
    }
});

test('Abort is offered only while a run is in progress', () => {
    assert.equal(f.mayAbortCalibration('calibrating'), true);
    for (const s of ['calibrated', 'not-calibrated', 'unknown', '', undefined]) {
        assert.equal(f.mayAbortCalibration(s), false, String(s));
    }
});

test('a status error never offers Abort', () => {
    const state = f.parseCalibrationStatus(undefined);
    assert.equal(f.mayAbortCalibration(state), false);
    assert.match(f.calibrationStatusText(state, 'Focuser not connected'), /unavailable: Focuser not connected/);
});

test('isCalibrationRunning and mayStartCalibration', () => {
    assert.equal(f.isCalibrationRunning('calibrating'), true);
    assert.equal(f.isCalibrationRunning('calibrated'), false);
    assert.equal(f.mayStartCalibration('calibrated'), true);
    assert.equal(f.mayStartCalibration('not-calibrated'), true);
    assert.equal(f.mayStartCalibration('calibrating'), false);
    assert.equal(f.mayStartCalibration('unknown'), false);
});

test('status text and hint per state', () => {
    assert.match(f.calibrationStatusText('calibrating'), /^Calibrating/);
    assert.equal(f.calibrationStatusText('calibrated'), 'Calibrated.');
    assert.equal(f.calibrationStatusText('not-calibrated'), 'Not calibrated.');
    assert.equal(f.calibrationStatusText('unknown'), 'Calibration status unknown.');
    assert.match(f.calibrationHint('not-calibrated'), /Press Calibrate/);
    for (const s of ['calibrating', 'calibrated', 'unknown']) assert.equal(f.calibrationHint(s), '');
});

test('the confirm text warns about end-to-end travel', () => {
    assert.match(f.calibrationConfirmText(), /one end of its travel to the other/);
});

test('controls show only on Celestron focuser devices', () => {
    assert.equal(f.showsCalibrationControls({ Vendor: 'celestron', DeviceType: 'Focuser' }), true);
    assert.equal(f.showsCalibrationControls({ DeviceType: 'focuser', Config: { vendor: 'celestron' } }), true);
    assert.equal(f.showsCalibrationControls({ Vendor: 'celestron', DeviceType: 'Telescope' }), false);
    assert.equal(f.showsCalibrationControls({ Vendor: 'zwo', DeviceType: 'Focuser' }), false);
    assert.equal(f.showsCalibrationControls(null), false);
});

test('the action body carries the Alpaca form fields', () => {
    const p = new URLSearchParams(f.calibrationActionBody('CalibrationStatus', 7));
    assert.equal(p.get('Action'), 'CalibrationStatus');
    assert.equal(p.get('Parameters'), '');
    assert.equal(p.get('ClientID'), '1');
    assert.equal(p.get('ClientTransactionID'), '7');
});

test('app.js uses the generic focuser action route and guards Abort and polling', () => {
    assert.ok(app.includes("'/api/v1/focuser/' + encodeURIComponent(deviceNumber) + '/action'"));
    assert.ok(app.includes("if (!mayAbortCalibration(state)) return;"));
    assert.ok(app.includes('if (inFlight) return;'));
    assert.ok(app.includes('if (!box.isConnected) return;'));
});

const css = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'style.css'), 'utf8');

test('collapsing a card hides info, settings, actions and calibration controls', () => {
    const rule = css.match(/((?:\.device-card\.collapsed [^{,]+,\s*)+\.device-card\.collapsed [^{,]+)\{([^}]*)\}/);
    assert.ok(rule, 'collapsed rule present');
    for (const sel of ['.device-info', '.device-settings', '.device-actions', '.calibration-controls']) {
        assert.ok(rule[1].includes('.device-card.collapsed ' + sel), sel);
    }
    assert.match(rule[2], /display:\s*none/);
    assert.doesNotMatch(css, /(^|\n)\.device-actions\s*\{[^}]*display:\s*none/);
});

test('a failed status read keeps polling only during a run', () => {
    assert.equal(f.keepPollingCalibration('calibrating', 'unknown', true), true);
    assert.equal(f.keepPollingCalibration('calibrated', 'unknown', true), false);
    assert.equal(f.keepPollingCalibration('unknown', 'unknown', true), false);
    assert.equal(f.keepPollingCalibration('unknown', 'calibrating', false), true);
    assert.equal(f.keepPollingCalibration('calibrating', 'calibrated', false), false);
});

test('Abort is sent only when a fresh status read says Calibrating', () => {
    assert.equal(f.mayAbortFromStatus('Calibrating'), true);
    for (const v of ['Calibrated', 'NotCalibrated', 'junk', undefined, null]) {
        assert.equal(f.mayAbortFromStatus(v), false);
    }
    assert.ok(app.includes("mayAbortFromStatus(await calibrationAction(deviceNumber, 'CalibrationStatus'))"));
});

test('the Not calibrated status line has a warning tint', () => {
    assert.match(f.calibrationStatusClass('not-calibrated'), /calibration-status-warn/);
    assert.equal(f.calibrationStatusClass('calibrated'), 'calibration-status');
    assert.match(css, /\.calibration-status-warn\s*\{[^}]*var\(--warn\)/);
});
