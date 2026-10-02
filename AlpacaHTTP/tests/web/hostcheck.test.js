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

// Unit tests for the Host check settings rows of the server settings area
// (hostCheckSettings, settingsSaveError, HOST_CHECK_ALWAYS_ALLOWED) in
// format.js.
//
// Run: node --test AlpacaHTTP/tests/web/hostcheck.test.js
//
// The case that earns this file is the refused save: the server answers a
// PUT that would lock the browser out with HTTP 400 and the reason in
// ErrorMessage, and a fetch wrapper that throws on !response.ok before
// reading the body shows "HTTP error! status: 400" instead of the one line
// that tells the operator what to add to the list.

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const { hostCheckSettings, settingsSaveError, HOST_CHECK_ALWAYS_ALLOWED } =
    require(path.join(__dirname, '..', '..', 'web', 'format.js'));

test('a description without the fields renders no Host check rows', () => {
    // An older server: no rows, rather than a toggle that reads as "off".
    assert.strictEqual(hostCheckSettings({ ServerName: 'AlpacaBridge' }), null);
    assert.strictEqual(hostCheckSettings(undefined), null);
    assert.strictEqual(hostCheckSettings(null), null);
    assert.strictEqual(hostCheckSettings({ HostCheckEnabled: 'true', AllowedHosts: '' }), null);
});

test('the rows load the current values', () => {
    assert.deepStrictEqual(
        hostCheckSettings({
            HostCheckEnabled: true,
            AllowedHosts: '.lan, astropi.home',
            HostCheckEnabledFixedByEnvironment: false,
            AllowedHostsFixedByEnvironment: false,
        }),
        { enabled: true, hosts: '.lan, astropi.home', enabledFixed: false, hostsFixed: false });
    assert.deepStrictEqual(
        hostCheckSettings({ HostCheckEnabled: false }),
        { enabled: false, hosts: '', enabledFixed: false, hostsFixed: false });
});

test('a field fixed by the environment is marked read-only, each on its own', () => {
    assert.deepStrictEqual(
        hostCheckSettings({
            HostCheckEnabled: true,
            AllowedHosts: '.lan',
            HostCheckEnabledFixedByEnvironment: true,
            AllowedHostsFixedByEnvironment: false,
        }),
        { enabled: true, hosts: '.lan', enabledFixed: true, hostsFixed: false });
    assert.deepStrictEqual(
        hostCheckSettings({
            HostCheckEnabled: false,
            AllowedHosts: '',
            HostCheckEnabledFixedByEnvironment: false,
            AllowedHostsFixedByEnvironment: true,
        }),
        { enabled: false, hosts: '', enabledFixed: false, hostsFixed: true });
    // Only a literal true fixes a field.
    assert.strictEqual(
        hostCheckSettings({ HostCheckEnabled: true, HostCheckEnabledFixedByEnvironment: 'false' }).enabledFixed,
        false);
});

test('a refused save shows the server message, not the status code', () => {
    const refusal = "Host 'astropi.lan' would be refused by these settings; add it to the allowed host names or use the IP address";
    assert.strictEqual(settingsSaveError(400, { ErrorNumber: 1025, ErrorMessage: refusal }), refusal);
    assert.strictEqual(
        settingsSaveError(400, { ErrorNumber: 1025, ErrorMessage: '  AllowedHosts is fixed  ' }),
        'AllowedHosts is fixed');
    // HTTP 200 with an Alpaca error is the same case.
    assert.strictEqual(settingsSaveError(200, { ErrorNumber: 1280, ErrorMessage: 'Failed to persist' }),
                       'Failed to persist');
});

test('a saved setting is no error', () => {
    assert.strictEqual(settingsSaveError(200, { ErrorNumber: 0, ErrorMessage: '' }), '');
});

test('a reply with no usable body still says something', () => {
    assert.strictEqual(settingsSaveError(400, null), 'HTTP error! status: 400');
    assert.strictEqual(settingsSaveError(403, { ErrorNumber: 1025, ErrorMessage: '' }), 'HTTP error! status: 403');
    assert.strictEqual(settingsSaveError(502, 'Bad Gateway'), 'HTTP error! status: 502');
    assert.strictEqual(settingsSaveError(200, { ErrorNumber: 1280 }), 'Server error 1280');
    // HTTP 200 with a body that is not an Alpaca envelope is not a save.
    assert.strictEqual(settingsSaveError(200, null), 'Unknown server error');
});

test('the help line lists the names default.yaml says are always allowed', () => {
    assert.strictEqual(typeof HOST_CHECK_ALWAYS_ALLOWED, 'string');
    const yaml = fs.readFileSync(path.join(__dirname, '..', '..', 'config', 'default.yaml'), 'utf8');
    assert.ok(yaml.includes('Always allowed: ' + HOST_CHECK_ALWAYS_ALLOWED + '.'),
              'HOST_CHECK_ALWAYS_ALLOWED differs from the allowed_hosts comment in default.yaml');
});
