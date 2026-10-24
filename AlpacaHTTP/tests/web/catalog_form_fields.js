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

// Shared helper for the catalog web tests (not a test file itself). It reads
// the device form out of index.html the way app.js reads it from the DOM: an
// element's vendor and device type come from its own data-vendor /
// data-device-type attribute, else from the nearest enclosing <div> that has
// one; its catalog field is its own data-config-key. An element marked
// data-config-fill="manual" is filled by hand-written code in app.js.

'use strict';

const fs = require('node:fs');
const path = require('node:path');

const WEB = path.join(__dirname, '..', '..', 'web');
const CATALOG_FIXTURE = path.join(__dirname, '..', 'fixtures', 'devicecatalog.json');
const ROUTER = path.join(__dirname, '..', '..', 'src', 'http', 'router.cpp');

function readWeb(file) {
    return fs.readFileSync(path.join(WEB, file), 'utf8');
}

// The committed catalog fixture: the exact Value array the server's
// GET /management/v1/devicecatalog answers in the all-vendors build
// (test_routing.cpp compares the two byte for byte).
function readCatalogFixture() {
    return JSON.parse(fs.readFileSync(CATALOG_FIXTURE, 'utf8'));
}

// The keys sanitize_device_config() keeps for every device whatever its
// descriptor declares (vendor, deviceType, deviceNumber, the site and timing
// keys): the copy_if_present("...") calls in router.cpp.
function routerAgnosticKeys(routerSource = fs.readFileSync(ROUTER, 'utf8')) {
    return [...routerSource.matchAll(/copy_if_present\("([A-Za-z0-9]+)"\)/g)].map((m) => m[1]);
}

function attr(attrs, name) {
    const m = attrs.match(new RegExp(`(?:^|\\s)${name}="([^"]*)"`));
    return m ? m[1] : null;
}

function hasAttr(attrs, name) {
    return new RegExp(`(?:^|\\s)${name}(?=[\\s=/]|$)`).test(attrs);
}

function deviceFormHtml(html) {
    const start = html.indexOf('<form id="device-form"');
    const end = html.indexOf('</form>', start);
    if (start < 0 || end < 0) {
        throw new Error('index.html has no <form id="device-form">');
    }
    return html.slice(start, end).replace(/<!--[\s\S]*?-->/g, '');
}

// What the element shows before any script sets it: an input's value
// attribute, a checkbox's checked attribute, a select's selected option (else
// its first), a textarea's text.
function markupDefault(tag, control, attrs, rest) {
    if (tag === 'select') {
        const body = rest.slice(0, rest.indexOf('</select>'));
        const options = [...body.matchAll(/<option\b([^>]*)>/g)].map((m) => m[1]);
        const chosen = options.find((o) => hasAttr(o, 'selected')) || options[0];
        return chosen === undefined ? '' : (attr(chosen, 'value') ?? '');
    }
    if (tag === 'textarea') {
        return rest.slice(0, rest.indexOf('</textarea>'));
    }
    if (control === 'checkbox' || control === 'radio') {
        return hasAttr(attrs, 'checked');
    }
    return attr(attrs, 'value') ?? '';
}

// Every <input>, <select> and <textarea> inside <form id="device-form">, in
// document order: { id, name, tag, control, vendor, deviceType, configKey,
// manual, path, markupDefault }. path lists the ids of the enclosing <div>s,
// outermost first.
function formFields(html) {
    const form = deviceFormHtml(html);
    const re = /<(\/?)(div|input|select|textarea)\b([^>]*)>/g;
    const stack = [];
    const fields = [];
    let m;
    while ((m = re.exec(form)) !== null) {
        const [, close, tag, attrs] = m;
        if (tag === 'div') {
            if (close) {
                stack.pop();
            } else {
                stack.push({
                    id: attr(attrs, 'id'),
                    vendor: attr(attrs, 'data-vendor'),
                    deviceType: attr(attrs, 'data-device-type'),
                });
            }
            continue;
        }
        if (close) {
            continue;
        }
        const nearest = (own, key) => {
            if (own !== null) return own;
            for (let i = stack.length - 1; i >= 0; i -= 1) {
                if (stack[i][key] !== null) return stack[i][key];
            }
            return null;
        };
        const control = tag === 'input' ? (attr(attrs, 'type') || 'text') : tag;
        fields.push({
            id: attr(attrs, 'id'),
            name: attr(attrs, 'name'),
            tag,
            control,
            vendor: nearest(attr(attrs, 'data-vendor'), 'vendor'),
            deviceType: nearest(attr(attrs, 'data-device-type'), 'deviceType'),
            configKey: attr(attrs, 'data-config-key'),
            manual: attr(attrs, 'data-config-fill') === 'manual',
            path: stack.map((s) => s.id).filter(Boolean),
            markupDefault: markupDefault(tag, control, attrs, form.slice(re.lastIndex)),
        });
    }
    return fields;
}

// The option values of <select id="vendor">, without the empty placeholder.
function vendorOptions(html) {
    const start = html.indexOf('<select id="vendor"');
    const body = html.slice(start, html.indexOf('</select>', start));
    return [...body.matchAll(/<option value="([^"]*)"/g)].map((m) => m[1]).filter(Boolean);
}

module.exports = {
    WEB,
    readWeb,
    readCatalogFixture,
    routerAgnosticKeys,
    formFields,
    vendorOptions,
};
