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

// Pure formatting helpers for the web UI, split out of app.js so they can be
// tested (open-astro#385).
//
// `node --check` was the only automated gate on the hand-written web UI
// JavaScript, and a syntax check catches a missing brace and nothing else.
// That was defensible while app.js was DOM wiring; these two functions have a
// real contract with three separate fallback guards, one of which (an engine
// that ignores hourCycle and answers in 12-hour form) renders a
// plausible-looking WRONG time rather than an obvious failure. They were
// verified by hand once, under several TZ settings and with
// Intl.DateTimeFormat monkey-patched; that table is now
// AlpacaHTTP/tests/web/format.test.js and runs in CI.
//
// Nothing here touches the DOM or any app.js global, which is the whole point:
// the file loads before app.js in index.html and is `require`-able from Node
// with no browser stub. Keep it that way -- a helper that reaches for
// `document` belongs in app.js.

// open-astro#354: the header clock rendered toISOString(), which is UTC by
// definition, while the server's own log lines are written through
// localtime_r() (logging_adapter.cpp, logging.cpp). The two disagreed about
// which zone they were in with nothing on screen saying so, and correlating a
// header reading against a log line meant knowing the host's offset and doing
// the arithmetic by eye.
//
// This renders the same instant in the SERVER's zone when the description
// payload names one (its TimeZone field, an IANA name read from the host,
// which is the zone the log lines are in), else in the viewer's own zone,
// and keeps a zone label either way so the reading stays unambiguous. The
// two tiers differ for a remote operator: a rig in a paddock twelve hours
// away should read as the rig's wall clock, since that is what its logs say.
//
// `timeZone` is optional. An empty or unrecognised value (Intl throws a
// RangeError for a name its tz database lacks) takes the viewer-zone tier,
// which was the whole behaviour before the field existed; and if Intl cannot
// produce the parts at all, the previous UTC rendering: a wrong-looking
// clock is worse than an unfashionable one.
function localZoneLabel(date, timeZone) {
    try {
        const options = { timeZoneName: 'short' };
        if (timeZone) {
            options.timeZone = timeZone;
        }
        const part = new Intl.DateTimeFormat(undefined, options)
            .formatToParts(date)
            .find((p) => p.type === 'timeZoneName');
        return part ? part.value : '';
    } catch (e) {
        return '';
    }
}

// True when a synctime Value (whole seconds) can become a valid Date. Number.isFinite
// alone passes 1e15, which is finite but far outside the +-8.64e15 ms Date range
// (issue #511); such a Value is the same error case as a non-numeric one.
function isValidClockSeconds(value) {
    return Number.isFinite(value) && !Number.isNaN(new Date(value * 1000).getTime());
}

function formatServerClock(date, timeZone) {
    if (Number.isNaN(date.getTime())) {
        // No time to show; the UTC arm below would throw on toISOString().
        return '--:--:--';
    }
    if (timeZone) {
        // Prove the zone before using it: the viewer-zone tier is the right
        // fallback for a name this browser's Intl does not know, and it must
        // not be collapsed into the UTC tier below, which exists for a
        // broken engine, not a stale tz database.
        try {
            new Intl.DateTimeFormat('en-CA', { timeZone: timeZone });
        } catch (e) {
            return formatServerClock(date, '');
        }
    }
    try {
        const options = {
            year: 'numeric',
            month: '2-digit',
            day: '2-digit',
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
            hourCycle: 'h23',
            timeZoneName: 'short'
        };
        if (timeZone) {
            options.timeZone = timeZone;
        }
        const parts = new Intl.DateTimeFormat('en-CA', options).formatToParts(date).reduce((acc, part) => {
            acc[part.type] = part.value;
            return acc;
        }, {});
        if (!parts.year || !parts.month || !parts.day || !parts.hour || !parts.minute || !parts.second) {
            throw new Error('incomplete date parts');
        }
        // hourCycle predates neither Chrome 73 nor Safari 14.1; an older
        // browser ignores it, en-CA falls back to 12-hour, and the reduce
        // above drops the AM/PM part -- 23:30 would render as 11:30 with
        // nothing to say which half of the day it is. A dayPeriod part is
        // exactly that case, so treat it as unusable and take the fallback.
        if (parts.dayPeriod) {
            throw new Error('12-hour clock, hourCycle unsupported');
        }
        // The date parts come from en-CA for its year-month-day ordering, but
        // that locale renders most zones as a GMT offset (it carries only the
        // North American abbreviations). The label is asked for again in the
        // viewer's own locale, which yields the abbreviation an operator
        // recognises (NZST rather than GMT+12) wherever the browser has one,
        // and falls back to the offset where it does not.
        const zone = localZoneLabel(date, timeZone) || parts.timeZoneName || '';
        if (!zone) {
            // An engine that accepted the options but produced no zone part
            // would otherwise render a bare "2026-09-11 23:30:48" -- an
            // unlabelled local time, which is the exact ambiguity this
            // function exists to remove. Take the labelled UTC fallback.
            throw new Error('no zone label');
        }
        const suffix = ` (${zone})`;
        // Pad the hour rather than trusting hour: '2-digit': some ICU
        // versions hand back a single digit for 0-9 once hourCycle forces a
        // non-default cycle, which makes the header jitter by a character on
        // the hour. The other fields are unaffected, but pad them the same
        // way so one rule covers the line.
        const pad = (value) => String(value).padStart(2, '0');
        return `${parts.year}-${pad(parts.month)}-${pad(parts.day)} ` +
               `${pad(parts.hour)}:${pad(parts.minute)}:${pad(parts.second)}${suffix}`;
    } catch (e) {
        // Parenthesised like the normal path, so the two read as one format
        // and the only difference an operator sees is the zone itself.
        return date.toISOString().replace('T', ' ').substring(0, 19) + ' (UTC)';
    }
}

