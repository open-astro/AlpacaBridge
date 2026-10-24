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

// The text the live server clock shows when GET /management/v1/synctime answers
// with an Alpaca error, or '' when the reply is not one (issue #677). The #670
// refusal of a host clock outside 2000..2100 UTC carries the fix in its
// ErrorMessage, so the clock says it rather than keeping a stale time.
function serverClockError(result) {
    if (!result || typeof result.ErrorNumber !== 'number' || result.ErrorNumber === 0) {
        return '';
    }
    const message = typeof result.ErrorMessage === 'string' ? result.ErrorMessage.trim() : '';
    return message || 'Server clock error ' + result.ErrorNumber;
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

// Software update card (docs/software-update.md): the one-line summary of a
// /management/v1/update/status payload. Pure so node can pin every shape,
// including the one that matters most: a dev build NEWER than the repository
// must read as up to date, not as an available "update" backwards.
function updateStatusText(status) {
    const source = status || {};
    const installed = String(source.InstalledVersion || '');
    const latest = source.LatestVersion ? String(source.LatestVersion) : '';
    if (source.CheckEnabled === false) {
        return 'Checking for updates is turned off in the server configuration.';
    }
    if (source.CheckError) {
        return 'Check failed: ' + source.CheckError;
    }
    if (!latest) {
        return 'Not checked yet.';
    }
    if (source.UpdateAvailable) {
        return 'Version ' + latest + ' is available.';
    }
    if (installed && installed !== latest) {
        return 'Up to date. This build (' + installed + ') is newer than the repository release ' + latest + '.';
    }
    return 'Up to date.';
}

// The installer half of the same payload. '' when there is nothing to say.
function installerStateText(installer) {
    const source = installer || {};
    const detail = source.Detail ? String(source.Detail) : '';
    switch (source.State) {
        case 'running':
            return 'Installing the update. The server restarts when it finishes.';
        case 'succeeded':
            return 'The last update finished successfully.';
        case 'failed':
            return 'The last update failed.' + (detail ? ' (' + detail + ')' : '');
        case 'unavailable':
            return 'Installing from this page is not available on this host' + (detail ? ': ' + detail : '.');
        default:
            return '';
    }
}

// Channel selector of the Software Update card (docs/software-update.md).
// `status` is the /management/v1/update/status payload. The selector shows the
// host's effective channel and is disabled, with the reason, while
// update_packages_url pins the check (ChannelOverridden) or a switch/install
// is running.
function channelSelectorState(status) {
    const source = status || {};
    const value = source.Channel === 'beta' ? 'beta' : 'stable';
    if (source.ChannelOverridden) {
        return { value, disabled: true, note: 'set by update_packages_url' };
    }
    const switching = (source.ChannelSwitch || {}).State === 'running';
    const installing = (source.Installer || {}).State === 'running';
    if (switching) return { value, disabled: true, note: 'Switching the channel...' };
    if (installing) return { value, disabled: true, note: 'An update is installing.' };
    if ((source.ChannelSwitch || {}).State === 'unavailable') {
        return { value, disabled: true, note: 'Not available on this host.' };
    }
    return { value, disabled: false, note: channelDowngradeNotice(value, source.InstalledVersion) };
}

// What choosing Stable does to a host on a beta build: apt never downgrades,
// so the build stays until the stable release overtakes it. '' when the
// installed version is not a pre-release, or the choice is not Stable.
function channelDowngradeNotice(choice, installedVersion) {
    const installed = String(installedVersion || '');
    if (choice !== 'stable' || installed.indexOf('~') < 0) return '';
    return 'You stay on ' + installed + ' until the stable release overtakes it. ' +
           'The update card then offers stable releases only.';
}

// The channel helper half of the payload; '' when there is nothing to say.
// A finished switch is reported only when this page started it: the helper's
// transcript outlives the run, and a reload weeks later must not repeat it.
function channelSwitchText(channelSwitch, startedHere) {
    const source = channelSwitch || {};
    if (source.State !== 'running' && !startedHere) return '';
    switch (source.State) {
        case 'running':
            return 'Switching the update channel...';
        case 'succeeded':
            return 'The update channel was switched. Check for updates to see what it offers.';
        case 'failed':
            return 'Switching the update channel failed.' + (source.Detail ? ' (' + source.Detail + ')' : '');
        default:
            return '';
    }
}

// Release notes for the Software Update card (docs/software-update.md).
//
// docs/releases/X.Y.Z.md is written in a small Markdown subset: "#"/"##"
// headings, "-" bullets, paragraphs, **bold**, `code`, [links](https://...)
// and fenced code blocks. This renders exactly that subset and nothing else.
// Everything is HTML-escaped BEFORE any tag is produced, tags come only
// from the parsed structure, and a link is emitted only for an http(s)
// target, so notes fetched from the network can never inject markup. The
// leading "# AlpacaBridge X.Y.Z" title is dropped: the card names the
// version itself.
function escapeNotesHtml(text) {
    return String(text)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function renderNotesInline(escaped) {
    // Operates on already-escaped text; the patterns contain no characters
    // that escaping changes. Code spans are lifted out first so that bold
    // and link syntax inside backticks stays literal, as Markdown means it.
    const spans = [];
    let out = escaped.replace(/`([^`]+)`/g, (match, code) => {
        spans.push('<code>' + code + '</code>');
        return '\u0000' + (spans.length - 1) + '\u0000';
    });
    out = out.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
    out = out.replace(/\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)/g,
        '<a href="$2" target="_blank" rel="noopener">$1</a>');
    return out.replace(/\u0000(\d+)\u0000/g, (match, index) => spans[Number(index)]);
}

function renderReleaseNotes(markdown) {
    // NUL is the code-span placeholder delimiter below and has no place in
    // notes; strip it so notes carrying it cannot confuse the placeholders.
    const lines = String(markdown || '').replace(/\u0000/g, '').replace(/\r\n?/g, '\n').split('\n');
    const html = [];
    let paragraph = [];
    let inList = false;
    let inFence = false;
    let fence = [];
    let titleSeen = false;

    const flushParagraph = () => {
        if (paragraph.length) {
            html.push('<p>' + renderNotesInline(escapeNotesHtml(paragraph.join(' '))) + '</p>');
            paragraph = [];
        }
    };
    const closeList = () => {
        if (inList) {
            html.push('</ul>');
            inList = false;
        }
    };

    for (const raw of lines) {
        const line = raw.replace(/\s+$/, '');
        if (inFence) {
            if (/^```/.test(line)) {
                html.push('<pre><code>' + escapeNotesHtml(fence.join('\n')) + '</code></pre>');
                fence = [];
                inFence = false;
            } else {
                fence.push(line);
            }
            continue;
        }
        if (/^```/.test(line)) {
            flushParagraph();
            closeList();
            inFence = true;
            continue;
        }
        const heading = /^(#{1,6})\s+(.*)$/.exec(line);
        if (heading) {
            flushParagraph();
            closeList();
            if (heading[1].length === 1 && !titleSeen) {
                titleSeen = true;  // the "# AlpacaBridge X.Y.Z" title
                continue;
            }
            const level = heading[1].length === 1 ? 4 : 5;
            html.push('<h' + level + '>' + renderNotesInline(escapeNotesHtml(heading[2])) + '</h' + level + '>');
            continue;
        }
        const bullet = /^\s*[-*]\s+(.*)$/.exec(line);
        if (bullet) {
            flushParagraph();
            if (!inList) {
                html.push('<ul>');
                inList = true;
            }
            html.push('<li>' + renderNotesInline(escapeNotesHtml(bullet[1])) + '</li>');
            continue;
        }
        if (line.trim() === '') {
            flushParagraph();
            closeList();
            continue;
        }
        if (inList) {
            // A wrapped bullet continues the previous item.
            html[html.length - 1] = html[html.length - 1].replace(/<\/li>$/,
                ' ' + renderNotesInline(escapeNotesHtml(line.trim())) + '</li>');
            continue;
        }
        paragraph.push(line.trim());
    }
    if (inFence) {
        while (fence.length && fence[fence.length - 1] === '') fence.pop();
        html.push('<pre><code>' + escapeNotesHtml(fence.join('\n')) + '</code></pre>');
    }
    flushParagraph();
    closeList();
    return html.join('\n');
}

// open-astro#392: the Host check rows of the server settings area.
// The names default.yaml says are always allowed (the text after "Always
// allowed: ", no final dot); a test pins this to that file.
const HOST_CHECK_ALWAYS_ALLOWED =
    "IP addresses, localhost and *.localhost, this machine's hostname, *.local, *.home.arpa and *.internal";

// What the two Host check rows render from the description Value. null when
// the server does not report the setting (an older build), so no row shows a
// toggle that reads as "off". Both rows are always editable (open-astro#787).
function hostCheckSettings(desc) {
    if (!desc || typeof desc.HostCheckEnabled !== 'boolean') {
        return null;
    }
    return {
        enabled: desc.HostCheckEnabled,
        hosts: typeof desc.AllowedHosts === 'string' ? desc.AllowedHosts : '',
    };
}

// #1509: the saved discovery port the description reports, or null when the
// server does not report one (an older build) so the row is not shown.
function discoveryPortSetting(desc) {
    const port = desc && desc.DiscoveryPort;
    return Number.isInteger(port) && port >= 1 && port <= 65535 ? port : null;
}

// The message to show for a settings PUT, or '' when it saved. The server
// refuses a lockout with HTTP 400 and the reason in ErrorMessage, so the body
// is read before the status decides anything.
function settingsSaveError(status, data) {
    const ok = status >= 200 && status < 300;
    const isEnvelope = data !== null && typeof data === 'object' && typeof data.ErrorNumber === 'number';
    if (ok && isEnvelope && data.ErrorNumber === 0) {
        return '';
    }
    const message = isEnvelope && typeof data.ErrorMessage === 'string' ? data.ErrorMessage.trim() : '';
    if (message) {
        return message;
    }
    if (!ok) {
        return `HTTP error! status: ${status}`;
    }
    return isEnvelope ? `Server error ${data.ErrorNumber}` : 'Unknown server error';
}

function wifiSsidKey(item) {
    return item && typeof item.SsidHex === 'string' ? item.SsidHex.toLowerCase() : String((item && item.Ssid) || '');
}

function wifiSsidLabel(item, displayCount) {
    const label = String((item && item.Ssid) || '');
    const hex = item && typeof item.SsidHex === 'string' ? item.SsidHex : '';
    return displayCount > 1 && hex ? `${label} (${hex})` : label;
}

// The server accepts each ASIAIR GPIO line on at most one port (router.cpp).
// Returns a message naming the first repeated line, or null. `gpios` holds one
// number per port row in order; NaN (a blank row) is skipped.
function asiairDuplicateGpioError(gpios) {
    const firstRow = new Map();
    for (let i = 0; i < gpios.length; i += 1) {
        const gpio = gpios[i];
        if (Number.isNaN(gpio)) continue;
        if (firstRow.has(gpio)) {
            return `GPIO ${gpio} is selected for ports ${firstRow.get(gpio) + 1} and ${i + 1}. ` +
                'Each GPIO line can be used on only one port.';
        }
        firstRow.set(gpio, i);
    }
    return null;
}

// The alignmentMode values the SynScan and Celestron drivers accept (#860);
// anything else reads as 'auto', as the server's sanitize drops it.
function normalizeAlignmentMode(value) {
    return value === 'altaz' || value === 'equatorial' ? value : 'auto';
}

// Status dot for a configureddevices row: green = driver loaded and a client
// Connected, yellow = loaded and idle (no client connected; the hardware is not
// probed until a connect), red = LoadError, LastConnectError, a LinkFault
// (connected or not), or Connected missing/non-boolean (unavailable).
// Green means a client is connected AND the device still replies, so any latched
// LinkFault turns the device red. Each red reason carries its own label, and the
// red text joins every reason.
function deviceStatus(device) {
    const d = device || {};
    const reasons = [];
    if (d.LoadError === true) reasons.push('Driver failed to load, see the server log');
    if (d.LastConnectError) reasons.push('Last connect failed: ' + String(d.LastConnectError));
    if (d.LinkFault) reasons.push('Device not responding: ' + String(d.LinkFault));
    if (reasons.length > 0) return { state: 'error', text: reasons.join('; ') };
    if (d.Connected === true) return { state: 'connected', text: 'Client connected' };
    if (d.Connected === false) return { state: 'idle', text: 'Idle, no client connected' };
    return { state: 'error', text: 'Status unavailable' };
}

// Site coordinates in the Configured Settings list: finite numbers print at a
// fixed 6 decimals (about 0.11 m); display only, the stored value keeps full
// precision. Any other type returns null so the caller keeps its own path.
// A value that rounds to zero from below prints "0.000000", never "-0.000000".
function formatSiteCoordinate(value) {
    if (typeof value !== 'number' || !Number.isFinite(value)) return null;
    const text = value.toFixed(6);
    return text === '-0.000000' ? '0.000000' : text;
}

// Camera picker for a camera-attached thermal switch (ZWO dew heater, ToupTek and
// Player One heater/fan). A bare "Camera Index" box let a dew heater bind to a
// camera with no heater (an ASI290MM Mini at index 0 beside an ASI2600MM Pro at
// index 1), and a rig can carry several heater cameras (2600MC, 6200, 2400), so
// the form always asks: nothing is preselected for a new switch and the submit
// refuses until a camera is picked. `cameras` is the Value of GET
// /management/v1/thermalcameras ({Index, Name, Serial, HasThermal} each), or
// null when the listing failed. `saved` is the edited device's {cameraIndex,
// serialNumber}, or null for a new one. `taken` lists the {cameraIndex,
// serialNumber} of this vendor's other thermal switches. Option values: ''
// (nothing picked yet), 'cam:<index>' (an attached camera, sent with its serial
// when it has one), 'saved' (the saved binding of a camera that is not attached
// now, sent back unchanged). The server still accepts cameraIndex -1 (Auto) for
// switches saved that way; the form never offers it.
const THERMAL_AUTO_INDEX = -1;
const THERMAL_UNLISTED_INDEXES = 4;

function thermalCameraTaken(camera, taken) {
    return (taken || []).some(t => (t.serialNumber ? t.serialNumber === camera.Serial
                                                   : Number(t.cameraIndex) === camera.Index));
}

function thermalCameraLabel(camera, controls, taken) {
    return String(camera.Name) + ' (index ' + camera.Index + '), ' +
        (camera.HasThermal ? 'has a ' : 'no ') + controls +
        (camera.HasThermal && thermalCameraTaken(camera, taken) ? ', used by another switch' : '');
}

function thermalSavedIndex(saved) {
    return !saved || saved.cameraIndex === undefined || saved.cameraIndex === null
        ? null : Number(saved.cameraIndex);
}

// The value an edited switch's saved binding selects: an attached camera that
// has the controls, the not-attached 'saved' entry, or '' when it must be picked
// again (saved as Auto, or bound to a camera without the controls).
function thermalSavedValue(cameras, saved) {
    if (!saved) return '';
    const serial = saved.serialNumber ? String(saved.serialNumber) : '';
    const index = thermalSavedIndex(saved);
    if (!serial && (index === null || index === THERMAL_AUTO_INDEX)) return '';
    if (Array.isArray(cameras)) {
        const match = cameras.find(c => (serial ? c.Serial === serial : c.Index === index));
        if (!match) return 'saved';
        return match.HasThermal ? 'cam:' + match.Index : '';
    }
    if (!serial && index >= 0 && index < THERMAL_UNLISTED_INDEXES) return 'cam:' + index;
    return 'saved';
}

function thermalCameraOptions(cameras, saved, controls, current, taken) {
    const options = [{ value: '', label: 'Choose the camera this ' + controls + ' is on', disabled: true }];
    if (Array.isArray(cameras)) {
        // Cameras with the controls first; the rest are shown but cannot be picked.
        const ordered = cameras.filter(c => c.HasThermal).concat(cameras.filter(c => !c.HasThermal));
        ordered.forEach(c => options.push({
            value: 'cam:' + c.Index, label: thermalCameraLabel(c, controls, taken), disabled: !c.HasThermal,
        }));
    } else {
        for (let i = 0; i < THERMAL_UNLISTED_INDEXES; i += 1) {
            options.push({ value: 'cam:' + i, label: 'Camera index ' + i + ' (not checked)', disabled: false });
        }
    }
    const savedValue = thermalSavedValue(cameras, saved);
    if (savedValue === 'saved') {
        const label = saved.serialNumber
            ? 'Saved camera, serial ' + saved.serialNumber + ' (not attached)'
            : 'Saved camera, index ' + saved.cameraIndex + ' (not attached)';
        options.push({ value: 'saved', label, disabled: false });
    }
    const pickable = value => options.some(o => o.value === value && !o.disabled);
    const selected = current && pickable(current) ? current : savedValue;
    return { options, selected };
}

// What the submit sends, or null when nothing is picked (the submit refuses).
function thermalCameraSelection(value, cameras, saved) {
    if (value === 'saved' && saved) {
        const result = { cameraIndex: Number(saved.cameraIndex ?? THERMAL_AUTO_INDEX) };
        if (saved.serialNumber) result.serialNumber = String(saved.serialNumber);
        return result;
    }
    if (typeof value === 'string' && value.startsWith('cam:')) {
        const index = Number.parseInt(value.slice(4), 10);
        const result = { cameraIndex: index };
        const camera = Array.isArray(cameras) ? cameras.find(c => c.Index === index) : null;
        if (camera && camera.Serial) result.serialNumber = String(camera.Serial);
        return result;
    }
    return null;
}

function thermalCameraHint(cameras, value, controls, listError, saved, taken) {
    if (!Array.isArray(cameras)) {
        return 'Could not list the attached cameras' + (listError ? ': ' + listError : '') +
            '. Pick the index of the camera this ' + controls + ' is on.';
    }
    if (value === 'saved') {
        return 'The saved camera is not attached. Saving keeps it.';
    }
    const thermal = cameras.filter(c => c.HasThermal);
    if (value === '') {
        if (thermal.length === 0) {
            return 'No attached camera reports a ' + controls + '. Plug the camera in, then press Refresh.';
        }
        const index = thermalSavedIndex(saved);
        if (saved && !saved.serialNumber && index === null && Number(saved.cameraId) >= 0) {
            return 'This switch was saved with camera ID ' + saved.cameraId + ' by an older version. ' +
                'Choose the camera this ' + controls + ' is on.';
        }
        if (saved && !saved.serialNumber && (index === null || index === THERMAL_AUTO_INDEX)) {
            return 'This switch was saved as Auto. Choose the camera this ' + controls + ' is on.';
        }
        const bound = saved ? cameras.find(c => (saved.serialNumber ? c.Serial === String(saved.serialNumber)
                                                                    : c.Index === index)) : null;
        if (bound && !bound.HasThermal) {
            return 'The saved camera, ' + bound.Name + ' (index ' + bound.Index + '), has no ' + controls +
                '. Choose the camera this ' + controls + ' is on.';
        }
        return 'Choose the camera this ' + controls + ' is on.';
    }
    const camera = cameras.find(c => 'cam:' + c.Index === value);
    if (camera && thermalCameraTaken(camera, taken)) {
        return 'Another switch already uses ' + camera.Name + '.';
    }
    return '';
}

// Saved-config keys the device card's settings list leaves out: the card shows
// them in its own rows (type, number, vendor, and the Unique ID row for the
// stored ID in `uniqueId`, decision 0018) or derives them (aperture area
// and focal length).
const HIDDEN_DEVICE_SETTING_KEYS = new Set(['vendor', 'deviceType', 'deviceNumber', 'uniqueId',
    'apertureDiameter', 'focalLength']);

function deviceSettingShown(key) {
    return !HIDDEN_DEVICE_SETTING_KEYS.has(key);
}

// Celestron focus motor calibration (issue #1411). The driver's
// CalibrationStatus action answers exactly "Calibrating", "Calibrated" or
// "NotCalibrated"; anything else (an error, a disconnected device, an unknown
// string) parses to 'unknown'. Abort is offered only for 'calibrating': the
// motor firmware can lose a good calibration to an abort sent while idle.
const CALIBRATION_STATUS_STATES = {
    Calibrating: 'calibrating',
    Calibrated: 'calibrated',
    NotCalibrated: 'not-calibrated',
};

function showsCalibrationControls(device) {
    if (!device) return false;
    const vendor = String(device.Vendor || (device.Config && device.Config.vendor) || '').toLowerCase();
    return vendor === 'celestron' && String(device.DeviceType || '').toLowerCase() === 'focuser';
}

function parseCalibrationStatus(value) {
    if (typeof value !== 'string') return 'unknown';
    return Object.prototype.hasOwnProperty.call(CALIBRATION_STATUS_STATES, value)
        ? CALIBRATION_STATUS_STATES[value] : 'unknown';
}

function isCalibrationRunning(state) {
    return state === 'calibrating';
}

function mayAbortCalibration(state) {
    return state === 'calibrating';
}

// A fresh CalibrationStatus answer, read just before AbortCalibration is sent,
// must still say "Calibrating"; a cached state is not enough.
function mayAbortFromStatus(value) {
    return mayAbortCalibration(parseCalibrationStatus(value));
}

// Whether to schedule another status read. Failed reads during a run keep
// the poll going (Abort stays hidden meanwhile); a failure after any other
// state ends it.
function keepPollingCalibration(lastGoodState, nextState, failed) {
    if (failed) return lastGoodState === 'calibrating';
    return isCalibrationRunning(nextState);
}

function calibrationStatusClass(state) {
    return state === 'not-calibrated' ? 'calibration-status calibration-status-warn' : 'calibration-status';
}

function mayStartCalibration(state) {
    return state === 'calibrated' || state === 'not-calibrated';
}

function calibrationStatusText(state, errorText) {
    if (state === 'calibrating') return 'Calibrating: the motor is driving between its end stops (about 5 minutes).';
    if (state === 'calibrated') return 'Calibrated.';
    if (state === 'not-calibrated') return 'Not calibrated.';
    return errorText ? 'Calibration status unavailable: ' + errorText : 'Calibration status unknown.';
}

function calibrationHint(state) {
    if (state === 'not-calibrated') {
        return 'The motor is not calibrated, so Position, MaxStep and moves are refused. Press Calibrate.';
    }
    return '';
}

function calibrationConfirmText() {
    return 'Calibrate the focus motor? It drives from one end of its travel to the other and back, ' +
        'which takes about 5 minutes. Remove anything that could be hit by the focuser or its load first.';
}

// Body for PUT /api/v1/focuser/{n}/action (Alpaca form fields).
function calibrationActionBody(actionName, clientTransactionId) {
    return new URLSearchParams({
        Action: actionName,
        Parameters: '',
        ClientID: '1',
        ClientTransactionID: String(clientTransactionId),
    }).toString();
}

// Browsers ignore this; `node --test` uses it. Guarded rather than a real
// module so index.html can keep loading the file with a plain <script> tag.
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { isValidClockSeconds, serverClockError, localZoneLabel, formatServerClock, buildBadgeLabel,
                       updateStatusText, installerStateText, renderReleaseNotes,
                       channelSelectorState, channelDowngradeNotice, channelSwitchText,
                        HOST_CHECK_ALWAYS_ALLOWED, hostCheckSettings, discoveryPortSetting, settingsSaveError,
                        wifiSsidKey, wifiSsidLabel, asiairDuplicateGpioError,
                        normalizeAlignmentMode, deviceStatus, formatSiteCoordinate,
                        thermalCameraOptions, thermalCameraSelection, thermalCameraHint,
                        deviceSettingShown,
                       showsCalibrationControls, parseCalibrationStatus, isCalibrationRunning,
                       mayAbortCalibration, mayAbortFromStatus, keepPollingCalibration, calibrationStatusClass,
                       mayStartCalibration, calibrationStatusText, calibrationHint,
                       calibrationConfirmText, calibrationActionBody };
}
