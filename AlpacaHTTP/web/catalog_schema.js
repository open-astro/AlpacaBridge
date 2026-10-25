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

// Pure readers of GET /management/v1/devicecatalog for the web UI (decision
// 0004), split out of app.js so node tests can run them. The catalog is the
// source of truth for which vendor a device type offers and which form fields
// are auto-numbered enumeration indexes. Nothing here touches the DOM or
// builds markup; catalog text only ever reaches the page through .value,
// .checked or .textContent.

// The descriptor array of a management envelope; throws the server's message
// on a non-zero ErrorNumber.
function catalogFromResponse(payload) {
    if (payload && payload.ErrorNumber !== 0) {
        throw new Error(payload.ErrorMessage || `Error ${payload.ErrorNumber}`);
    }
    if (!payload || !Array.isArray(payload.Value)) {
        throw new Error('the catalog answer has no descriptor list');
    }
    return payload.Value;
}

// load() shares one promise of the catalog. A failed load is dropped so the
// next call fetches again; a loaded catalog is kept for the page's life.
// fetchFn() resolves to the management envelope.
function catalogLoader(fetchFn) {
    let pending = null;
    return function load() {
        if (pending === null) {
            const attempt = Promise.resolve().then(fetchFn).then(catalogFromResponse);
            pending = attempt;
            attempt.catch(() => {
                if (pending === attempt) {
                    pending = null;
                }
            });
        }
        return pending;
    };
}

// The vendors with an available descriptor for the device type, plus
// keepVendor (the vendor of the device being edited) when it has a descriptor
// for the type even though this build lacks it.
function offeredVendors(catalog, deviceType, keepVendor = '') {
    if (!Array.isArray(catalog)) {
        return [];
    }
    const type = String(deviceType).toLowerCase();
    const offered = new Set();
    for (const descriptor of catalog) {
        if (descriptor.deviceType === type && (descriptor.available || descriptor.vendor === keepVendor)) {
            offered.add(descriptor.vendor);
        }
    }
    return [...offered];
}

// What the #vendor select shows: the offered vendors, the selected vendor
// only while it is offered, and disabled until a catalog is loaded.
function vendorSelectState(catalog, deviceType, vendor, keepVendor = '') {
    const offered = offeredVendors(catalog, deviceType, keepVendor);
    return {
        offered,
        value: offered.includes(vendor) ? vendor : '',
        disabled: !Array.isArray(catalog),
    };
}

// The device types, out of deviceTypes, that the #device-type select offers:
// those with at least one available descriptor (the test offeredVendors
// applies), plus keepType (the type of the device being edited). Nothing is
// offered until a non-empty catalog is loaded.
function offeredDeviceTypes(catalog, deviceTypes, keepType = '') {
    const keep = String(keepType).toLowerCase();
    return deviceTypes.filter((type) => type === keep && Array.isArray(catalog) && catalog.length > 0
        || offeredVendors(catalog, type).length > 0);
}

// The auto-numbered index fields: each number input mapped to an
// EnumerationIndex catalog field, paired with the input that pins a specific
// unit (the descriptor's first DeviceId field, else its first PortPath field,
// with the same appliesWhen; the one sharing the longest id path wins).
// formFields entries: { id, vendor, deviceType, configKey, control, manual, readOnly, path }.
function deriveIndexFields(catalog, formFields) {
    const sameWhen = (a, b) => (a ? JSON.stringify(a) : '') === (b ? JSON.stringify(b) : '');
    const sharedPath = (a, b) => {
        let n = 0;
        while (n < a.length && n < b.length && a[n] === b[n]) {
            n += 1;
        }
        return n;
    };
    const elementsOf = (descriptor, key) => formFields.filter((f) =>
        f.id && f.vendor === descriptor.vendor && f.deviceType === descriptor.deviceType && f.configKey === key);
    const entries = [];
    for (const descriptor of catalog) {
        for (const field of descriptor.fields) {
            if (field.role !== 'EnumerationIndex') {
                continue;
            }
            const idKey = descriptor.fields.find((f) => f.role === 'DeviceId' && sameWhen(f.appliesWhen, field.appliesWhen))
                || descriptor.fields.find((f) => f.role === 'PortPath' && sameWhen(f.appliesWhen, field.appliesWhen));
            for (const element of elementsOf(descriptor, field.key).filter((e) => e.control === 'number')) {
                const entry = {
                    fieldId: element.id,
                    vendor: descriptor.vendor,
                    deviceType: descriptor.deviceType,
                    configKey: field.key,
                };
                if (idKey) {
                    const best = elementsOf(descriptor, idKey.key)
                        .sort((a, b) => sharedPath(element.path, b.path) - sharedPath(element.path, a.path))[0];
                    if (best) {
                        entry.idFieldId = best.id;
                    }
                }
                entries.push(entry);
            }
        }
    }
    return entries;
}

