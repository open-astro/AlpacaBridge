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
// This renders the same instant in the viewer's zone, in the log lines' own
// "YYYY-MM-DD HH:MM:SS" shape, and keeps a zone label so the reading stays
// unambiguous. The host's zone would be the better label for a remote
// operator, but it is not in the description payload yet; adding it touches
// add_clock_fields(), which two open PRs are already editing.
//
// Falls back to the previous UTC rendering if Intl cannot produce the parts:
// a wrong-looking clock is worse than an unfashionable one.
function localZoneLabel(date) {
    try {
        const part = new Intl.DateTimeFormat(undefined, { timeZoneName: 'short' })
            .formatToParts(date)
            .find((p) => p.type === 'timeZoneName');
        return part ? part.value : '';
    } catch (e) {
        return '';
    }
}

function formatServerClock(date) {
    try {
        const parts = new Intl.DateTimeFormat('en-CA', {
            year: 'numeric',
            month: '2-digit',
            day: '2-digit',
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
            hourCycle: 'h23',
            timeZoneName: 'short'
        }).formatToParts(date).reduce((acc, part) => {
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
        const zone = localZoneLabel(date) || parts.timeZoneName || '';
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

// Browsers ignore this; `node --test` uses it. Guarded rather than a real
// module so index.html can keep loading the file with a plain <script> tag.
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { localZoneLabel, formatServerClock };
}
