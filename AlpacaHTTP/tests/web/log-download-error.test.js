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


// downloadAllLogs and downloadLogFile (app.js): the server answers a refused
// or failed download with HTTP 200 and a JSON error body. The UI shows the
// ErrorMessage and saves no file. Runs the two real functions from app.js
// against a stub DOM and fetch.
//
// Run: node --test AlpacaHTTP/tests/web/log-download-error.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const APP_JS = path.join(__dirname, '../../web/app.js');
const BUSY = 'Log archive build already in progress; retry shortly';

function slice(src, from, to) {
    return src.slice(src.indexOf(from), src.indexOf(to));
}

function load() {
    const src = fs.readFileSync(APP_JS, 'utf8');
    const code = slice(src, 'async function downloadAllLogs()', '// Log file management') +
        slice(src, 'async function downloadLogFile(', 'async function deleteAllLogFiles');
    const statuses = [];
    const clicks = [];
    const ctx = {
        API_BASE: '',
        LOG_FILES_ENDPOINT: '/management/v1/logfiles',
        setLogFilesStatus: (t) => statuses.push(t),
        fetch: async () => ({
            ok: true,
            headers: { get: () => 'application/json' },
            json: async () => ({ ErrorNumber: 0x40B, ErrorMessage: BUSY }),
            blob: async () => { throw new Error('blob read'); },
        }),
        URL: { createObjectURL: () => 'blob:x', revokeObjectURL: () => {} },
        document: {
            createElement: () => ({ click: () => clicks.push(1), remove() {} }),
            body: { appendChild() {} },
        },
    };
    vm.createContext(ctx);
    vm.runInContext(code + '\n;globalThis.__all = downloadAllLogs; globalThis.__one = downloadLogFile;', ctx);
    return { ctx, statuses, clicks };
}

// Falsified by: app.js downloadAllLogs, delete the application/json check.
test('downloadAllLogs shows the JSON ErrorMessage and saves no file', async () => {
    const { ctx, statuses, clicks } = load();
    await ctx.__all();
    assert.strictEqual(statuses[statuses.length - 1], `Failed to download logs: ${BUSY}`);
    assert.strictEqual(clicks.length, 0);
});

// Falsified by: app.js downloadLogFile, delete the application/json check.
test('downloadLogFile shows the JSON ErrorMessage and saves no file', async () => {
    const { ctx, statuses, clicks } = load();
    await ctx.__one('2026-01-01.log');
    assert.strictEqual(statuses[statuses.length - 1], `Failed to download 2026-01-01.log: ${BUSY}`);
    assert.strictEqual(clicks.length, 0);
});