// The form elements mapped to the descriptor's catalog fields, in the
// descriptor's field order (document order within one field).
function elementsByField(descriptor, formFields) {
    const entries = [];
    for (const field of descriptor.fields) {
        for (const element of formFields) {
            if (element.id && element.vendor === descriptor.vendor && element.deviceType === descriptor.deviceType
                && element.configKey === field.key) {
                entries.push({ field, element });
            }
        }
    }
    return entries;
}

// What the edit form fills from a saved config: one { id, key, value,
// discriminator } per mapped element, in descriptor field order, so the
// Discriminators come out in the order their togglers must run. A saved value
// is assigned as saved (false and 0 included; a stringList joins with "\n").
// An absent or null value is null, meaning "show the markup default", except
// for a Discriminator, which gets its catalog default (the value the server
// reads for an absent key). Record lists, secrets, read-only elements (their
// value is fixed by the server) and elements marked data-config-fill="manual"
// are not assigned. [] without a descriptor.
function editAssignments(descriptor, config, formFields) {
    if (!descriptor) {
        return [];
    }
    const saved = config || {};
    const assignments = [];
    for (const { field, element } of elementsByField(descriptor, formFields)) {
        if (element.manual || element.readOnly || field.type === 'recordList' || field.role === 'Secret') {
            continue;
        }
        const discriminator = field.role === 'Discriminator';
        let value = saved[field.key];
        if (value === undefined || value === null) {
            value = discriminator && field.default !== undefined ? field.default : null;
        } else if (field.type === 'stringList') {
            value = Array.isArray(value) ? value.join('\n') : null;
        } else if (element.control === 'checkbox') {
            value = value === true;
        }
        assignments.push({ id: element.id, key: field.key, value, discriminator });
    }
    return assignments;
}

// Whether each mapped element's field applies, given the values the form holds
// (valuesById: element id to value). A field applies when it has no
// appliesWhen, or when the discriminator element nearest to it (longest shared
// id path) holds appliesWhen.value and itself applies. A discriminator with no
// value in valuesById counts as holding its catalog default; one with no
// element on the form cannot be judged, so its dependants apply.
// { elementId: boolean }; {} without a descriptor.
function applicability(descriptor, formFields, valuesById) {
    if (!descriptor) {
        return {};
    }
    const entries = elementsByField(descriptor, formFields);
    const sharedPath = (a, b) => {
        let n = 0;
        while (n < a.length && n < b.length && a[n] === b[n]) {
            n += 1;
        }
        return n;
    };
    const result = {};
    const applies = (entry, seen) => {
        if (entry.element.id in result) {
            return result[entry.element.id];
        }
        const when = entry.field.appliesWhen;
        let answer = true;
        if (when && !seen.has(entry.element.id)) {
            const nearest = entries.filter((e) => e.field.key === when.key)
                .sort((a, b) => sharedPath(entry.element.path, b.element.path)
                    - sharedPath(entry.element.path, a.element.path))[0];
            if (nearest) {
                const held = nearest.element.id in valuesById ? valuesById[nearest.element.id] : nearest.field.default;
                answer = String(held) === String(when.value) && applies(nearest, new Set(seen).add(entry.element.id));
            }
        }
        result[entry.element.id] = answer;
        return answer;
    };
    for (const entry of entries) {
        applies(entry, new Set());
    }
    return result;
}

// Browsers ignore this; `node --test` uses it. Guarded rather than a real
// module so index.html can keep loading the file with a plain <script> tag.
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { catalogFromResponse, catalogLoader, offeredVendors, vendorSelectState, offeredDeviceTypes, deriveIndexFields,
        editAssignments, applicability };
}
