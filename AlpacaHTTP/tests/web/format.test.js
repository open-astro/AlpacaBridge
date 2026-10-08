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

// Unit tests for the web UI's pure formatting helpers (open-astro#385).
//
// Run: node --test AlpacaHTTP/tests/web/*.test.js
// (the file form, not the directory: `node --test <dir>` resolves the path
// as a module on Node 22 and dies with MODULE_NOT_FOUND. Both gates pass an
// explicit git ls-files list for the same reason.)
// `node --test` ships with the Node versions CI already installs; there is no
// dependency to add and no package.json.
//
// These cases are the by-hand verification table from open-astro#359 made
// executable. That table covered TZ=Pacific/Auckland, TZ=UTC, local midnight,
// and Intl.DateTimeFormat monkey-patched to throw and to drop hourCycle. None
// of it ran in CI, so the next edit to formatServerClock() had nothing
// catching a regression -- including the 12-hour case, which renders a
// plausible-looking WRONG time rather than an obvious failure.

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

const FORMAT_PATH = path.join(__dirname, '..', '..', 'web', 'format.js');

// Each case re-requires the module so a patched Intl cannot leak between them.
function withFormat(fn) {
    delete require.cache[require.resolve(FORMAT_PATH)];
    // eslint-disable-next-line global-require
    return fn(require(FORMAT_PATH));
}

// The instant every case uses: 2026-09-11 11:30:48 UTC. Chosen so the hour is
// unambiguous in both halves of the day (23:30 local in Auckland, so a 12-hour
// engine renders "11:30 PM" and the guard for that is exercised) and so the
// offset is a whole 12 hours. It does NOT cross a date boundary: NZST is
// UTC+12 in September (NZDT starts 2026-09-27), so 11:30 UTC is 23:30 the SAME
// day locally. The date-shift path is covered by the local-midnight case
// below, which is the only one that rolls the date.
const INSTANT = new Date(Date.UTC(2026, 8, 11, 11, 30, 48));

const SHAPE = /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \(.+\)$/;

function withTZ(tz, fn) {
    const previous = process.env.TZ;
    process.env.TZ = tz;
    try {
        return fn();
    } finally {
        if (previous === undefined) {
            delete process.env.TZ;
        } else {
            process.env.TZ = previous;
        }
    }
}

test('renders the log lines\' own YYYY-MM-DD HH:MM:SS shape with a zone label', () => {
    withTZ('UTC', () => withFormat(({ formatServerClock }) => {
        const rendered = formatServerClock(INSTANT);
        assert.match(rendered, SHAPE);
        assert.ok(rendered.startsWith('2026-09-11 11:30:48 '), rendered);
    }));
});

test('renders the instant in the viewer\'s zone, not UTC', () => {
    // The whole point of #354: a viewer twelve hours from Greenwich should not
    // have to do the arithmetic to match a header reading against a log line.
    // NZST is UTC+12, so 11:30 UTC renders as 23:30 on the SAME date -- the
    // date roll is the separate midnight case below.
    withTZ('Pacific/Auckland', () => withFormat(({ formatServerClock }) => {
        const rendered = formatServerClock(INSTANT);
        assert.match(rendered, SHAPE);
        assert.ok(rendered.startsWith('2026-09-11 23:30:48 '), rendered);
        assert.ok(!rendered.includes('(UTC)'), rendered);
    }));
});

// open-astro#354, second half: the description payload's TimeZone field
// (the host's IANA zone) takes precedence over the viewer's zone, so a
// remote operator reads the rig's wall clock, which is what its logs say.
test('renders in the server zone when one is given, whatever the viewer zone', () => {
    withTZ('UTC', () => withFormat(({ formatServerClock }) => {
        const rendered = formatServerClock(INSTANT, 'Pacific/Auckland');
        assert.match(rendered, SHAPE);
        assert.ok(rendered.startsWith('2026-09-11 23:30:48 '), rendered);
        assert.ok(!rendered.includes('(UTC)'), rendered);
    }));
    withTZ('Pacific/Auckland', () => withFormat(({ formatServerClock }) => {
        const rendered = formatServerClock(INSTANT, 'Etc/UTC');
        assert.ok(rendered.startsWith('2026-09-11 11:30:48 '), rendered);
    }));
});

test('an empty or missing server zone keeps the viewer-zone rendering', () => {
    withTZ('Pacific/Auckland', () => withFormat(({ formatServerClock }) => {
        assert.strictEqual(formatServerClock(INSTANT, ''), formatServerClock(INSTANT));
        assert.strictEqual(formatServerClock(INSTANT, undefined), formatServerClock(INSTANT));
        assert.ok(formatServerClock(INSTANT, '').startsWith('2026-09-11 23:30:48 '));
    }));
});

