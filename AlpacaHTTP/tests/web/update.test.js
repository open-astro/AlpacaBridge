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

// Unit tests for the Software Update card's text rules (updateStatusText,
// installerStateText) in format.js -- docs/software-update.md.
//
// Run: node --test AlpacaHTTP/tests/web/update.test.js
//
// The server decides UpdateAvailable with dpkg's ordering; the browser only
// words it. The case that earns this file is the dev-build one: a build
// NEWER than the repository must read as up to date rather than offering the
// older release as an "update", and the wording must say why.

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

const { updateStatusText, installerStateText, renderReleaseNotes } =
    require(path.join(__dirname, '..', '..', 'web', 'format.js'));

test('nothing checked yet', () => {
    assert.strictEqual(updateStatusText({ InstalledVersion: '4.1.0', LatestVersion: null, UpdateAvailable: false }),
                       'Not checked yet.');
    assert.strictEqual(updateStatusText(undefined), 'Not checked yet.');
});

test('a newer release is offered by number', () => {
    assert.strictEqual(
        updateStatusText({ InstalledVersion: '4.1.0', LatestVersion: '4.2.0', UpdateAvailable: true }),
        'Version 4.2.0 is available.');
});

test('the same release is up to date', () => {
    assert.strictEqual(
        updateStatusText({ InstalledVersion: '4.1.0', LatestVersion: '4.1.0', UpdateAvailable: false }),
        'Up to date.');
});

test('a dev build newer than the repository is up to date and says so', () => {
    const text = updateStatusText({ InstalledVersion: '4.2.0', LatestVersion: '4.1.0', UpdateAvailable: false });
    assert.match(text, /^Up to date\./);
    assert.match(text, /4\.2\.0/);
    assert.match(text, /4\.1\.0/);
    assert.doesNotMatch(text, /available/);
});

test('a failed check wins over a stale result', () => {
    assert.strictEqual(
        updateStatusText({ InstalledVersion: '4.1.0', LatestVersion: '4.2.0', UpdateAvailable: true,
                           CheckError: 'Update check failed: Could not resolve host: apt.openastro.net' }),
        'Check failed: Update check failed: Could not resolve host: apt.openastro.net');
});

test('installer states', () => {
    assert.strictEqual(installerStateText({ State: 'idle' }), '');
    assert.strictEqual(installerStateText(undefined), '');
    assert.match(installerStateText({ State: 'running' }), /Installing/);
    assert.match(installerStateText({ State: 'succeeded' }), /finished successfully/);
    assert.strictEqual(installerStateText({ State: 'failed' }), 'The last update failed.');
    assert.strictEqual(installerStateText({ State: 'failed', Detail: 'ActiveState=failed Result=exit-code' }),
                       'The last update failed. (ActiveState=failed Result=exit-code)');
    assert.strictEqual(
        installerStateText({ State: 'unavailable', Detail: 'alpacabridge-update.service is not installed' }),
        'Installing from this page is not available on this host: alpacabridge-update.service is not installed');
    assert.strictEqual(installerStateText({ State: 'unavailable' }),
                       'Installing from this page is not available on this host.');
});

// --- renderReleaseNotes: the docs/releases subset, and nothing from the
// network can become markup.

test('renders the release-notes subset and drops the title line', () => {
    const md = [
        '# AlpacaBridge 4.2.0',
        '',
        'One line summary with **bold** and `code`.',
        '',
        '## Read this first',
        '',
        '- **First bullet.** Details [here](https://example.org/x).',
        '- Second bullet',
        '  wrapped onto a second line.',
        '',
        '```sh',
        'sudo apt update && sudo apt upgrade alpacabridge',
        '```',
        '',
    ].join('\n');
    const html = renderReleaseNotes(md);
    assert.doesNotMatch(html, /AlpacaBridge 4\.2\.0/);
    assert.match(html, /<p>One line summary with <strong>bold<\/strong> and <code>code<\/code>\.<\/p>/);
    assert.match(html, /<h5>Read this first<\/h5>/);
    assert.match(html, /<ul>\n<li><strong>First bullet\.<\/strong> Details <a href="https:\/\/example\.org\/x" target="_blank" rel="noopener">here<\/a>\.<\/li>/);
    assert.match(html, /<li>Second bullet wrapped onto a second line\.<\/li>\n<\/ul>/);
    assert.match(html, /<pre><code>sudo apt update &amp;&amp; sudo apt upgrade alpacabridge<\/code><\/pre>/);
});

test('a second top-level heading is rendered, only the first is the title', () => {
    const html = renderReleaseNotes('# Title\n\n# Another\n\ntext');
    assert.match(html, /<h4>Another<\/h4>/);
    assert.doesNotMatch(html, /Title/);
});

test('markup in the notes is text, never tags', () => {
    const html = renderReleaseNotes('- <script>alert(1)</script> and <img src=x onerror=y>');
    assert.doesNotMatch(html, /<script>/);
    assert.doesNotMatch(html, /<img/);
    assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt;/);
});

test('only http(s) links become anchors', () => {
    assert.match(renderReleaseNotes('[ok](https://a.example/p)'), /<a href="https:\/\/a\.example\/p"/);
    assert.match(renderReleaseNotes('[ok](http://a.example/p)'), /<a href="http:\/\/a\.example\/p"/);
    const bad = renderReleaseNotes('[no](javascript:alert(1)) [no](data:text/html,x)');
    assert.doesNotMatch(bad, /<a /);
    assert.match(bad, /javascript:alert/);
});

test('empty or missing notes render nothing', () => {
    assert.strictEqual(renderReleaseNotes(''), '');
    assert.strictEqual(renderReleaseNotes(undefined), '');
    assert.strictEqual(renderReleaseNotes('# Only a title\n'), '');
});

test('an unterminated fence still renders as code', () => {
    assert.match(renderReleaseNotes('```\nx < y\n'), /<pre><code>x &lt; y<\/code><\/pre>/);
});

test('markdown inside a code span stays literal', () => {
    const html = renderReleaseNotes('Use `[x](https://a.example)` and `**not bold**` here, but [y](https://b.example) is a link.');
    assert.match(html, /<code>\[x\]\(https:\/\/a\.example\)<\/code>/);
    assert.match(html, /<code>\*\*not bold\*\*<\/code>/);
    assert.match(html, /<a href="https:\/\/b\.example" target="_blank" rel="noopener">y<\/a>/);
    assert.doesNotMatch(html, /<code>[^<]*<a /);
});

test('a NUL byte in the notes cannot reach the code-span placeholders', () => {
    const html = renderReleaseNotes('a \u00000\u0000 b `c` d');
    assert.doesNotMatch(html, /undefined/);
    assert.match(html, /<p>a 0 b <code>c<\/code> d<\/p>/);
});

test('a disabled check is worded as such, before any stale result', () => {
    assert.strictEqual(
        updateStatusText({ InstalledVersion: '4.1.0', CheckEnabled: false, LatestVersion: null, UpdateAvailable: false }),
        'Checking for updates is turned off in the server configuration.');
    assert.strictEqual(updateStatusText({ InstalledVersion: '4.1.0', CheckEnabled: true, LatestVersion: null }),
                       'Not checked yet.');
});
