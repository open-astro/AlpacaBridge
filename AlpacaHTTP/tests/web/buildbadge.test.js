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

// Unit tests for the header build badge's decision rule (buildBadgeLabel).
//
// Run: node --test AlpacaHTTP/tests/web/buildbadge.test.js
// (the file form, not the directory -- see format.test.js for why.)
//
// The badge exists so a dev build cannot be mistaken for an official
// release, and the rule got that backwards once already: it hid the badge
// whenever the branch name read "HEAD", which `git rev-parse --abbrev-ref
// HEAD` prints for EVERY detached checkout, not just the one packaging does.
// A PR head checked out by sha then showed no badge at all. test_routing.cpp
// pins the endpoint and its keys; this pins what the browser does with them.
// The detached-non-release case below fails if that condition comes back.

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

const { buildBadgeLabel } = require(path.join(__dirname, '..', '..', 'web', 'format.js'));

const REMOTE = 'https://github.com/open-astro/AlpacaBridge';

test('an exact-tag release build hides the badge', () => {
    // What packaging produces: the tag checked out as a detached HEAD, so
    // GitBranch is "HEAD" AND GitIsRelease is true.
    assert.strictEqual(buildBadgeLabel({
        GitBranch: 'HEAD', GitCommit: 'abc1234', GitDirty: false,
        GitIsRelease: true, GitRemoteUrl: REMOTE
    }), null);
});

test('a detached non-release build shows the badge, labelled by commit', () => {
    // `git fetch origin pull/447/head && git checkout FETCH_HEAD`, a bisect,
    // or any actions/checkout build. GitBranch is "HEAD" but this is NOT a
    // release, and hiding the badge here is the one outcome the feature
    // cannot have.
    const view = buildBadgeLabel({
        GitBranch: 'HEAD', GitCommit: 'abc1234', GitDirty: false,
        GitIsRelease: false, GitRemoteUrl: REMOTE
    });
    assert.notStrictEqual(view, null);
    assert.strictEqual(view.label, 'detached@abc1234');
    // "HEAD" names nothing to a reader; the commit is the identity.
    assert.ok(!view.label.includes('HEAD'));
    assert.ok(view.title.includes('detached HEAD'));
    assert.strictEqual(view.href, REMOTE + '/commit/abc1234');
});

test('a branch build shows branch@commit and links to the commit', () => {
    const view = buildBadgeLabel({
        GitBranch: 'feature/build-branch-badge', GitCommit: 'deadbee', GitDirty: false,
        GitIsRelease: false, GitRemoteUrl: REMOTE
    });
    assert.strictEqual(view.label, 'feature/build-branch-badge@deadbee');
    // The commit, never the branch: a PR head fetched under a local name has
    // no matching ref on the remote.
    assert.strictEqual(view.href, REMOTE + '/commit/deadbee');
    assert.ok(view.title.includes('branch feature/build-branch-badge'));
});

test('a dirty tree gets a trailing asterisk and says so', () => {
    const view = buildBadgeLabel({
        GitBranch: 'main', GitCommit: 'deadbee', GitDirty: true,
        GitIsRelease: false, GitRemoteUrl: REMOTE
    });
    assert.strictEqual(view.label, 'main@deadbee*');
    assert.ok(view.title.includes('uncommitted changes'));
});

test('a build with no git metadata hides the badge', () => {
    // The version.h defaults: a source tarball built outside a git checkout
    // knows nothing, so it claims nothing.
    assert.strictEqual(buildBadgeLabel({
        GitBranch: 'unknown', GitCommit: 'unknown', GitDirty: false,
        GitIsRelease: false, GitRemoteUrl: ''
    }), null);
    assert.strictEqual(buildBadgeLabel({}), null);
    assert.strictEqual(buildBadgeLabel(null), null);
});

test('no remote or no commit means no link, and no promise of one', () => {
    const noRemote = buildBadgeLabel({
        GitBranch: 'wip', GitCommit: 'deadbee', GitDirty: false,
        GitIsRelease: false, GitRemoteUrl: ''
    });
    assert.strictEqual(noRemote.label, 'wip@deadbee');
    assert.strictEqual(noRemote.href, '');
    // The title must not offer a click the badge cannot honour.
    assert.ok(!noRemote.title.includes('click'));

    const noCommit = buildBadgeLabel({
        GitBranch: 'wip', GitCommit: 'unknown', GitDirty: false,
        GitIsRelease: false, GitRemoteUrl: REMOTE
    });
    assert.strictEqual(noCommit.label, 'wip');
    assert.strictEqual(noCommit.href, '');
    assert.ok(!noCommit.title.includes('click'));
});

test('lowercase key spellings are accepted', () => {
    // parseResponseValue() hands back whatever the server sent, and the UI
    // has always tolerated both spellings. The release check especially must
    // not fail open on a lowercase payload.
    assert.strictEqual(buildBadgeLabel({
        gitBranch: 'HEAD', gitCommit: 'abc1234', gitIsRelease: true, gitRemoteUrl: REMOTE
    }), null);
    assert.strictEqual(buildBadgeLabel({
        gitBranch: 'topic', gitCommit: 'abc1234', gitDirty: true, gitIsRelease: false, gitRemoteUrl: REMOTE
    }).label, 'topic@abc1234*');
});

test('a commit with URL-significant characters is escaped into the href', () => {
    // GitCommit is a short sha in practice; this pins the encode so a
    // describe-style value, or anything odder, cannot break out of the path.
    assert.strictEqual(buildBadgeLabel({
        GitBranch: 'topic', GitCommit: 'a b#c', GitDirty: false,
        GitIsRelease: false, GitRemoteUrl: REMOTE
    }).href, REMOTE + '/commit/a%20b%23c');
});