test('a server zone this Intl does not know falls back to the viewer zone, not UTC', () => {
    // A stale browser tz database (or a host zone name that is not IANA
    // after all) must degrade to the pre-#354 behaviour, not to the UTC tier
    // that exists for a broken engine.
    withTZ('Pacific/Auckland', () => withFormat(({ formatServerClock }) => {
        const rendered = formatServerClock(INSTANT, 'Mars/Olympus_Mons');
        assert.match(rendered, SHAPE);
        assert.ok(rendered.startsWith('2026-09-11 23:30:48 '), rendered);
        assert.ok(!rendered.includes('(UTC)'), rendered);
    }));
});

test('the zone label follows the server zone', () => {
    withTZ('UTC', () => withFormat(({ localZoneLabel }) => {
        const label = localZoneLabel(INSTANT, 'Pacific/Auckland');
        // The viewer's locale decides between "NZST" and "GMT+12"; either
        // way it must not be the viewer's own UTC.
        assert.ok(label && label !== 'UTC', label);
    }));
});

test('local midnight does not roll the date backwards', () => {
    // 2026-09-11 12:00 UTC is 2026-09-12 00:00 in Auckland. A padding or
    // rounding slip here shows up as a date that disagrees with the time.
    withTZ('Pacific/Auckland', () => withFormat(({ formatServerClock }) => {
        const midnight = new Date(Date.UTC(2026, 8, 11, 12, 0, 0));
        assert.ok(formatServerClock(midnight).startsWith('2026-09-12 00:00:00 '),
                  formatServerClock(midnight));
    }));
});

test('single-digit fields are zero padded', () => {
    // Some ICU versions hand back a single digit for hours 0-9 once hourCycle
    // forces a non-default cycle, which makes the header jitter by a character
    // on the hour. The ICU this runs against is NOT one of them -- it already
    // returns "03" for the same instant, so asking it directly would pass with
    // the pad deleted. Stub the unpadded shape to exercise the pad itself.
    withTZ('UTC', () => withFormat(({ formatServerClock }) => {
        const real = Intl.DateTimeFormat;
        Intl.DateTimeFormat = function () {
            return {
                formatToParts: () => [
                    { type: 'year', value: '2026' }, { type: 'month', value: '1' },
                    { type: 'day', value: '2' }, { type: 'hour', value: '3' },
                    { type: 'minute', value: '4' }, { type: 'second', value: '5' },
                    { type: 'timeZoneName', value: 'UTC' },
                ],
            };
        };
        try {
            const early = new Date(Date.UTC(2026, 0, 2, 3, 4, 5));
            assert.ok(formatServerClock(early).startsWith('2026-01-02 03:04:05 '),
                      formatServerClock(early));
        } finally {
            Intl.DateTimeFormat = real;
        }
    }));
});

test('falls back to a labelled UTC rendering when Intl throws', () => {
    withTZ('Pacific/Auckland', () => withFormat(({ formatServerClock }) => {
        const real = Intl.DateTimeFormat;
        Intl.DateTimeFormat = function () { throw new Error('no Intl here'); };
        try {
            assert.strictEqual(formatServerClock(INSTANT), '2026-09-11 11:30:48 (UTC)');
        } finally {
            Intl.DateTimeFormat = real;
        }
    }));
});

test('falls back when Intl returns incomplete parts', () => {
    withTZ('UTC', () => withFormat(({ formatServerClock }) => {
        const real = Intl.DateTimeFormat;
        Intl.DateTimeFormat = function () {
            // A timeZoneName is included deliberately: without it the
            // "no zone label" guard takes the fallback too, and this case
            // would pass with the incomplete-parts check deleted.
            return {
                formatToParts: () => [
                    { type: 'year', value: '2026' },
                    { type: 'timeZoneName', value: 'UTC' },
                ],
            };
        };
        try {
            assert.strictEqual(formatServerClock(INSTANT), '2026-09-11 11:30:48 (UTC)');
        } finally {
            Intl.DateTimeFormat = real;
        }
    }));
});

test('falls back on an engine that ignores hourCycle and answers in 12-hour form', () => {
    // The case that matters most: without this guard the reduce drops the
    // AM/PM part and 23:30 renders as 11:30 with nothing to say which half of
    // the day it is -- a plausible-looking wrong time, not an obvious failure.
    //
    // The stub deliberately answers 11:30 PM for an instant that really is
    // 23:30, so the guarded fallback ("23:30:48") and the unguarded 12-hour
    // path ("11:30:48") produce DIFFERENT strings. An earlier draft used an
    // 11:30 instant, where both paths happen to agree and deleting the guard
    // left this case green.
    const lateInstant = new Date(Date.UTC(2026, 8, 11, 23, 30, 48));
    withTZ('UTC', () => withFormat(({ formatServerClock }) => {
        const real = Intl.DateTimeFormat;
        Intl.DateTimeFormat = function () {
            return {
                formatToParts: () => [
                    { type: 'year', value: '2026' },
                    { type: 'month', value: '09' },
                    { type: 'day', value: '11' },
                    { type: 'hour', value: '11' },
                    { type: 'minute', value: '30' },
                    { type: 'second', value: '48' },
                    { type: 'dayPeriod', value: 'PM' },
                    { type: 'timeZoneName', value: 'UTC' }
                ]
            };
        };
        try {
            assert.strictEqual(formatServerClock(lateInstant), '2026-09-11 23:30:48 (UTC)');
        } finally {
            Intl.DateTimeFormat = real;
        }
    }));
});

