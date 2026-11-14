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

// Issue #1764: what the in-page dialog resolves to. Every destructive action
// (delete a device or the log files, shut down, restart, forget a WiFi
// network, install an update) waits on confirmDialog(), so Cancel and Esc must
// resolve "no". Runs the real dialog block of app.js (from the "In-page
// dialog" banner to "Tab management") against a stub DOM whose <dialog>
// behaves like the browser's: close(value) sets returnValue and fires
// "close", and a submit from the method="dialog" form closes with the
// submitter's value ("confirm").
//
// Run: node --test AlpacaHTTP/tests/web/ui_dialog.test.js

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const APP_JS = path.join(__dirname, '..', '..', 'web', 'app.js');

function makeDom() {
    const byId = {};
    const doc = { activeElement: null };

    function makeEl(tag, id) {
        const listeners = {};
        const attrs = {};
        const el = {
            tagName: tag.toUpperCase(),
            id: id || '',
            hidden: false,
            textContent: '',
            className: '',
            value: '',
            type: '',
            dataset: {},
            children: [],
            parentNode: null,
            isConnected: true,
            addEventListener(name, fn, opts) {
                (listeners[name] = listeners[name] || []).push({ fn, once: !!(opts && opts.once) });
            },
            removeEventListener(name, fn) {
                listeners[name] = (listeners[name] || []).filter((l) => l.fn !== fn);
            },
            dispatch(name) {
                for (const l of [...(listeners[name] || [])]) {
                    if (l.once) el.removeEventListener(name, l.fn);
                    l.fn({ type: name, target: el });
                }
            },
            listenerCount(name) { return (listeners[name] || []).length; },
            focus() { doc.activeElement = el; },
            setAttribute(name, value) {
                attrs[name] = String(value);
                if (name === 'id') el.id = String(value);
            },
            getAttribute(name) { return Object.prototype.hasOwnProperty.call(attrs, name) ? attrs[name] : null; },
            removeAttribute(name) { delete attrs[name]; },
            appendChild(child) {
                child.parentNode = el;
                el.children.push(child);
                if (child.id) byId[child.id] = child;
                return child;
            },
            append(...kids) { kids.forEach((k) => el.appendChild(k)); },
            remove() {
                if (el.parentNode) {
                    el.parentNode.children = el.parentNode.children.filter((c) => c !== el);
                    el.parentNode = null;
                }
                if (byId[el.id] === el) delete byId[el.id];
                el.isConnected = false;
            },
            insertAdjacentElement(where, other) {
                assert.strictEqual(where, 'afterend');
                el.parentNode.appendChild(other);
                return other;
            },
        };
        if (id) byId[id] = el;
        return el;
    }

    const dialog = makeEl('dialog', 'ui-dialog');
    dialog.open = false;
    dialog.returnValue = '';
    dialog.showModal = () => { dialog.open = true; };
    dialog.close = (value) => {
        if (!dialog.open) return;
        dialog.open = false;
        if (value !== undefined) dialog.returnValue = value;
        dialog.dispatch('close');
    };
    for (const id of ['ui-dialog-title', 'ui-dialog-body', 'ui-dialog-field', 'ui-dialog-input-label',
                      'ui-dialog-input', 'ui-dialog-confirm', 'ui-dialog-cancel', 'toast-region']) {
        makeEl(id === 'ui-dialog-input' ? 'input' : 'div', id);
    }
    byId['ui-dialog-confirm'].click = () => dialog.close('confirm');  // the method="dialog" submit
    byId['ui-dialog-cancel'].click = () => byId['ui-dialog-cancel'].dispatch('click');

    doc.getElementById = (id) => byId[id] || null;
    doc.createElement = (tag) => makeEl(tag);
    return { doc, byId, dialog, makeEl };
}

function load() {
    const src = fs.readFileSync(APP_JS, 'utf8');
    const start = src.indexOf('// ---- In-page dialog');
    const end = src.indexOf('// Tab management');
    assert.ok(start >= 0 && end > start, 'app.js no longer has the dialog block between its banners');
    const dom = makeDom();
    const timers = [];
    const ctx = {
        document: dom.doc,
        setTimeout: (fn, ms) => { timers.push({ fn, ms }); return timers.length; },
        console,
    };
    vm.createContext(ctx);
    vm.runInContext(src.slice(start, end) + '\n;globalThis.__ui = { confirmDialog, noticeDialog, promptDialog,'
                    + ' showToast, showFieldError, clearFieldError, clearFieldErrors, TOAST_MS };', ctx);
    return { ui: ctx.__ui, timers, ...dom };
}

// The dialog opens after the queue's microtask; wait for it.
const flush = () => new Promise((r) => setImmediate(r));

async function opened(t) {
    await flush();
    assert.ok(t.dialog.open, 'the dialog did not open');
}