// Decides what the header build badge shows for a /management/v1/buildinfo
// payload: null to hide it, else {label, title, href} (href '' when there is
// nowhere to link). Pure, and here rather than in app.js because this is the
// one rule in the feature that has already been wrong once in review --
// updateHeaderBuildBadge() now does nothing but apply the result, and
// AlpacaHTTP/tests/web/buildbadge.test.js pins it.
//
// isRelease -- HEAD sits exactly on a vX.Y.Z tag -- is the ONLY release
// signal. The first cut also hid the badge when the branch name read "HEAD",
// as a proxy for the detached checkout that packaging does, but
// `git rev-parse --abbrev-ref HEAD` answers "HEAD" for EVERY detached
// checkout: a PR head fetched with `git fetch origin pull/N/head && git
// checkout FETCH_HEAD`, a `git checkout <sha>` to test an older commit, any
// actions/checkout build. Those are dev builds, and hiding the badge made
// them read as official releases -- the exact failure the badge exists to
// prevent. A detached non-release build is labelled by its commit instead,
// since "HEAD" names nothing to a reader.
function buildBadgeLabel(info) {
    const source = info || {};
    const branch = source.GitBranch || source.gitBranch || '';
    const commit = source.GitCommit || source.gitCommit || '';
    const dirty = !!(source.GitDirty !== undefined ? source.GitDirty : source.gitDirty);
    const isRelease = !!(source.GitIsRelease !== undefined ? source.GitIsRelease : source.gitIsRelease);
    const remoteUrl = source.GitRemoteUrl || source.gitRemoteUrl || '';

    if (isRelease || !branch || branch === 'unknown') {
        return null;
    }

    const detached = branch === 'HEAD';
    const haveCommit = !!commit && commit !== 'unknown';
    // Link to the commit, not the branch: a local checkout's branch name
    // (e.g. a PR head fetched under an arbitrary local name) often has no
    // matching ref on the remote, but the commit itself is always valid
    // there since it's the same object fetched from origin. A detached
    // checkout has no branch ref to link to at all.
    const href = (remoteUrl && haveCommit) ? remoteUrl + '/commit/' + encodeURIComponent(commit) : '';
    return {
        label: (detached ? 'detached' : branch) + (haveCommit ? '@' + commit : '') + (dirty ? '*' : ''),
        title: 'Running from a non-release checkout: ' +
            (detached ? 'detached HEAD' : 'branch ' + branch) +
            (haveCommit ? ', commit ' + commit : '') +
            (dirty ? ' (uncommitted changes present)' : '') +
            // Only promise a click when there is somewhere to go.
            (href ? ' -- click to open this commit on GitHub' : ''),
        href: href
    };
}

// Browsers ignore this; `node --test` uses it. Guarded rather than a real
// module so index.html can keep loading the file with a plain <script> tag.
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { isValidClockSeconds, localZoneLabel, formatServerClock, buildBadgeLabel };
}