test('falls back when the engine produces no zone label at all', () => {
    // An unlabelled local time is the exact ambiguity this function exists to
    // remove, so "no label" takes the fallback rather than rendering bare.
    withTZ('UTC', () => withFormat(({ formatServerClock }) => {
        const real = Intl.DateTimeFormat;
        Intl.DateTimeFormat = function () {
            return {
                formatToParts: () => [
                    { type: 'year', value: '2026' },
                    { type: 'month', value: '09' },
                    { type: 'day', value: '11' },
                    { type: 'hour', value: '11' },
                    { type: 'minute', value: '30' },
                    { type: 'second', value: '48' }
                ]
            };
        };
        try {
            assert.strictEqual(formatServerClock(INSTANT), '2026-09-11 11:30:48 (UTC)');
        } finally {
            Intl.DateTimeFormat = real;
        }
    }));
});

test('localZoneLabel returns a label, and an empty string when Intl throws', () => {
    withTZ('Pacific/Auckland', () => withFormat(({ localZoneLabel }) => {
        assert.notStrictEqual(localZoneLabel(INSTANT), '');
        const real = Intl.DateTimeFormat;
        Intl.DateTimeFormat = function () { throw new Error('nope'); };
        try {
            assert.strictEqual(localZoneLabel(INSTANT), '');
        } finally {
            Intl.DateTimeFormat = real;
        }
    }));
});

test('formatServerClock does not throw on an Invalid Date (issue #511)', () => {
    // A finite but absurd clock Value (1e15 s) passes Number.isFinite() and
    // yields an Invalid Date; the catch arm's toISOString() used to throw.
    withTZ('UTC', () => withFormat(({ formatServerClock }) => {
        assert.doesNotThrow(() => formatServerClock(new Date(1e18)));
        assert.equal(formatServerClock(new Date(1e18)), '--:--:--');
        assert.equal(formatServerClock(new Date(NaN), 'UTC'), '--:--:--');
    }));
});

test('isValidClockSeconds rejects non-finite and out-of-range Values (issue #511)', () => {
    withFormat(({ isValidClockSeconds }) => {
        assert.equal(isValidClockSeconds(1.7e9), true);
        assert.equal(isValidClockSeconds(1e15), false);
        assert.equal(isValidClockSeconds(-1e15), false);
        assert.equal(isValidClockSeconds(NaN), false);
        assert.equal(isValidClockSeconds(Infinity), false);
        assert.equal(isValidClockSeconds('1700000000'), false);
        assert.equal(isValidClockSeconds(null), false);
    });
});

test('serverClockError surfaces a synctime ErrorMessage and nothing on success (issue #677)', () => {
    withFormat(({ serverClockError }) => {
        // The #670 refusal, verbatim, is what the clock must show.
        const message = 'Host clock is outside 2000-01-01..2100-01-01 UTC; set the time with POST ' +
            '/management/v1/synctime.';
        assert.equal(serverClockError({ ErrorNumber: 1035, ErrorMessage: message }), message);
        // A success reply, and anything that is not an Alpaca error reply, is no error.
        assert.equal(serverClockError({ ErrorNumber: 0, ErrorMessage: '', Value: 1.7e9 }), '');
        assert.equal(serverClockError(null), '');
        assert.equal(serverClockError(undefined), '');
        assert.equal(serverClockError({ Value: 1.7e9 }), '');
        assert.equal(serverClockError({ ErrorNumber: '1035', ErrorMessage: message }), '');
        // An error with no usable text still says so, with its number.
        assert.equal(serverClockError({ ErrorNumber: 1035, ErrorMessage: '   ' }), 'Server clock error 1035');
        assert.equal(serverClockError({ ErrorNumber: 1035 }), 'Server clock error 1035');
        assert.equal(serverClockError({ ErrorNumber: 1035, ErrorMessage: 42 }), 'Server clock error 1035');
    });
});

test('deviceStatus maps Connected, LoadError, LastConnectError and unknown', () => {
    const { deviceStatus } = require('../../web/format.js');
    assert.strictEqual(deviceStatus({ Connected: true }).state, 'connected');
    assert.strictEqual(deviceStatus({ Connected: false }).state, 'idle');
    assert.strictEqual(deviceStatus({}).state, 'error');
    assert.strictEqual(deviceStatus({ Connected: true, LastConnectError: 'port busy' }).text, 'Error: port busy');
    const both = deviceStatus({ LoadError: true, LastConnectError: 'x' });
    assert.strictEqual(both.state, 'error');
    assert.match(both.text, /failed to load.*x/);
    const faulted = deviceStatus({ Connected: true, LinkFault: 'no status frame for 12 s' });
    assert.strictEqual(faulted.state, 'error');
    assert.match(faulted.text, /Link fault: no status frame/);
    assert.strictEqual(deviceStatus({ Connected: false, LastConnectError: '' }).state, 'idle');
});