test('confirmDialog: Cancel resolves false and returns focus to the trigger (#1764)', async () => {
    const t = load();
    const trigger = t.makeEl('button', 'delete-btn');
    trigger.focus();
    const answer = t.ui.confirmDialog({ title: 'Delete device?', body: 'Gone for good.', confirmLabel: 'Delete', danger: true });
    await opened(t);
    assert.strictEqual(t.byId['ui-dialog-title'].textContent, 'Delete device?');
    assert.strictEqual(t.byId['ui-dialog-confirm'].textContent, 'Delete');
    t.byId['ui-dialog-cancel'].click();
    assert.strictEqual(await answer, false);
    assert.strictEqual(t.doc.activeElement, trigger);
    assert.strictEqual(t.dialog.listenerCount('close'), 0, 'the close listener leaked');
    assert.strictEqual(t.byId['ui-dialog-cancel'].listenerCount('click'), 0, 'the Cancel listener leaked');
});

test('confirmDialog: Esc (a close with no value) resolves false (#1764)', async () => {
    const t = load();
    const answer = t.ui.confirmDialog({ title: 'Shut down the server?', danger: true });
    await opened(t);
    t.dialog.close();  // Esc: the browser closes without setting returnValue
    assert.strictEqual(await answer, false);
});

test('confirmDialog: the confirm button resolves true (#1764)', async () => {
    const t = load();
    const answer = t.ui.confirmDialog({ title: 'Restart the server?' });
    await opened(t);
    t.byId['ui-dialog-confirm'].click();
    assert.strictEqual(await answer, true);
});

test('a danger dialog opens on Cancel, a plain one on its confirm button (#1764)', async () => {
    const t = load();
    let answer = t.ui.confirmDialog({ title: 'Delete all log files?', danger: true });
    await opened(t);
    assert.strictEqual(t.doc.activeElement, t.byId['ui-dialog-cancel']);
    assert.strictEqual(t.byId['ui-dialog-confirm'].className, 'btn btn-danger');
    t.byId['ui-dialog-cancel'].click();
    await answer;
    answer = t.ui.confirmDialog({ title: 'Sync the clock?' });
    await opened(t);
    assert.strictEqual(t.doc.activeElement, t.byId['ui-dialog-confirm']);
    assert.strictEqual(t.byId['ui-dialog-confirm'].className, 'btn btn-primary');
    t.byId['ui-dialog-confirm'].click();
    assert.strictEqual(await answer, true);
});

test('promptDialog returns the typed text, or null on Cancel or Esc (#1764)', async () => {
    const t = load();
    let answer = t.ui.promptDialog({ title: 'Join "Home"', input: { label: 'Password', type: 'password' } });
    await opened(t);
    const input = t.byId['ui-dialog-input'];
    assert.strictEqual(t.doc.activeElement, input);
    assert.strictEqual(input.type, 'password');
    assert.strictEqual(t.byId['ui-dialog-field'].hidden, false);
    input.value = 'hunter22';
    t.byId['ui-dialog-confirm'].click();
    assert.strictEqual(await answer, 'hunter22');
    assert.strictEqual(input.value, '', 'the typed password stays in the field');

    answer = t.ui.promptDialog({ title: 'Join "Home"', input: { label: 'Password' } });
    await opened(t);
    input.value = 'typed then cancelled';
    t.byId['ui-dialog-cancel'].click();
    assert.strictEqual(await answer, null);

    answer = t.ui.promptDialog({ title: 'Join "Home"', input: { label: 'Password' } });
    await opened(t);
    t.dialog.close();
    assert.strictEqual(await answer, null);
});

test('noticeDialog has no Cancel, and a page without showModal never confirms (#1764)', async () => {
    const t = load();
    const done = t.ui.noticeDialog('Server clock error', 'The clock is off.');
    await opened(t);
    assert.strictEqual(t.byId['ui-dialog-cancel'].hidden, true);
    t.byId['ui-dialog-confirm'].click();
    assert.strictEqual(await done, undefined);

    delete t.dialog.showModal;
    assert.strictEqual(await t.ui.confirmDialog({ title: 'Forget "Home"?' }), false);
});

test('a second dialog waits for the first to close (#1764)', async () => {
    const t = load();
    const first = t.ui.confirmDialog({ title: 'First' });
    const second = t.ui.confirmDialog({ title: 'Second' });
    await opened(t);
    assert.strictEqual(t.byId['ui-dialog-title'].textContent, 'First');
    t.byId['ui-dialog-cancel'].click();
    assert.strictEqual(await first, false);
    await opened(t);
    assert.strictEqual(t.byId['ui-dialog-title'].textContent, 'Second');
    t.byId['ui-dialog-confirm'].click();
    assert.strictEqual(await second, true);
});
