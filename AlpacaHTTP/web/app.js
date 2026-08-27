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

// AlpacaHTTP Web UI
const API_BASE = '';
const LOGGING_ENDPOINT = '/management/v1/loglevel';
const LOG_FILES_ENDPOINT = '/management/v1/logfiles';
const QUIET_LOG_LEVEL = 'WARNING';
const LOG_LEVEL_ORDER = ['TRACE', 'DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL'];
const FILTERWHEEL_CUSTOM_VALUE = '__custom__';
const FILTERWHEEL_PRESET_OPTIONS = [
    { value: 'Luminance', label: 'Luminance (L)', aliases: ['L', 'lum', 'luminance'] },
    { value: 'Red', label: 'Red (R)', aliases: ['R', 'red'] },
    { value: 'Green', label: 'Green (G)', aliases: ['G', 'green'] },
    { value: 'Blue', label: 'Blue (B)', aliases: ['B', 'blue'] },
    { value: 'Ha', label: 'H-alpha (H)', aliases: ['H', 'halpha', 'h-alpha'] },
    { value: 'OIII', label: 'OIII (O)', aliases: ['O', 'o3'] },
    { value: 'SII', label: 'SII (S)', aliases: ['S', 's2'] },
    { value: "U'", label: "Sloan (U')", aliases: ["U'", 'uprime', 'sloan-u', 'sloan-uprime'] },
    { value: "G'", label: "Sloan (G')", aliases: ["G'", 'gprime', 'sloan-g', 'sloan-gprime'] },
    { value: "R'", label: "Sloan (R')", aliases: ["R'", 'rprime', 'sloan-r', 'sloan-rprime'] },
    { value: "I'", label: "Sloan (I')", aliases: ["I'", 'iprime', 'sloan-i', 'sloan-iprime'] },
    { value: "Z'", label: "Sloan (Z')", aliases: ["Z'", 'zprime', 'sloan-z', 'sloan-zprime'] },
    { value: 'Clear', label: 'Clear (C)', aliases: ['clear', 'clr'] },
    { value: 'Dark', label: 'Dark (D)', aliases: ['dark'] },
    { value: 'UV', label: 'UV (U)', aliases: ['uv'] },
    { value: 'IR', label: 'IR (I)', aliases: ['ir'] }
];
const FILTERWHEEL_PRESET_LOOKUP = new Map();
const FILTERWHEEL_SHORT_CODES = new Map();
const ALPACA_DEVICE_ORDER = [
    'telescope',
    'camera',
    'filterwheel',
    'focuser',
    'rotator',
    'dome',
    'switch'
];
FILTERWHEEL_PRESET_OPTIONS.forEach(option => {
    FILTERWHEEL_PRESET_LOOKUP.set(normalizeFilterName(option.value), option.value);
    (option.aliases || []).forEach(alias => {
        FILTERWHEEL_PRESET_LOOKUP.set(normalizeFilterName(alias), option.value);
    });
});

[
    { code: 'L', names: ['luminance', 'lum', 'l'] },
    { code: 'R', names: ['red', 'r'] },
    { code: 'G', names: ['green', 'g'] },
    { code: 'B', names: ['blue', 'b'] },
    { code: 'H', names: ['ha', 'halpha', 'h-alpha'] },
    { code: 'O', names: ['oiii', 'o3'] },
    { code: 'S', names: ['sii', 's2'] },
    { code: "U'", names: ["u'", 'uprime', 'sloan-u', 'sloan-uprime'] },
    { code: "G'", names: ["g'", 'gprime', 'sloan-g', 'sloan-gprime'] },
    { code: "R'", names: ["r'", 'rprime', 'sloan-r', 'sloan-rprime'] },
    { code: "I'", names: ["i'", 'iprime', 'sloan-i', 'sloan-iprime'] },
    { code: "Z'", names: ["z'", 'zprime', 'sloan-z', 'sloan-zprime'] },
    { code: 'CLR', names: ['clear', 'clr'] },
    { code: 'DRK', names: ['dark'] },
    { code: 'UV', names: ['uv'] },
    { code: 'IR', names: ['ir'] }
].forEach(entry => {
    entry.names.forEach(name => {
        FILTERWHEEL_SHORT_CODES.set(normalizeFilterName(name), entry.code);
    });
});

function normalizeLogLevel(level) {
    return String(level || '').trim().toUpperCase();
}

function resolveLogLevel(level) {
    const normalized = normalizeLogLevel(level);
    return LOG_LEVEL_ORDER.includes(normalized) ? normalized : QUIET_LOG_LEVEL;
}

function getLogLevelIndex(level) {
    return LOG_LEVEL_ORDER.indexOf(normalizeLogLevel(level));
}

function getLogLevelToggles() {
    return Array.from(document.querySelectorAll('#log-level-toggles input[data-level]'));
}

function setLogControlsDisabled(disabled) {
    getLogLevelToggles().forEach(toggle => {
        toggle.disabled = disabled;
    });
}

function ensureLogLevelToggles(supportedLevels) {
    const container = document.getElementById('log-level-toggles');
    if (!container) {
        return;
    }

    const normalizedLevels = Array.isArray(supportedLevels)
        ? supportedLevels.map(normalizeLogLevel)
        : LOG_LEVEL_ORDER.slice();
    const orderedLevels = LOG_LEVEL_ORDER.filter(level => normalizedLevels.includes(level));

    container.innerHTML = '';
    orderedLevels.forEach(level => {
        const labelText = level === 'WARNING'
            ? 'Warning'
            : level.charAt(0) + level.slice(1).toLowerCase();
        const label = document.createElement('label');
        label.className = 'log-level-toggle';
        label.innerHTML = `
            <input type="checkbox" data-level="${level}">
            <span>${labelText}</span>
        `;
        container.appendChild(label);
    });

    getLogLevelToggles().forEach(toggle => {
        toggle.addEventListener('change', handleLogLevelToggleChange);
    });
}

function applyLogLevelSelection(minLevel) {
    const resolved = resolveLogLevel(minLevel);
    const minIndex = getLogLevelIndex(resolved);
    getLogLevelToggles().forEach(toggle => {
        const idx = getLogLevelIndex(toggle.dataset.level);
        toggle.checked = idx >= minIndex && idx !== -1;
    });
}

function syncLogControls(level, supportedLevels) {
    const resolved = resolveLogLevel(level);
    ensureLogLevelToggles(supportedLevels);
    applyLogLevelSelection(resolved);
    return resolved;
}

async function requestLogLevelUpdate(desiredLevel) {
    const statusEl = document.getElementById('log-level-status');
    if (!statusEl) {
        return;
    }

    const resolvedLevel = resolveLogLevel(desiredLevel);
    setLogControlsDisabled(true);
    statusEl.textContent = 'Updating log settings...';

    try {
        const response = await fetch(API_BASE + LOGGING_ENDPOINT, {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({ level: resolvedLevel })
        });

        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const text = await response.text();
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            throw new Error('Invalid JSON response from server');
        }

        if (data.ErrorNumber !== 0) {
            throw new Error(data.ErrorMessage || 'Unknown server error');
        }

        const payload = parseResponseValue(data.Value) || {};
        const supportedLevels = payload.SupportedLevels || payload.supportedLevels || null;
        const level = syncLogControls(payload.Level || payload.level || resolvedLevel, supportedLevels);
        statusEl.textContent = `Current log level: ${level}`;
    } catch (error) {
        statusEl.textContent = `Failed to update log level: ${error.message}`;
        await loadLogSettings();
    } finally {
        setLogControlsDisabled(false);
    }
}

// Tab management
function showTab(tabName, options) {
    // Hide all tabs
    document.querySelectorAll('.tab-content').forEach(tab => {
        tab.classList.remove('active');
    });
    document.querySelectorAll('.tab').forEach(btn => {
        btn.classList.remove('active');
    });

    // Show selected tab. Mark the matching tab button active by name rather
    // than relying on the global `event`, so showTab works when called
    // programmatically (e.g. after a successful add, or from an edit flow).
    document.getElementById(tabName + '-tab').classList.add('active');
    // Match the full showTab('<name>') call, not just the bare name, so a future
    // tab whose name is a prefix of another (e.g. 'configure' vs
    // 'configure-advanced') can't activate the wrong button via substring match.
    const tabButton = document.querySelector('.tab[onclick*="showTab(\'' + tabName + '\')"]');
    if (tabButton) {
        tabButton.classList.add('active');
    }

    // Opening the Configure tab fresh starts a clean "Add Device" form so a
    // previously edited/half-filled device's settings never leak in. Edit
    // flows populate the form first and pass {preserveForm: true} to skip this.
    if (tabName === 'configure' && !(options && options.preserveForm)) {
        resetDeviceForm();
    }

    // The global action bar's Refresh only makes sense where there is
    // something to refresh - hide it on the Configure form.
    const globalRefreshBtn = document.getElementById('global-refresh');
    if (globalRefreshBtn) {
        globalRefreshBtn.style.display = (tabName === 'configure') ? 'none' : '';
    }
}

// Refresh whatever the active tab shows.
function globalRefresh() {
    const serverTab = document.getElementById('server-tab');
    if (serverTab && serverTab.classList.contains('active')) {
        refreshServerInfo();
    } else {
        refreshDevices();
    }
}

// Restore the configure form to a clean "Add Device" state: native defaults,
// edit mode cleared, vendor sub-sections and filter-wheel slot UIs resynced.
function resetDeviceForm() {
    const form = document.getElementById('device-form');
    if (!form) {
        return;
    }
    form.reset();
    // form.reset() already fires the form's 'reset' listener (which calls
    // setEditMode(false)); call it explicitly too so this helper is
    // self-contained and doesn't silently rely on that listener existing.
    setEditMode(false);
    // Re-run the vendor option/sub-section toggles against the reset values so
    // stale vendor-specific blocks are hidden and slot UIs reflect empty input.
    updateVendorOptions();
    zwoFilterwheelSlotUI.syncSlotsFromTextarea();
    playerOneFilterwheelSlotUI.syncSlotsFromTextarea();
    touptekFilterwheelSlotUI.syncSlotsFromTextarea();
    qhyFilterwheelSlotUI.syncSlotsFromTextarea();
    const messageDiv = document.getElementById('form-message');
    if (messageDiv) {
        messageDiv.style.display = 'none';
    }
}

let currentDevices = [];

function extractDeviceConfig(device) {
    return device.Config || device.config || null;
}

function extractDeviceType(device) {
    const config = extractDeviceConfig(device);
    return normalizeDeviceType(device.DeviceType || device.deviceType || (config && config.deviceType));
}

function extractDeviceNumber(device) {
    const config = extractDeviceConfig(device);
    const value = device.DeviceNumber ?? device.deviceNumber ?? (config && config.deviceNumber);
    const parsed = Number.parseInt(value, 10);
    return Number.isNaN(parsed) ? null : parsed;
}

function extractVendor(device) {
    const config = extractDeviceConfig(device);
    const value = device.Vendor || (config && config.vendor) || '';
    return value.toString().trim().toLowerCase();
}

function getNextAvailableNumber(usedNumbers) {
    let candidate = 0;
    while (usedNumbers.has(candidate)) {
        candidate += 1;
    }
    return candidate;
}

function getNextDeviceNumberForType(deviceType) {
    const normalizedType = normalizeDeviceType(deviceType);
    if (!normalizedType) {
        return null;
    }
    const used = new Set();
    currentDevices.forEach(device => {
        const type = extractDeviceType(device);
        if (type !== normalizedType) {
            return;
        }
        const number = extractDeviceNumber(device);
        if (number !== null && number >= 0) {
            used.add(number);
        }
    });
    return getNextAvailableNumber(used);
}

// Index-addressed config fields that auto-increment per (vendor, deviceType).
//
// The Alpaca device *number* (handled above, vendor-agnostic per type) is how a
// client addresses a device. A vendor *index* is different: it selects which
// physical unit a vendor SDK enumerates on the bus (camera 0, 1, 2 ...). Each
// SDK counts from 0 independently, so the index is scoped per (vendor,
// deviceType) - a ZWO camera and a Player One camera can both be index 0.
// Vendors that connect by serial port or network (iOptron, SynScan, Celestron,
// Bisque) identify the device by path/host and have no index field, so they
// aren't listed here. `idFieldId`, when present, is a serial/ID input that pins
// a specific unit; if the user filled it, we don't impose an auto index.
const INDEX_FIELDS = [
    { fieldId: 'camera-index', vendor: 'zwo', deviceType: 'camera', configKey: 'cameraIndex', idFieldId: 'camera-id' },
    { fieldId: 'filterwheel-index', vendor: 'zwo', deviceType: 'filterwheel', configKey: 'filterwheelIndex', idFieldId: 'filterwheel-id' },
    { fieldId: 'focuser-index', vendor: 'zwo', deviceType: 'focuser', configKey: 'focuserIndex', idFieldId: 'focuser-id' },
    { fieldId: 'rotator-index', vendor: 'zwo', deviceType: 'rotator', configKey: 'rotatorIndex', idFieldId: 'rotator-id' },
    { fieldId: 'qhy-camera-index', vendor: 'qhy', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'qhy-cfw-camera-index', vendor: 'qhy', deviceType: 'filterwheel', configKey: 'cameraIndex', idFieldId: 'qhy-cfw-camera-id' },
    { fieldId: 'svbony-camera-index', vendor: 'svbony', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'touptek-camera-index', vendor: 'touptek', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'touptek-focuser-index', vendor: 'touptek', deviceType: 'focuser', configKey: 'focuserIndex', idFieldId: 'touptek-focuser-id' },
    { fieldId: 'touptek-filterwheel-index', vendor: 'touptek', deviceType: 'filterwheel', configKey: 'filterwheelIndex', idFieldId: 'touptek-filterwheel-id' },
    { fieldId: 'touptek-thermal-camera-index', vendor: 'touptek', deviceType: 'switch', configKey: 'cameraIndex' },
    { fieldId: 'playerone-camera-index', vendor: 'playerone', deviceType: 'camera', configKey: 'cameraIndex' },
    { fieldId: 'playerone-switch-camera-index', vendor: 'playerone', deviceType: 'switch', configKey: 'cameraIndex' },
    { fieldId: 'playerone-filterwheel-index', vendor: 'playerone', deviceType: 'filterwheel', configKey: 'filterwheelIndex' },
    { fieldId: 'astroasis-focuser-index', vendor: 'astroasis', deviceType: 'focuser', configKey: 'focuserIndex', idFieldId: 'astroasis-hid-path' },
    { fieldId: 'gemini-focuser-index', vendor: 'gemini', deviceType: 'focuser', configKey: 'focuserIndex' },
    { fieldId: 'ioptron-ieaf-focuser-index', vendor: 'ioptron', deviceType: 'focuser', configKey: 'focuserIndex' },
    { fieldId: 'ioptron-filterwheel-index', vendor: 'ioptron', deviceType: 'filterwheel', configKey: 'filterwheelIndex' },
    { fieldId: 'gemini-flatpanel-index', vendor: 'gemini', deviceType: 'covercalibrator', configKey: 'panelIndex' },
    // Same configKey as the Lite field above: panelIndex is an index into
    // enumerate_gemini_flatpanel_ports(), a scan shared by both models, so
    // devices of either model correctly compete for the same index namespace.
    { fieldId: 'gemini-flatpanel-v2-index', vendor: 'gemini', deviceType: 'covercalibrator', configKey: 'panelIndex' },
    { fieldId: 'gemini-flatpanel-pro-index', vendor: 'gemini', deviceType: 'covercalibrator', configKey: 'panelIndex' },
    { fieldId: 'wandererastro-cover-index', vendor: 'wandererastro', deviceType: 'covercalibrator', configKey: 'coverIndex' },
    { fieldId: 'wandererastro-rotator-index', vendor: 'wandererastro', deviceType: 'rotator', configKey: 'rotatorIndex' },
    { fieldId: 'wandererastro-filterwheel-index', vendor: 'wandererastro', deviceType: 'filterwheel', configKey: 'wandererFilterwheelIndex' },
    { fieldId: 'wandererastro-box-index', vendor: 'wandererastro', deviceType: 'switch', configKey: 'boxIndex' },
];

// Lowest unused value of field.configKey across already-configured devices that
// match this field's (vendor, deviceType). Each SDK enumerates from 0, so the
// scan is intentionally scoped to the same vendor and type.
function getNextIndexForField(field) {
    const used = new Set();
    currentDevices.forEach(device => {
        if (extractVendor(device) !== field.vendor) {
            return;
        }
        if (extractDeviceType(device) !== field.deviceType) {
            return;
        }
        const config = extractDeviceConfig(device);
        const parsed = Number.parseInt(config && config[field.configKey], 10);
        if (!Number.isNaN(parsed) && parsed >= 0) {
            used.add(parsed);
        }
    });
    return getNextAvailableNumber(used);
}

function maybeAutoFillDeviceNumber() {
    const form = document.getElementById('device-form');
    const deviceNumberInput = document.getElementById('device-number');
    const deviceTypeSelect = document.getElementById('device-type');
    if (!form || !deviceNumberInput || !deviceTypeSelect) {
        return;
    }
    if (form.dataset.editing === 'true') {
        return;
    }
    if (deviceNumberInput.dataset.userModified === 'true') {
        return;
    }
    const nextNumber = getNextDeviceNumberForType(deviceTypeSelect.value);
    if (nextNumber === null) {
        return;
    }
    deviceNumberInput.value = nextNumber;
}

// Auto-fill the vendor index field(s) for the currently selected vendor +
// device type, so adding a second device of the same vendor/type doesn't
// silently reuse index 0. Skips a field the user has edited, or whose
// serial/ID twin is filled in (the ID pins the unit, making the index moot).
function maybeAutoFillIndexFields() {
    const form = document.getElementById('device-form');
    const vendorSelect = document.getElementById('vendor');
    const deviceTypeSelect = document.getElementById('device-type');
    if (!form || !vendorSelect || !deviceTypeSelect) {
        return;
    }
    if (form.dataset.editing === 'true') {
        return;
    }
    const vendor = vendorSelect.value;
    const deviceType = normalizeDeviceType(deviceTypeSelect.value);
    INDEX_FIELDS.forEach(field => {
        if (field.vendor !== vendor || field.deviceType !== deviceType) {
            return;
        }
        const input = document.getElementById(field.fieldId);
        if (!input || input.dataset.userModified === 'true') {
            return;
        }
        if (field.idFieldId) {
            const idInput = document.getElementById(field.idFieldId);
            if (idInput && idInput.value.trim() !== '') {
                return;
            }
        }
        input.value = getNextIndexForField(field);
    });
}

function updateAutoNumbering() {
    maybeAutoFillDeviceNumber();
    maybeAutoFillIndexFields();
}

// Load devices
async function loadDevices() {
    const devicesList = document.getElementById('devices-list');
    devicesList.innerHTML = '<p class="loading">Loading devices...</p>';

    try {
        const response = await fetch(
            API_BASE + '/management/v1/configureddevices?ts=' + Date.now(),
            { cache: 'no-store' }
        );
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        
        // Get response as text first to debug any JSON parsing issues
        const text = await response.text();
        console.log('Raw response text:', text);
        console.log('Response length:', text.length);
        
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            console.error('JSON parse error:', e);
            console.error('Response text that failed to parse:', text);
            devicesList.innerHTML = `<p class="error">Error loading devices: Invalid JSON response from server. Response: ${escapeHtml(text.substring(0, 200))}</p>`;
            return;
        }
        
        if (data.ErrorNumber !== 0) {
            devicesList.innerHTML = `<p class="error">Error: ${escapeHtml(data.ErrorMessage || 'Unknown error')}</p>`;
            return;
        }

        // Handle Value field - it can be a string (JSON), an array, or undefined
        let devices = [];
        
        if (data.Value !== undefined && data.Value !== null) {
            if (Array.isArray(data.Value)) {
                // Already an array
                devices = data.Value;
            } else if (typeof data.Value === 'string') {
                // JSON string - parse it
                try {
                    const parsed = JSON.parse(data.Value);
                    if (Array.isArray(parsed)) {
                        devices = parsed;
                    } else {
                        console.warn('Parsed Value is not an array:', parsed);
                    }
                } catch (e) {
                    console.error('Failed to parse Value:', e, 'Value was:', data.Value);
                    devicesList.innerHTML = `<p class="error">Error loading devices: Invalid JSON format</p>`;
                    return;
                }
            } else {
                console.warn('Unexpected Value type:', typeof data.Value, data.Value);
            }
        }
        
        if (devices.length === 0) {
            currentDevices = [];
            devicesList.innerHTML = `
                <div class="empty-state">
                    <p>No devices configured</p>
                    <p>Go to the "Configure" tab to add a device</p>
                </div>
            `;
            updateAutoNumbering();
            return;
        }

        const orderMap = new Map(ALPACA_DEVICE_ORDER.map((type, index) => [type, index]));
        const sortedDevices = devices.slice().sort((a, b) => {
            const aType = normalizeDeviceType(a.DeviceType || a.deviceType);
            const bType = normalizeDeviceType(b.DeviceType || b.deviceType);
            const aOrder = orderMap.has(aType) ? orderMap.get(aType) : ALPACA_DEVICE_ORDER.length;
            const bOrder = orderMap.has(bType) ? orderMap.get(bType) : ALPACA_DEVICE_ORDER.length;
            if (aOrder !== bOrder) {
                return aOrder - bOrder;
            }
            const aNumber = Number.isFinite(a.DeviceNumber) ? a.DeviceNumber : Number.parseInt(a.DeviceNumber, 10);
            const bNumber = Number.isFinite(b.DeviceNumber) ? b.DeviceNumber : Number.parseInt(b.DeviceNumber, 10);
            if (Number.isFinite(aNumber) && Number.isFinite(bNumber) && aNumber !== bNumber) {
                return aNumber - bNumber;
            }
            const aName = (a.DeviceName || a.Name || '').toString().toLowerCase();
            const bName = (b.DeviceName || b.Name || '').toString().toLowerCase();
            return aName.localeCompare(bName);
        });

        currentDevices = sortedDevices;
        devicesList.innerHTML = sortedDevices.map((device, index) => {
            const config = device.Config || device.config || null;
            const vendor = (device.Vendor || (config && config.vendor) || 'N/A').toString();
            const settingsHtml = renderDeviceSettings(config);
            const deviceName = device.DeviceName || device.Name || 'Unknown Device';
            const hasLoadError = device.LoadError === true;
            return `
            <div class="device-card collapsed${hasLoadError ? ' device-error' : ''}">
                <div class="device-card-header">
                    <h3>${hasLoadError ? '&#x26a0; ' : ''}${escapeHtml(deviceName)}</h3>
                    <button class="device-toggle" type="button" aria-expanded="false" data-device-index="${index}">
                        <span class="device-toggle-icon" aria-hidden="true"></span>
                        <span class="device-toggle-label">Details</span>
                    </button>
                </div>
                <div class="device-info">
                    <div class="info-item">
                        <span class="info-label">Type</span>
                        <span class="info-value">${escapeHtml(device.DeviceType)}</span>
                    </div>
                    <div class="info-item">
                        <span class="info-label">Device Number</span>
                        <span class="info-value">${device.DeviceNumber}</span>
                    </div>
                    <div class="info-item">
                        <span class="info-label">Unique ID</span>
                        <span class="info-value">${escapeHtml(device.UniqueID)}</span>
                    </div>
                    <div class="info-item">
                        <span class="info-label">Vendor</span>
                        <span class="info-value">${escapeHtml(vendor)}</span>
                    </div>
                    ${device.Firmware ? `
                    <div class="info-item">
                        <span class="info-label">Firmware</span>
                        <span class="info-value">${escapeHtml(device.Firmware)}</span>
                    </div>
                    ` : ''}
                    ${device.SdkVersion ? `
                    <div class="info-item">
                        <span class="info-label">SDK Version</span>
                        <span class="info-value">${escapeHtml(device.SdkVersion)}</span>
                    </div>
                    ` : ''}
                </div>
                ${settingsHtml}
                <div class="device-actions">
                    <button class="btn btn-secondary btn-small btn-edit-device" data-device-index="${index}" type="button">Edit</button>
                    <button class="btn btn-danger btn-small btn-delete-device" data-device-index="${index}" type="button">Delete</button>
                </div>
            </div>
        `;
        }).join('');

        document.querySelectorAll('.btn-edit-device').forEach(button => {
            button.addEventListener('click', handleEditDeviceClick);
        });
        document.querySelectorAll('.btn-delete-device').forEach(button => {
            button.addEventListener('click', handleDeleteDeviceClick);
        });
        document.querySelectorAll('.device-toggle').forEach(button => {
            button.addEventListener('click', () => {
                const card = button.closest('.device-card');
                if (!card) {
                    return;
                }
                const isCollapsed = card.classList.toggle('collapsed');
                button.setAttribute('aria-expanded', isCollapsed ? 'false' : 'true');
            });
        });
        updateAutoNumbering();
    } catch (error) {
        console.error('Error loading devices:', error);
        let errorMsg = 'Unknown error';
        if (error instanceof Error) {
            errorMsg = error.message;
        } else if (typeof error === 'string') {
            errorMsg = error;
        } else {
            try {
                errorMsg = JSON.stringify(error);
            } catch (e) {
                errorMsg = 'Error occurred (unable to stringify)';
            }
        }
        devicesList.innerHTML = `<p class="error">Error loading devices: ${escapeHtml(errorMsg)}</p>`;
    }
}

async function handleEditDeviceClick(event) {
    const button = event.currentTarget;
    const index = Number.parseInt(button.dataset.deviceIndex, 10);
    const clickedDevice = currentDevices[index];
    if (!clickedDevice) {
        return;
    }
    const deviceType = clickedDevice.DeviceType;
    const deviceNumber = clickedDevice.DeviceNumber;
    // Re-fetch before opening the form: currentDevices is only populated on
    // page load or after a submit from THIS tab, so a tab left open across an
    // out-of-band config change (another tab, a direct API call) would
    // otherwise populate the edit form from stale data and silently
    // re-persist it over the newer config on save.
    await loadDevices();
    // Look up by stable identity (DeviceType + DeviceNumber) rather than the
    // pre-reload positional index: an out-of-band change can alter the sort
    // order of currentDevices, so the same index could now point at a
    // different device.
    const device = currentDevices.find(d => d.DeviceType === deviceType && d.DeviceNumber === deviceNumber);
    if (!device) {
        return;
    }
    startEditDevice(device);
}

function handleDeleteDeviceClick(event) {
    const button = event.currentTarget;
    const index = Number.parseInt(button.dataset.deviceIndex, 10);
    const device = currentDevices[index];
    if (!device) {
        return;
    }
    deleteDevice(device.DeviceType, device.DeviceNumber);
}

function normalizeDeviceType(value) {
    if (!value) {
        return '';
    }
    return value.toString().trim().toLowerCase();
}

function setEditMode(isEditing) {
    const configureTitle = document.getElementById('configure-title');
    const submitButton = document.querySelector('#device-form button[type="submit"]');
    const form = document.getElementById('device-form');
    if (configureTitle) {
        configureTitle.textContent = isEditing ? 'Edit Device' : 'Add Device';
    }
    if (submitButton) {
        submitButton.textContent = isEditing ? 'Update Device' : 'Add Device';
    }
    if (form) {
        form.dataset.editing = isEditing ? 'true' : 'false';
        if (!isEditing) {
            delete form.dataset.originalDeviceType;
            delete form.dataset.originalDeviceNumber;
            delete form.dataset.originalVendor;
            const deviceNumberInput = document.getElementById('device-number');
            if (deviceNumberInput) {
                delete deviceNumberInput.dataset.userModified;
            }
            // Clear the manual-edit flag on every vendor index field so the next
            // fresh "Add Device" re-derives them instead of treating a prior
            // edit's value as user-pinned.
            INDEX_FIELDS.forEach(field => {
                const input = document.getElementById(field.fieldId);
                if (input) {
                    delete input.dataset.userModified;
                }
            });
            updateAutoNumbering();
        }
    }
}

function setFormValue(elementId, value) {
    const element = document.getElementById(elementId);
    if (!element) {
        return;
    }
    element.value = value !== undefined && value !== null ? value : '';
}

function updateApertureAreaFromDiameter(diameterId, areaId) {
    const diameterEl = document.getElementById(diameterId);
    const areaEl = document.getElementById(areaId);
    if (!diameterEl || !areaEl) {
        return;
    }

    const diameterMm = Number.parseFloat(diameterEl.value);
    if (!Number.isFinite(diameterMm) || diameterMm <= 0) {
        areaEl.value = '';
        return;
    }

    // Diameter is entered in mm; the derived area is shown in m^2, the unit
    // the Alpaca ApertureArea property reports.
    const radius = diameterMm / MM_PER_METER / 2;
    const area = Math.PI * radius * radius;
    areaEl.value = area.toFixed(6);
}

function startEditDevice(device) {
    const config = device.Config || device.config || {};
    const vendor = device.Vendor || config.vendor || '';
    const deviceType = normalizeDeviceType(device.DeviceType || device.deviceType);

    setEditMode(true);
    setFormValue('device-type', deviceType);
    setFormValue('device-number', device.DeviceNumber);
    setFormValue('vendor', vendor);
    updateVendorOptions();

    // The 'change' handler runs every vendor sub-section toggler
    // (updateZwoConfigFields / updateTouptekConfigFields /
    // updatePlayerOneConfigFields), so device-type-aware blocks like
    // playerone-filterwheel-fields are shown for edits through this
    // dispatch - no explicit toggler calls are needed here.
    document.getElementById('vendor').dispatchEvent(new Event('change'));

    if (vendor === 'ioptron' && deviceType === 'switch') {
        // iMate PowerBox: optional GPIO chip override plus PWM frequency and
        // per-port PWM flags (positional: index 1 = DC1, index 2 = DC2).
        if (config.gpioChip !== undefined && config.gpioChip !== null) {
            setFormValue('ioptron-powerbox-gpio-chip', config.gpioChip);
        }
        if (config.pwmFrequencyHz !== undefined && config.pwmFrequencyHz !== null) {
            setFormValue('ioptron-powerbox-pwm-frequency', config.pwmFrequencyHz);
        }
        if (Array.isArray(config.ports)) {
            [1, 2].forEach(function(i) {
                const pwmCheckbox = document.getElementById('ioptron-port-pwm-' + i);
                if (pwmCheckbox && config.ports[i]) {
                    pwmCheckbox.checked = config.ports[i].pwm === true;
                }
            });
        }
        updateIoptronConfigFields();
    } else if (vendor === 'ioptron' && deviceType === 'filterwheel') {
        // iEFW: USB-serial only, fixed 115200 baud.
        const iefwConnectionType = config.connectionType || 'auto';
        setFormValue('ioptron-filterwheel-model', config.model || 'iefw15');
        setFormValue('ioptron-filterwheel-connection-type', iefwConnectionType);
        if (iefwConnectionType === 'serial') {
            setFormValue('ioptron-filterwheel-port-path', config.portPath);
        } else if (config.filterwheelIndex !== undefined && config.filterwheelIndex !== null) {
            setFormValue('ioptron-filterwheel-index', config.filterwheelIndex);
        }
        const iefwConnectionTypeEl = document.getElementById('ioptron-filterwheel-connection-type');
        if (iefwConnectionTypeEl) {
            iefwConnectionTypeEl.dispatchEvent(new Event('change'));
        }
        const ioptronFilterNamesField = document.getElementById('ioptron-filter-names');
        if (ioptronFilterNamesField) {
            ioptronFilterNamesField.value = Array.isArray(config.filterNames)
                ? config.filterNames.join('\n')
                : '';
            ioptronFilterwheelSlotUI.syncSlotsFromTextarea();
        }
        updateIoptronConfigFields();
    } else if (vendor === 'ioptron' && deviceType === 'focuser') {
        // iEAF: USB-serial only, fixed 115200 baud.
        const ieafConnectionType = config.connectionType || 'auto';
        setFormValue('ioptron-ieaf-model', config.model || 'ieaf');
        setFormValue('ioptron-ieaf-connection-type', ieafConnectionType);
        if (ieafConnectionType === 'serial') {
            setFormValue('ioptron-ieaf-port-path', config.portPath);
        } else if (config.focuserIndex !== undefined && config.focuserIndex !== null) {
            setFormValue('ioptron-ieaf-focuser-index', config.focuserIndex);
        }
        const ieafConnectionTypeEl = document.getElementById('ioptron-ieaf-connection-type');
        if (ieafConnectionTypeEl) {
            ieafConnectionTypeEl.dispatchEvent(new Event('change'));
        }
        updateIoptronConfigFields();
    } else if (vendor === 'ioptron') {
        const ioptronConnectionType = config.connectionType || 'auto';
        setFormValue('ioptron-connection-type', ioptronConnectionType);
        if (ioptronConnectionType === 'serial') {
            setFormValue('ioptron-port-path', config.portPath);
            setFormValue('ioptron-baud-rate', config.baudRate);
        } else if (ioptronConnectionType === 'network') {
            setFormValue('ioptron-host', config.host);
            setFormValue('ioptron-tcp-port', config.tcpPort);
        }
        setFormValue('aperture-diameter', opticsMetersToMm(config.apertureDiameter));
        setFormValue('focal-length', opticsMetersToMm(config.focalLength));
        updateApertureAreaFromDiameter('aperture-diameter', 'aperture-area');
        const ioptronConnectionTypeEl = document.getElementById('ioptron-connection-type');
        if (ioptronConnectionTypeEl) {
            ioptronConnectionTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'synscan') {
        setFormValue('synscan-version', config.synscanVersion || 'auto');
        const synscanConnectionType = config.connectionType || 'auto';
        setFormValue('synscan-connection-type', synscanConnectionType);
        if (synscanConnectionType === 'serial') {
            setFormValue('synscan-port-path', config.portPath);
            setFormValue('synscan-baud-rate', config.baudRate);
        } else if (synscanConnectionType === 'network') {
            setFormValue('synscan-host', config.host);
            setFormValue('synscan-tcp-port', config.tcpPort);
        }
        setFormValue('synscan-aperture-diameter', opticsMetersToMm(config.apertureDiameter));
        setFormValue('synscan-focal-length', opticsMetersToMm(config.focalLength));
        updateApertureAreaFromDiameter('synscan-aperture-diameter', 'synscan-aperture-area');
        const synscanConnectionTypeEl = document.getElementById('synscan-connection-type');
        if (synscanConnectionTypeEl) {
            synscanConnectionTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'skywatcher') {
        const skywatcherConnectionType = config.connectionType || 'auto';
        setFormValue('skywatcher-connection-type', skywatcherConnectionType);
        if (skywatcherConnectionType === 'serial') {
            setFormValue('skywatcher-port-path', config.portPath);
            setFormValue('skywatcher-baud-rate', config.baudRate);
        } else if (skywatcherConnectionType === 'network') {
            setFormValue('skywatcher-host', config.host);
            setFormValue('skywatcher-udp-port', config.udpPort);
        }
        setFormValue('skywatcher-aperture-diameter', opticsMetersToMm(config.apertureDiameter));
        setFormValue('skywatcher-focal-length', opticsMetersToMm(config.focalLength));
        setFormValue('skywatcher-site-latitude', config.siteLatitude);
        setFormValue('skywatcher-site-longitude', config.siteLongitude);
        setFormValue('skywatcher-site-elevation', config.siteElevation);
        updateApertureAreaFromDiameter('skywatcher-aperture-diameter', 'skywatcher-aperture-area');
        const skywatcherConnectionTypeEl = document.getElementById('skywatcher-connection-type');
        if (skywatcherConnectionTypeEl) {
            skywatcherConnectionTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'onstep') {
        const onstepConnectionType = config.connectionType || 'auto';
        setFormValue('onstep-connection-type', onstepConnectionType);
        if (onstepConnectionType === 'serial') {
            setFormValue('onstep-port-path', config.portPath);
            setFormValue('onstep-baud-rate', config.baudRate);
        }
        setFormValue('onstep-aperture-diameter', opticsMetersToMm(config.apertureDiameter));
        setFormValue('onstep-focal-length', opticsMetersToMm(config.focalLength));
        updateApertureAreaFromDiameter('onstep-aperture-diameter', 'onstep-aperture-area');
        const onstepConnectionTypeEl = document.getElementById('onstep-connection-type');
        if (onstepConnectionTypeEl) {
            onstepConnectionTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'celestron') {
        const celestronConnectionType = config.connectionType || 'auto';
        setFormValue('celestron-connection-type', celestronConnectionType);
        if (celestronConnectionType === 'serial') {
            setFormValue('celestron-port-path', config.portPath);
            setFormValue('celestron-baud-rate', config.baudRate);
        } else {
            setFormValue('celestron-host', config.host);
            setFormValue('celestron-tcp-port', config.tcpPort);
        }
        setFormValue('celestron-aperture-diameter', opticsMetersToMm(config.apertureDiameter));
        setFormValue('celestron-focal-length', opticsMetersToMm(config.focalLength));
        const celestronConnectionTypeEl = document.getElementById('celestron-connection-type');
        if (celestronConnectionTypeEl) {
            celestronConnectionTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'bisque') {
        setFormValue('bisque-host', config.host || 'localhost');
        setFormValue('bisque-tcp-port', config.tcpPort || 3040);
        setFormValue('bisque-aperture-diameter', opticsMetersToMm(config.apertureDiameter));
        setFormValue('bisque-focal-length', opticsMetersToMm(config.focalLength));
    } else if (vendor === 'zwo' && deviceType === 'telescope') {
        const zwoMountConnectionType = config.connectionType || 'serial';
        setFormValue('zwo-mount-connection-type', zwoMountConnectionType);
        if (zwoMountConnectionType === 'serial') {
            setFormValue('zwo-mount-port-path', config.portPath);
            setFormValue('zwo-mount-baud-rate', config.baudRate);
        } else {
            setFormValue('zwo-mount-host', config.host);
            setFormValue('zwo-mount-tcp-port', config.tcpPort);
        }
        setFormValue('zwo-mount-aperture-diameter', opticsMetersToMm(config.apertureDiameter));
        setFormValue('zwo-mount-focal-length', opticsMetersToMm(config.focalLength));
        setFormValue('zwo-mount-site-latitude', config.siteLatitude);
        setFormValue('zwo-mount-site-longitude', config.siteLongitude);
        setFormValue('zwo-mount-site-elevation', config.siteElevation);
        const zwoMountSyncTimeCheckbox = document.getElementById('zwo-mount-sync-time-on-connect');
        if (zwoMountSyncTimeCheckbox) {
            zwoMountSyncTimeCheckbox.checked = config.syncTimeOnConnect !== false;
        }
        updateApertureAreaFromDiameter('zwo-mount-aperture-diameter', 'zwo-mount-aperture-area');
        const zwoMountConnectionTypeEl = document.getElementById('zwo-mount-connection-type');
        if (zwoMountConnectionTypeEl) {
            zwoMountConnectionTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'qhy' && deviceType === 'filterwheel') {
        // Integrated CFW: bound to the same camera index/id as the paired
        // QHY camera device, but stored under its own field names.
        setFormValue('qhy-cfw-camera-index', config.cameraIndex);
        setFormValue('qhy-cfw-camera-id', config.cameraId);
        const qhyFilterNamesField = document.getElementById('qhy-filter-names');
        if (qhyFilterNamesField) {
            qhyFilterNamesField.value = Array.isArray(config.filterNames)
                ? config.filterNames.join('\n')
                : '';
            qhyFilterwheelSlotUI.syncSlotsFromTextarea();
        }
    } else if (vendor === 'qhy') {
        setFormValue('qhy-camera-index', config.cameraIndex);
        setFormValue('qhy-camera-id', config.cameraId);
    } else if (vendor === 'astroasis') {
        setFormValue('astroasis-focuser-index', config.focuserIndex);
        setFormValue('astroasis-hid-path', config.hidPath);
    } else if (vendor === 'svbony') {
        setFormValue('svbony-camera-index', config.cameraIndex);
    } else if (vendor === 'touptek' && deviceType === 'switch') {
        // switchType selects the backend: 'thermal' (camera dew heater + fan) or
        // 'stellavita' (GPIO PowerBox, the default for legacy configs).
        const touptekSwitchType = config.switchType || 'stellavita';
        setFormValue('touptek-switch-type', touptekSwitchType);
        if (touptekSwitchType === 'thermal') {
            setFormValue('touptek-thermal-camera-index', config.cameraIndex);
        }
        // Re-run the sub-block toggle now that switchType is set, so the thermal
        // vs StellaVita fields match the loaded config (mirrors ZWO's pattern).
        const touptekSwitchTypeEl = document.getElementById('touptek-switch-type');
        if (touptekSwitchTypeEl) {
            touptekSwitchTypeEl.dispatchEvent(new Event('change'));
        }
        // StellaVita PowerBox: optional GPIO chip override plus PWM frequency
        // and per-port PWM flags (positional: index 0 = Port 1 .. 3 = Port 4).
        if (config.gpioChip !== undefined && config.gpioChip !== null) {
            setFormValue('touptek-powerbox-gpio-chip', config.gpioChip);
        }
        if (config.pwmFrequencyHz !== undefined && config.pwmFrequencyHz !== null) {
            setFormValue('touptek-powerbox-pwm-frequency', config.pwmFrequencyHz);
        }
        if (Array.isArray(config.ports)) {
            [0, 1, 2, 3].forEach(function(i) {
                const pwmCheckbox = document.getElementById('touptek-port-pwm-' + i);
                if (pwmCheckbox && config.ports[i]) {
                    pwmCheckbox.checked = config.ports[i].pwm === true;
                }
            });
        }
    } else if (vendor === 'touptek') {
        setFormValue('touptek-camera-index', config.cameraIndex);
        setFormValue('touptek-focuser-index', config.focuserIndex);
        setFormValue('touptek-focuser-id', config.focuserId);
        setFormValue('touptek-filterwheel-index', config.filterwheelIndex);
        setFormValue('touptek-filterwheel-id', config.filterwheelId);
        // Only touch the filter-name/slot UI when editing a filter wheel;
        // otherwise editing a ToupTek camera/focuser would blank the slot list.
        if (deviceType === 'filterwheel') {
            const touptekFilterNamesField = document.getElementById('touptek-filter-names');
            if (touptekFilterNamesField) {
                touptekFilterNamesField.value = Array.isArray(config.filterNames)
                    ? config.filterNames.join('\n')
                    : '';
                touptekFilterwheelSlotUI.syncSlotsFromTextarea();
            }
        }
    } else if (vendor === 'playerone') {
        setFormValue('playerone-camera-index', config.cameraIndex);
        setFormValue('playerone-switch-camera-index', config.cameraIndex);
        setFormValue('playerone-filterwheel-index', config.filterwheelIndex);
        // Only touch the filter-name/slot UI when editing a filter wheel;
        // otherwise editing a Player One camera/switch would blank the slot list.
        if (deviceType === 'filterwheel') {
            const playerOneFilterNamesField = document.getElementById('playerone-filter-names');
            if (playerOneFilterNamesField) {
                playerOneFilterNamesField.value = Array.isArray(config.filterNames)
                    ? config.filterNames.join('\n')
                    : '';
                playerOneFilterwheelSlotUI.syncSlotsFromTextarea();
            }
        }
    } else if (vendor === 'weewx') {
        setFormValue('weewx-url', config.weewxUrl);
        setFormValue('weewx-poll-interval', config.pollIntervalSeconds);
        setFormValue('weewx-timeout', config.timeoutMs);
    } else if (vendor === 'gemini' && deviceType === 'covercalibrator') {
        const model = (config.flatPanelModel === 'v2' || config.flatPanelModel === 'pro') ? config.flatPanelModel : 'lite';
        setFormValue('gemini-flatpanel-model', model);
        const connType = config.connectionType || 'auto';
        if (model === 'pro') {
            setFormValue('gemini-flatpanel-pro-connection-type', connType);
            if (connType === 'auto') {
                setFormValue('gemini-flatpanel-pro-index', config.panelIndex);
            } else if (connType === 'serial') {
                setFormValue('gemini-flatpanel-pro-port-path', config.portPath);
                setFormValue('gemini-flatpanel-pro-baud-rate', config.baudRate);
            }
            const geminiFlatPanelProConnTypeEl = document.getElementById('gemini-flatpanel-pro-connection-type');
            if (geminiFlatPanelProConnTypeEl) {
                geminiFlatPanelProConnTypeEl.dispatchEvent(new Event('change'));
            }
        } else if (model === 'v2') {
            setFormValue('gemini-flatpanel-v2-connection-type', connType);
            if (connType === 'auto') {
                setFormValue('gemini-flatpanel-v2-index', config.panelIndex);
            } else if (connType === 'serial') {
                setFormValue('gemini-flatpanel-v2-port-path', config.portPath);
                setFormValue('gemini-flatpanel-v2-baud-rate', config.baudRate);
            }
            const geminiFlatPanelV2ConnTypeEl = document.getElementById('gemini-flatpanel-v2-connection-type');
            if (geminiFlatPanelV2ConnTypeEl) {
                geminiFlatPanelV2ConnTypeEl.dispatchEvent(new Event('change'));
            }
        } else {
            setFormValue('gemini-flatpanel-connection-type', connType);
            if (connType === 'auto') {
                setFormValue('gemini-flatpanel-index', config.panelIndex);
            } else if (connType === 'serial') {
                setFormValue('gemini-flatpanel-port-path', config.portPath);
                setFormValue('gemini-flatpanel-baud-rate', config.baudRate);
            }
            const geminiFlatPanelConnTypeEl = document.getElementById('gemini-flatpanel-connection-type');
            if (geminiFlatPanelConnTypeEl) {
                geminiFlatPanelConnTypeEl.dispatchEvent(new Event('change'));
            }
        }
        const geminiFlatPanelModelEl = document.getElementById('gemini-flatpanel-model');
        if (geminiFlatPanelModelEl) {
            geminiFlatPanelModelEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'gemini') {
        const connType = config.connectionType || 'auto';
        setFormValue('gemini-connection-type', connType);
        if (connType === 'auto') {
            setFormValue('gemini-focuser-index', config.focuserIndex);
        } else if (connType === 'serial') {
            setFormValue('gemini-port-path', config.portPath);
            setFormValue('gemini-baud-rate', config.baudRate);
        }
        const geminiConnTypeEl = document.getElementById('gemini-connection-type');
        if (geminiConnTypeEl) {
            geminiConnTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'wandererastro') {
        const connType = config.connectionType || 'auto';
        if (normalizeDeviceType(config.deviceType) === 'filterwheel') {
            setFormValue('wandererastro-filterwheel-connection-type', connType);
            if (connType === 'auto') {
                setFormValue('wandererastro-filterwheel-index', config.wandererFilterwheelIndex);
            } else if (connType === 'serial') {
                setFormValue('wandererastro-filterwheel-port-path', config.portPath);
                setFormValue('wandererastro-filterwheel-baud-rate', config.baudRate);
            }
            const wandererFwConnTypeEl = document.getElementById('wandererastro-filterwheel-connection-type');
            if (wandererFwConnTypeEl) {
                wandererFwConnTypeEl.dispatchEvent(new Event('change'));
            }
            const wandererFilterNamesField = document.getElementById('wandererastro-filter-names');
            if (wandererFilterNamesField) {
                wandererFilterNamesField.value = Array.isArray(config.filterNames)
                    ? config.filterNames.join('\n')
                    : '';
                wandererFilterwheelSlotUI.syncSlotsFromTextarea();
            }
        } else if (normalizeDeviceType(config.deviceType) === 'rotator') {
            setFormValue('wandererastro-rotator-connection-type', connType);
            if (connType === 'auto') {
                setFormValue('wandererastro-rotator-index', config.rotatorIndex);
            } else if (connType === 'serial') {
                setFormValue('wandererastro-rotator-port-path', config.portPath);
                setFormValue('wandererastro-rotator-baud-rate', config.baudRate);
            }
            const wandererRotatorConnTypeEl = document.getElementById('wandererastro-rotator-connection-type');
            if (wandererRotatorConnTypeEl) {
                wandererRotatorConnTypeEl.dispatchEvent(new Event('change'));
            }
        } else if (normalizeDeviceType(config.deviceType) === 'switch') {
            setFormValue('wandererastro-box-connection-type', connType);
            if (connType === 'auto') {
                setFormValue('wandererastro-box-index', config.boxIndex);
            } else if (connType === 'serial') {
                setFormValue('wandererastro-box-port-path', config.portPath);
                setFormValue('wandererastro-box-baud-rate', config.baudRate);
            }
            const wandererBoxConnTypeEl = document.getElementById('wandererastro-box-connection-type');
            if (wandererBoxConnTypeEl) {
                wandererBoxConnTypeEl.dispatchEvent(new Event('change'));
            }
        } else {
            setFormValue('wandererastro-connection-type', connType);
            if (connType === 'auto') {
                setFormValue('wandererastro-cover-index', config.coverIndex);
            } else if (connType === 'serial') {
                setFormValue('wandererastro-port-path', config.portPath);
                setFormValue('wandererastro-baud-rate', config.baudRate);
            }
            const wandererConnTypeEl = document.getElementById('wandererastro-connection-type');
            if (wandererConnTypeEl) {
                wandererConnTypeEl.dispatchEvent(new Event('change'));
            }
        }
    }
    setFormValue('camera-index', config.cameraIndex);
    setFormValue('camera-id', config.cameraId);
    setFormValue('filterwheel-index', config.filterwheelIndex);
    setFormValue('filterwheel-id', config.filterwheelId);
    const filterNamesField = document.getElementById('filterwheel-names');
    if (filterNamesField) {
        if (Array.isArray(config.filterNames)) {
            filterNamesField.value = config.filterNames.join('\n');
        } else {
            filterNamesField.value = '';
        }
        zwoFilterwheelSlotUI.syncSlotsFromTextarea();
    }

    setFormValue('focuser-index', config.focuserIndex);
    setFormValue('focuser-id', config.focuserId);
    setFormValue('rotator-index', config.rotatorIndex);
    setFormValue('rotator-id', config.rotatorId);
    setFormValue('zwo-switch-type', config.switchType);

    // Populate the ASIAIR Pro Switch per-port table from the saved config.
    // When the saved device omits ports/gpioChip/pwmFrequencyHz (one-click
    // default flow), the HTML's pre-filled defaults remain in place.
    if (vendor === 'zwo' &&
        (config.switchType === 'asiair' || config.switchType === 'asiair-plus-picm4')) {
        if (config.gpioChip !== undefined && config.gpioChip !== null) {
            setFormValue('asiair-gpio-chip', config.gpioChip);
        }
        if (config.pwmFrequencyHz !== undefined && config.pwmFrequencyHz !== null) {
            setFormValue('asiair-pwm-frequency', config.pwmFrequencyHz);
        }
        if (Array.isArray(config.ports)) {
            for (let i = 0; i < 4; i += 1) {
                const port = config.ports[i];
                if (!port) continue;
                if (port.name !== undefined && port.name !== null) {
                    setFormValue('asiair-port-name-' + i, port.name);
                }
                if (port.gpio !== undefined && port.gpio !== null) {
                    setFormValue('asiair-port-gpio-' + i, port.gpio);
                }
                const pwmCheckbox = document.getElementById('asiair-port-pwm-' + i);
                if (pwmCheckbox) {
                    pwmCheckbox.checked = port.pwm === true;
                }
            }
        }
    }
    // Populate the ASIAIR Plus (RK3568) per-port table from the saved config.
    // The kernel module fixes the per-port hardware mapping, so only the
    // device path, PWM frequency, channel names and per-port PWM flags are
    // configurable here.
    if (vendor === 'zwo' && config.switchType === 'asiair-plus-rk3568') {
        if (config.devicePath !== undefined && config.devicePath !== null) {
            setFormValue('asiair-plus-device-path', config.devicePath);
        }
        // pwmFrequencyHz was previously surfaced here as a user-editable
        // field. It's now auto-managed by the wrapper (defaults to 50 Hz,
        // matching what ZWO's stock zwoair_imager daemon actually uses -
        // see the comment block in default_asiair_plus_rk3568_config())
        // and intentionally not shown in the UI. The persisted value, if
        // any, is still passed through by the router so power users can
        // set it via direct JSON edits.
        if (Array.isArray(config.ports)) {
            for (let i = 0; i < 4; i += 1) {
                const port = config.ports[i];
                if (!port) continue;
                if (port.name !== undefined && port.name !== null) {
                    setFormValue('asiair-plus-port-name-' + i, port.name);
                }
                const pwmCheckbox = document.getElementById('asiair-plus-port-pwm-' + i);
                if (pwmCheckbox) {
                    pwmCheckbox.checked = port.pwm === true;
                }
            }
        }
    }
    // Trigger the visibility update so the ASIAIR section actually appears
    // when editing an asiair-typed switch (vs. dewheater).
    const zwoSwitchTypeEl = document.getElementById('zwo-switch-type');
    if (zwoSwitchTypeEl) {
        zwoSwitchTypeEl.dispatchEvent(new Event('change'));
    }

    const messageDiv = document.getElementById('form-message');
    if (messageDiv) {
        messageDiv.style.display = 'none';
    }

    const form = document.getElementById('device-form');
    if (form) {
        form.dataset.originalDeviceType = deviceType;
        form.dataset.originalDeviceNumber = String(device.DeviceNumber);
        form.dataset.originalVendor = vendor;
    }

    // Switch to the Configure tab without resetting the form we just populated.
    showTab('configure', { preserveForm: true });
}

// Refresh devices
function refreshDevices() {
    loadDevices();
}

// Delete device
async function deleteDevice(deviceType, deviceNumber) {
    const normalizedType = normalizeDeviceType(deviceType);
    if (!normalizedType) {
        alert('Error deleting device: Invalid device type');
        return;
    }

    if (!confirm(`Are you sure you want to delete ${deviceType} device #${deviceNumber}?`)) {
        return;
    }
    
    try {
        const response = await fetch(API_BASE + '/management/v1/removedevice', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                deviceType: normalizedType,
                deviceNumber: deviceNumber
            })
        });

        const result = await response.json();
        
        if (result.ErrorNumber === 0) {
            alert('Device deleted successfully!');
            loadDevices();
        } else {
            alert('Error deleting device: ' + result.ErrorMessage);
        }
    } catch (error) {
        alert('Error deleting device: ' + error.message);
    }
}

// Load server info
async function loadServerInfo() {
    const serverInfo = document.getElementById('server-info');
    serverInfo.innerHTML = '<p class="loading">Loading server information...</p>';

    try {
        const response = await fetch(API_BASE + '/management/v1/description');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        
        // Get response as text first to debug any JSON parsing issues
        const text = await response.text();
        console.log('Server info raw response text:', text);
        
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            console.error('JSON parse error for server info:', e);
            console.error('Response text that failed to parse:', text);
            serverInfo.innerHTML = `<p class="error">Error loading server info: Invalid JSON response from server</p>`;
            return;
        }
        
        if (data.ErrorNumber !== 0) {
            serverInfo.innerHTML = `<p class="error">Error: ${escapeHtml(data.ErrorMessage || 'Unknown error')}</p>`;
            return;
        }

        const desc = parseResponseValue(data.Value) || {};
        const serverName = resolveDescriptionValue(desc, ['ServerName', 'serverName']) || 'N/A';
        const manufacturer = resolveDescriptionValue(desc, ['Manufacturer', 'manufacturer']) || 'N/A';
        const manufacturerVersion = resolveDescriptionValue(desc, ['ManufacturerVersion', 'manufacturerVersion', 'Version', 'version']) || 'N/A';
        const location = resolveDescriptionValue(desc, ['Location', 'location']) || '';
        const profileName = resolveDescriptionValue(desc, ['ProfileName', 'profileName', 'profile_name']) || '';

        // Mirror the server-reported version (sourced from the VERSION file at
        // build time) into the header badge.
        const headerVersion = document.getElementById('header-version');
        if (headerVersion) {
            headerVersion.textContent = manufacturerVersion !== 'N/A' ? 'v' + manufacturerVersion : '';
        }
        updateHeaderProfileName(profileName);

        serverInfo.innerHTML = `
            <div class="server-info-grid">
                ${renderServerInfoRow('Server Name', serverName)}
                ${renderServerInfoRow('Manufacturer', manufacturer)}
                ${renderServerInfoRow('Version', manufacturerVersion)}
                <div class="server-info-row">
                    <span class="info-label">Profile Name</span>
                    <div class="server-location">
                        <input id="server-profile-input" type="text" placeholder="e.g. Backyard Rig">
                    </div>
                    <button id="server-profile-save" class="btn btn-secondary btn-small" type="button">Save</button>
                </div>
                <div class="server-info-row">
                    <span class="info-label">Location</span>
                    <div class="server-location">
                        <input id="server-location-input" type="text" placeholder="City, State/Province, Country">
                    </div>
                    <button id="server-location-save" class="btn btn-secondary btn-small" type="button">Save</button>
                </div>
            </div>
        `;

        const locationInput = document.getElementById('server-location-input');
        if (locationInput) {
            locationInput.value = location;
            locationInput.addEventListener('keydown', event => {
                if (event.key === 'Enter') {
                    event.preventDefault();
                    updateServerLocation();
                }
            });
        }
        const locationSaveButton = document.getElementById('server-location-save');
        if (locationSaveButton) {
            locationSaveButton.addEventListener('click', updateServerLocation);
        }
        const profileInput = document.getElementById('server-profile-input');
        if (profileInput) {
            profileInput.value = profileName;
            profileInput.addEventListener('keydown', event => {
                if (event.key === 'Enter') {
                    event.preventDefault();
                    updateServerProfileName();
                }
            });
        }
        const profileSaveButton = document.getElementById('server-profile-save');
        if (profileSaveButton) {
            profileSaveButton.addEventListener('click', updateServerProfileName);
        }
        setServerInfoStatus('');
    } catch (error) {
        console.error('Error loading server info:', error);
        let errorMsg = 'Unknown error';
        if (error instanceof Error) {
            errorMsg = error.message;
        } else if (typeof error === 'string') {
            errorMsg = error;
        } else {
            try {
                errorMsg = JSON.stringify(error);
            } catch (e) {
                errorMsg = 'Error occurred (unable to stringify)';
            }
        }
        serverInfo.innerHTML = `<p class="error">Error loading server info: ${escapeHtml(errorMsg)}</p>`;
        setServerInfoStatus('');
    }
}

function renderServerInfoRow(label, value) {
    const displayValue = value !== undefined && value !== null && value !== '' ? value : 'N/A';
    return `
        <div class="server-info-row">
            <span class="info-label">${escapeHtml(label)}</span>
            <span class="info-value">${escapeHtml(String(displayValue))}</span>
        </div>
    `;
}

function resolveDescriptionValue(desc, keys) {
    if (!desc || typeof desc !== 'object') {
        return '';
    }
    for (const key of keys) {
        if (Object.prototype.hasOwnProperty.call(desc, key)) {
            return desc[key];
        }
    }
    return '';
}

function setServerInfoStatus(message, isError = false) {
    const status = document.getElementById('server-info-status');
    if (!status) {
        return;
    }
    status.textContent = message;
    status.classList.toggle('error', isError);
}

async function updateServerLocation() {
    const locationInput = document.getElementById('server-location-input');
    const locationSaveButton = document.getElementById('server-location-save');
    if (!locationInput) {
        return;
    }

    const location = locationInput.value || '';
    setServerInfoStatus('Updating location...');
    if (locationSaveButton) {
        locationSaveButton.dataset.originalLabel = locationSaveButton.textContent;
        locationSaveButton.textContent = 'Saving...';
        locationSaveButton.disabled = true;
    }
    locationInput.disabled = true;

    try {
        const response = await fetch(API_BASE + '/management/v1/description', {
            method: 'PUT',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({Location: location})
        });

        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const text = await response.text();
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            throw new Error('Invalid JSON response from server');
        }

        if (data.ErrorNumber !== 0) {
            throw new Error(data.ErrorMessage || 'Unknown server error');
        }

        const payload = parseResponseValue(data.Value) || {};
        const updatedLocation = resolveDescriptionValue(payload, ['Location', 'location']);
        if (updatedLocation !== undefined && updatedLocation !== null) {
            locationInput.value = updatedLocation;
        }
        setServerInfoStatus('Location updated.');
        if (locationSaveButton) {
            locationSaveButton.textContent = 'Saved';
            setTimeout(() => {
                locationSaveButton.textContent = locationSaveButton.dataset.originalLabel || 'Save';
                delete locationSaveButton.dataset.originalLabel;
            }, 1500);
        }
    } catch (error) {
        setServerInfoStatus(`Failed to update location: ${error.message}`, true);
        if (locationSaveButton) {
            locationSaveButton.textContent = locationSaveButton.dataset.originalLabel || 'Save';
            delete locationSaveButton.dataset.originalLabel;
        }
    } finally {
        if (locationSaveButton) {
            locationSaveButton.disabled = false;
        }
        locationInput.disabled = false;
    }
}

function updateHeaderProfileName(profileName) {
    const headerProfile = document.getElementById('header-profile');
    if (headerProfile) {
        headerProfile.textContent = profileName || '';
    }
}

async function updateServerProfileName() {
    const profileInput = document.getElementById('server-profile-input');
    const profileSaveButton = document.getElementById('server-profile-save');
    if (!profileInput) {
        return;
    }

    const profileName = profileInput.value || '';
    setServerInfoStatus('Updating profile name...');
    if (profileSaveButton) {
        profileSaveButton.dataset.originalLabel = profileSaveButton.textContent;
        profileSaveButton.textContent = 'Saving...';
        profileSaveButton.disabled = true;
    }
    profileInput.disabled = true;

    try {
        const response = await fetch(API_BASE + '/management/v1/description', {
            method: 'PUT',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({ProfileName: profileName})
        });

        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const text = await response.text();
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            throw new Error('Invalid JSON response from server');
        }

        if (data.ErrorNumber !== 0) {
            throw new Error(data.ErrorMessage || 'Unknown server error');
        }

        const payload = parseResponseValue(data.Value) || {};
        const updatedProfileName = resolveDescriptionValue(payload, ['ProfileName', 'profileName', 'profile_name']);
        if (updatedProfileName !== undefined && updatedProfileName !== null) {
            profileInput.value = updatedProfileName;
        }
        updateHeaderProfileName(profileInput.value);
        setServerInfoStatus('Profile name updated.');
        if (profileSaveButton) {
            profileSaveButton.textContent = 'Saved';
            setTimeout(() => {
                profileSaveButton.textContent = profileSaveButton.dataset.originalLabel || 'Save';
                delete profileSaveButton.dataset.originalLabel;
            }, 1500);
        }
    } catch (error) {
        setServerInfoStatus(`Failed to update profile name: ${error.message}`, true);
        if (profileSaveButton) {
            profileSaveButton.textContent = profileSaveButton.dataset.originalLabel || 'Save';
            delete profileSaveButton.dataset.originalLabel;
        }
    } finally {
        if (profileSaveButton) {
            profileSaveButton.disabled = false;
        }
        profileInput.disabled = false;
    }
}

function refreshServerInfo() {
    loadServerInfo();
    loadLogSettings();
    loadLogFiles();
    wifiRefresh();
}

// Sync the SBC's system clock from the browser's clock. The browser machine
// is assumed to have correct time (its OS NTP), which makes it a simple
// time source for internet-less SBCs that cannot reach an NTP server.
async function syncTime() {
    if (!confirm('Sync the server\'s clock to this computer\'s time?\n\nThis is useful when the server has no internet (no NTP) and its clock drifts or resets after a reboot.')) {
        return;
    }

    // Capture the epoch AFTER the user confirms - confirm() blocks, so an
    // epoch taken before it would be stale by however long the dialog was
    // open.
    const epoch = Math.floor(Date.now() / 1000);
    const t0 = Date.now();

    try {
        const response = await fetch(API_BASE + '/management/v1/synctime', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ Epoch: epoch })
        });

        let result = null;
        try {
            result = await response.json();
        } catch (e) {
            result = null;
        }

        if (result && result.ErrorNumber === 0) {
            // Account for round-trip latency so the confirmation shows the
            // server's adjusted time, not the browser's send time.
            const roundTripMs = Date.now() - t0;
            const serverTime = new Date((result.Value * 1000) + Math.floor(roundTripMs / 2));
            refreshServerClockOffset();
            alert('Time synced! Server time is now ' + serverTime.toLocaleString() + ' (UTC offset ' + (serverTime.getTimezoneOffset() / -60) + 'h).');
        } else {
            alert('Error syncing time: ' + (result ? result.ErrorMessage : 'unknown error'));
        }
    } catch (e) {
        alert('Error syncing time: ' + e.message);
    }
}

// Live server clock: fetch the SBC's time once, remember its offset from the
// browser's clock, and tick the display locally every second - no per-second
// network traffic. Re-synced every 60 s and after a Sync Time. If server and
// browser disagree by more than 2 s the clock turns red as a "needs sync" hint.
let serverClockOffsetMs = null;

async function refreshServerClockOffset() {
    try {
        const t0 = Date.now();
        const response = await fetch(API_BASE + '/management/v1/synctime');
        const result = await response.json();
        if (result && result.ErrorNumber === 0) {
            // Value is whole seconds; assume the server read its clock halfway
            // through the round trip.
            const midpoint = t0 + (Date.now() - t0) / 2;
            serverClockOffsetMs = (result.Value * 1000) - midpoint;
        }
    } catch (e) {
        // Keep the last known offset on a transient fetch failure - the clock
        // keeps ticking locally rather than blanking to --:--:--. It only
        // shows placeholders before the first successful fetch.
    }
    updateServerClock();
}

function updateServerClock() {
    const el = document.getElementById('server-clock');
    if (!el) {
        return;
    }
    if (serverClockOffsetMs === null) {
        el.textContent = '--:--:--';
        el.classList.remove('drift');
        return;
    }
    const serverNow = new Date(Date.now() + serverClockOffsetMs);
    el.textContent = serverNow.toISOString().replace('T', ' ').substring(0, 19) + ' UTC';
    // The GET returns whole seconds, so up to ±1 s of the offset is
    // quantization, not drift; only flag beyond 2 s.
    el.classList.toggle('drift', Math.abs(serverClockOffsetMs) > 2000);
}

// Shutdown server
async function shutdownServer() {
    if (!confirm('Are you sure you want to shutdown the server? This will stop all services.')) {
        return;
    }
    
    try {
        const response = await fetch(API_BASE + '/management/v1/shutdown', {
            method: 'POST'
        });
        
        let result = null;
        try {
            result = await response.json();
        } catch (e) {
            result = null;
        }

        if (!result || result.ErrorNumber === 0) {
            alert('Server shutdown initiated. The server will stop shortly.');
            // Optionally redirect or show a message
            setTimeout(() => {
                document.body.innerHTML = '<div style="text-align: center; padding: 50px;"><h1>Server Shutdown</h1><p>The server has been shut down.</p></div>';
            }, 1000);
        } else {
            alert('Error shutting down server: ' + result.ErrorMessage);
        }
    } catch (error) {
        alert('Shutdown request sent. If the server does not stop, check the server logs. Error: ' + error.message);
    }
}

// Restart server
async function restartServer() {
    if (!confirm('Are you sure you want to restart the server? This will briefly interrupt services.')) {
        return;
    }

    try {
        const response = await fetch(API_BASE + '/management/v1/restart', {
            method: 'POST'
        });

        let result = null;
        try {
            result = await response.json();
        } catch (e) {
            result = null;
        }

        if (!result || result.ErrorNumber === 0) {
            alert('Server restart initiated. The server will restart shortly.');
            setTimeout(() => {
                window.location.reload();
            }, 3000);
        } else {
            alert('Error restarting server: ' + result.ErrorMessage);
        }
    } catch (error) {
        alert('Restart request sent. If the server does not restart, check the server logs. Error: ' + error.message);
    }
}

// Log level management
async function loadLogSettings() {
    const statusEl = document.getElementById('log-level-status');
    if (!statusEl) {
        return;
    }

    setLogControlsDisabled(true);
    statusEl.textContent = 'Loading log settings...';

    try {
        const response = await fetch(API_BASE + LOGGING_ENDPOINT);
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const text = await response.text();
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            throw new Error('Invalid JSON response from server');
        }

        if (data.ErrorNumber !== 0) {
            throw new Error(data.ErrorMessage || 'Unknown server error');
        }

        const payload = parseResponseValue(data.Value) || {};
        const supportedLevels = payload.SupportedLevels || payload.supportedLevels || null;
        const level = syncLogControls(payload.Level || payload.level || QUIET_LOG_LEVEL, supportedLevels);
        statusEl.textContent = `Current log level: ${level}`;
    } catch (error) {
        statusEl.textContent = `Unable to load log settings: ${error.message}`;
    } finally {
        setLogControlsDisabled(false);
    }
}

async function handleLogLevelToggleChange(event) {
    const selectedLevel = normalizeLogLevel(event.target.dataset.level);
    const selectedIndex = getLogLevelIndex(selectedLevel);
    if (selectedIndex === -1) {
        return;
    }

    let minLevel = selectedLevel;
    if (!event.target.checked) {
        const toggles = getLogLevelToggles();
        const higherLevel = LOG_LEVEL_ORDER
            .slice(selectedIndex + 1)
            .find(level => toggles.some(toggle => normalizeLogLevel(toggle.dataset.level) === level && toggle.checked));
        minLevel = higherLevel || 'CRITICAL';
    }

    applyLogLevelSelection(minLevel);
    await requestLogLevelUpdate(minLevel);
}

async function downloadAllLogs() {
    setLogFilesStatus('Preparing log archive…');

    try {
        const response = await fetch(API_BASE + LOG_FILES_ENDPOINT + '?download=1');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const blob = await response.blob();
        const date = new Date().toISOString().slice(0, 10);
        const filename = `alpacabridge-logs-${date}.txt.gz`;
        const url = URL.createObjectURL(blob);
        const anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = filename;
        document.body.appendChild(anchor);
        anchor.click();
        anchor.remove();
        URL.revokeObjectURL(url);
        setLogFilesStatus(`Downloaded ${filename}`);
    } catch (error) {
        setLogFilesStatus(`Failed to download logs: ${error.message}`);
    }
}

// Log file management (on-disk daily files)
function formatLogFileSize(bytes) {
    if (!Number.isFinite(bytes) || bytes < 0) {
        return '–';
    }
    if (bytes < 1024) {
        return `${bytes} B`;
    }
    const units = ['KB', 'MB', 'GB', 'TB'];
    let value = bytes / 1024;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit++;
    }
    return `${value.toFixed(value >= 10 ? 0 : 1)} ${units[unit]}`;
}

function formatLogFileTimestamp(epochSeconds) {
    if (!Number.isFinite(epochSeconds) || epochSeconds <= 0) {
        return '–';
    }
    try {
        return new Date(epochSeconds * 1000).toLocaleString();
    } catch (err) {
        return '–';
    }
}

function setLogFilesStatus(text) {
    const statusEl = document.getElementById('log-files-status');
    if (statusEl) {
        statusEl.textContent = text;
    }
}

function setLogFileViewer(filename, contents) {
    const viewer = document.getElementById('log-file-viewer');
    const title = document.getElementById('log-file-viewer-title');
    const body = document.getElementById('log-file-viewer-body');
    if (!viewer || !body) return;
    if (title) {
        title.textContent = filename;
    }
    body.textContent = contents;
    viewer.classList.remove('hidden');
    body.scrollTop = body.scrollHeight;
}

function clearLogFileViewer() {
    const viewer = document.getElementById('log-file-viewer');
    const body = document.getElementById('log-file-viewer-body');
    const title = document.getElementById('log-file-viewer-title');
    if (body) body.textContent = '';
    if (title) title.textContent = '';
    if (viewer) viewer.classList.add('hidden');
}

async function loadLogFiles() {
    const listEl = document.getElementById('log-files-list');
    if (!listEl) return;
    setLogFilesStatus('Loading log files…');
    try {
        const response = await fetch(API_BASE + LOG_FILES_ENDPOINT);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const text = await response.text();
        const payload = JSON.parse(text);
        if (payload.ErrorNumber !== 0) {
            throw new Error(payload.ErrorMessage || `Server error ${payload.ErrorNumber}`);
        }
        const inner = parseResponseValue(payload.Value) || {};
        const directory = inner.Directory || '';
        const files = Array.isArray(inner.Files) ? inner.Files : [];
        renderLogFiles(directory, files);
    } catch (error) {
        listEl.innerHTML = '';
        setLogFilesStatus(`Failed to load log files: ${error.message}`);
    }
}

function renderLogFiles(directory, files) {
    const listEl = document.getElementById('log-files-list');
    const dirEl = document.getElementById('log-files-directory');
    if (dirEl) {
        dirEl.textContent = directory ? `Directory: ${directory}` : 'File logging is disabled.';
    }
    if (!listEl) return;
    listEl.innerHTML = '';
    if (!files.length) {
        setLogFilesStatus(directory
            ? 'No log files yet - they appear here once the server writes some.'
            : 'No log files (file logging is disabled).');
        return;
    }
    setLogFilesStatus(`${files.length} log file${files.length === 1 ? '' : 's'} on disk.`);

    files.forEach(file => {
        const row = document.createElement('div');
        row.className = 'log-file-row';

        const info = document.createElement('div');
        info.className = 'log-file-info';
        const nameEl = document.createElement('div');
        nameEl.className = 'log-file-name';
        nameEl.textContent = file.Name;
        const metaEl = document.createElement('div');
        metaEl.className = 'log-file-meta';
        metaEl.textContent = `${formatLogFileSize(file.Size)} • ${formatLogFileTimestamp(file.Modified)}`;
        info.appendChild(nameEl);
        info.appendChild(metaEl);

        const actions = document.createElement('div');
        actions.className = 'log-file-actions';

        const viewBtn = document.createElement('button');
        viewBtn.type = 'button';
        viewBtn.className = 'btn btn-secondary';
        viewBtn.textContent = 'View';
        viewBtn.addEventListener('click', () => viewLogFile(file.Name, file.Size));

        const downloadBtn = document.createElement('button');
        downloadBtn.type = 'button';
        downloadBtn.className = 'btn btn-secondary';
        downloadBtn.textContent = 'Download';
        downloadBtn.addEventListener('click', () => downloadLogFile(file.Name));

        const deleteBtn = document.createElement('button');
        deleteBtn.type = 'button';
        deleteBtn.className = 'btn btn-danger';
        deleteBtn.textContent = 'Delete';
        deleteBtn.addEventListener('click', () => deleteLogFile(file.Name));

        actions.appendChild(viewBtn);
        actions.appendChild(downloadBtn);
        actions.appendChild(deleteBtn);

        row.appendChild(info);
        row.appendChild(actions);
        listEl.appendChild(row);
    });
}

const INLINE_VIEW_MAX_BYTES = 5 * 1024 * 1024;  // 5 MiB

async function viewLogFile(filename, sizeBytes = null) {
    if (Number.isFinite(sizeBytes) && sizeBytes > INLINE_VIEW_MAX_BYTES) {
        clearLogFileViewer();
        setLogFilesStatus(
            `${filename} is ${formatLogFileSize(sizeBytes)} - too large to view inline. Use Download instead.`
        );
        return;
    }
    setLogFilesStatus(`Loading ${filename}…`);
    try {
        const response = await fetch(`${API_BASE}${LOG_FILES_ENDPOINT}/${encodeURIComponent(filename)}`);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const contents = await response.text();
        setLogFileViewer(filename, contents || '(file is empty)');
        setLogFilesStatus(`Viewing ${filename}`);
    } catch (error) {
        setLogFilesStatus(`Failed to load ${filename}: ${error.message}`);
    }
}

async function downloadLogFile(filename) {
    setLogFilesStatus(`Downloading ${filename}…`);
    try {
        const response = await fetch(
            `${API_BASE}${LOG_FILES_ENDPOINT}/${encodeURIComponent(filename)}?download=1`);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const blob = await response.blob();
        const url = URL.createObjectURL(blob);
        const anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = filename;
        document.body.appendChild(anchor);
        anchor.click();
        anchor.remove();
        URL.revokeObjectURL(url);
        setLogFilesStatus(`Downloaded ${filename}`);
    } catch (error) {
        setLogFilesStatus(`Failed to download ${filename}: ${error.message}`);
    }
}

async function deleteAllLogFiles() {
    if (!confirm('Delete ALL stored log files? This cannot be undone.')) {
        return;
    }
    setLogFilesStatus('Deleting all log files…');
    try {
        const response = await fetch(API_BASE + LOG_FILES_ENDPOINT, { method: 'DELETE' });
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const payload = JSON.parse(await response.text());
        if (payload.ErrorNumber !== 0) {
            throw new Error(payload.ErrorMessage || `Server error ${payload.ErrorNumber}`);
        }
        const value = parseResponseValue(payload.Value) || {};
        clearLogFileViewer();
        await loadLogFiles();
        const count = Number.isFinite(value.DeletedCount) ? value.DeletedCount : 0;
        setLogFilesStatus(`Deleted ${count} log file${count === 1 ? '' : 's'}.`);
    } catch (error) {
        setLogFilesStatus(`Failed to delete log files: ${error.message}`);
    }
}

async function deleteLogFile(filename) {
    if (!confirm(`Delete log file ${filename}? This cannot be undone.`)) {
        return;
    }
    setLogFilesStatus(`Deleting ${filename}…`);
    try {
        const response = await fetch(
            `${API_BASE}${LOG_FILES_ENDPOINT}/${encodeURIComponent(filename)}`,
            { method: 'DELETE' });
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const viewerTitle = document.getElementById('log-file-viewer-title');
        if (viewerTitle && viewerTitle.textContent === filename) {
            clearLogFileViewer();
        }
        await loadLogFiles();
    } catch (error) {
        setLogFilesStatus(`Failed to delete ${filename}: ${error.message}`);
    }
}

// Device form handling
function updateVendorOptions() {
    const deviceTypeSelect = document.getElementById('device-type');
    const vendorSelect = document.getElementById('vendor');
    if (!deviceTypeSelect || !vendorSelect) {
        return;
    }

    const deviceType = normalizeDeviceType(deviceTypeSelect.value);
    const isTelescope = deviceType === 'telescope';
    const isCamera = deviceType === 'camera';
    const isSwitch = deviceType === 'switch';
    const isFilterWheel = deviceType === 'filterwheel';
    const isFocuser = deviceType === 'focuser';
    const isRotator = deviceType === 'rotator';
    const isObservingConditions = deviceType === 'observingconditions';
    const isCoverCalibrator = deviceType === 'covercalibrator';
    const astroasisOption = vendorSelect.querySelector('option[value="astroasis"]');
    if (astroasisOption) {
        // Astroasis provides only the Oasis Focuser.
        astroasisOption.disabled = !isFocuser;
        astroasisOption.hidden = !isFocuser;
    }
    const ioptronOption = vendorSelect.querySelector('option[value="ioptron"]');
    if (ioptronOption) {
        // iOptron provides the mount (telescope), the iMate PowerBox (switch),
        // the iEAF / iAFS2/3 focusers and the iEFW filter wheel.
        const ioptronAllowed = isTelescope || isSwitch || isFocuser || isFilterWheel;
        ioptronOption.disabled = !ioptronAllowed;
        ioptronOption.hidden = !ioptronAllowed;
    }
    const synscanOption = vendorSelect.querySelector('option[value="synscan"]');
    if (synscanOption) {
        synscanOption.disabled = !isTelescope;
        synscanOption.hidden = !isTelescope;
    }
    const skywatcherOption = vendorSelect.querySelector('option[value="skywatcher"]');
    if (skywatcherOption) {
        skywatcherOption.disabled = !isTelescope;
        skywatcherOption.hidden = !isTelescope;
    }
    const celestronOption = vendorSelect.querySelector('option[value="celestron"]');
    if (celestronOption) {
        celestronOption.disabled = !isTelescope;
        celestronOption.hidden = !isTelescope;
    }
    const onstepOption = vendorSelect.querySelector('option[value="onstep"]');
    if (onstepOption) {
        onstepOption.disabled = !isTelescope;
        onstepOption.hidden = !isTelescope;
    }
    const bisqueOption = vendorSelect.querySelector('option[value="bisque"]');
    if (bisqueOption) {
        bisqueOption.disabled = !isTelescope;
        bisqueOption.hidden = !isTelescope;
    }
    const zwoOption = vendorSelect.querySelector('option[value="zwo"]');
    if (zwoOption) {
        const zwoAllowed = isTelescope || isCamera || isSwitch || isFilterWheel || isFocuser || isRotator;
        zwoOption.disabled = !zwoAllowed;
        zwoOption.hidden = !zwoAllowed;
    }
    const qhyOption = vendorSelect.querySelector('option[value="qhy"]');
    if (qhyOption) {
        // QHY provides cameras and, on models like the miniCam8M, an
        // integrated CFW (filter wheel) sharing the camera's SDK handle.
        const qhyAllowed = isCamera || isFilterWheel;
        qhyOption.disabled = !qhyAllowed;
        qhyOption.hidden = !qhyAllowed;
    }
    const svbonyOption = vendorSelect.querySelector('option[value="svbony"]');
    if (svbonyOption) {
        svbonyOption.disabled = !isCamera;
        svbonyOption.hidden = !isCamera;
    }
    const touptekOption = vendorSelect.querySelector('option[value="touptek"]');
    if (touptekOption) {
        // ToupTek provides cameras, the AAF focuser, the AFW filter wheel, and
        // the StellaVita PowerBox (switch).
        const allowTouptek = isCamera || isFocuser || isFilterWheel || isSwitch;
        touptekOption.disabled = !allowTouptek;
        touptekOption.hidden = !allowTouptek;
    }
    const playerOneOption = vendorSelect.querySelector('option[value="playerone"]');
    if (playerOneOption) {
        // Player One provides cameras, the Phoenix filter wheel, and the
        // thermal switch (cooled-camera dew heater / fan).
        const playerOneAllowed = isCamera || isFilterWheel || isSwitch;
        playerOneOption.disabled = !playerOneAllowed;
        playerOneOption.hidden = !playerOneAllowed;
    }
    const weewxOption = vendorSelect.querySelector('option[value="weewx"]');
    if (weewxOption) {
        weewxOption.disabled = !isObservingConditions;
        weewxOption.hidden = !isObservingConditions;
    }
    const geminiOption = vendorSelect.querySelector('option[value="gemini"]');
    if (geminiOption) {
        // Gemini provides the Automatic Astro Focuser Pro (focuser) and the
        // Flat Panel Cover Lite (CoverCalibrator, light-only).
        const geminiAllowed = isFocuser || isCoverCalibrator;
        geminiOption.disabled = !geminiAllowed;
        geminiOption.hidden = !geminiAllowed;
    }
    const wandererastroOption = vendorSelect.querySelector('option[value="wandererastro"]');
    if (wandererastroOption) {
        // WandererAstro provides the WandererCover V4 (CoverCalibrator), the
        // WandererRotator Mini (Rotator), the SFW filter wheels (FilterWheel)
        // and the WandererBox Pro V3 power box (Switch).
        const wandererastroAllowed = isCoverCalibrator || isRotator || isFilterWheel || isSwitch;
        wandererastroOption.disabled = !wandererastroAllowed;
        wandererastroOption.hidden = !wandererastroAllowed;
    }

    if (!isFocuser && vendorSelect.value === 'astroasis') {
        vendorSelect.value = '';
    }
    if (!isTelescope && !isSwitch && !isFocuser && !isFilterWheel && vendorSelect.value === 'ioptron') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'synscan') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'skywatcher') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'celestron') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'onstep') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'bisque') {
        vendorSelect.value = '';
    }
    if (!isTelescope && !isCamera && !isSwitch && !isFilterWheel && !isFocuser && !isRotator &&
        vendorSelect.value === 'zwo') {
        vendorSelect.value = '';
    }
    if (!isCamera && !isFilterWheel && vendorSelect.value === 'qhy') {
        vendorSelect.value = '';
    }
    if (!isCamera && vendorSelect.value === 'svbony') {
        vendorSelect.value = '';
    }
    if (!isCamera && !isFocuser && !isFilterWheel && !isSwitch && vendorSelect.value === 'touptek') {
        vendorSelect.value = '';
    }
    if (!isCamera && !isFilterWheel && !isSwitch && vendorSelect.value === 'playerone') {
        vendorSelect.value = '';
    }
    if (!isObservingConditions && vendorSelect.value === 'weewx') {
        vendorSelect.value = '';
    }
    if (!isFocuser && !isCoverCalibrator && vendorSelect.value === 'gemini') {
        vendorSelect.value = '';
    }
    if (!isCoverCalibrator && !isRotator && !isFilterWheel && !isSwitch && vendorSelect.value === 'wandererastro') {
        vendorSelect.value = '';
    }

    vendorSelect.dispatchEvent(new Event('change'));
}

document.getElementById('device-type').addEventListener('change', updateVendorOptions);

document.getElementById('vendor').addEventListener('change', function() {
    const vendor = this.value;
    const configs = document.querySelectorAll('.vendor-config');
    configs.forEach(config => config.style.display = 'none');
    
    if (vendor === 'astroasis') {
        document.getElementById('astroasis-config').style.display = 'block';
    } else if (vendor === 'ioptron') {
        document.getElementById('ioptron-config').style.display = 'block';
        updateIoptronConfigFields();
    } else if (vendor === 'synscan') {
        document.getElementById('synscan-config').style.display = 'block';
    } else if (vendor === 'skywatcher') {
        document.getElementById('skywatcher-config').style.display = 'block';
    } else if (vendor === 'celestron') {
        document.getElementById('celestron-config').style.display = 'block';
    } else if (vendor === 'onstep') {
        document.getElementById('onstep-config').style.display = 'block';
    } else if (vendor === 'bisque') {
        document.getElementById('bisque-config').style.display = 'block';
    } else if (vendor === 'zwo') {
        document.getElementById('zwo-config').style.display = 'block';
    } else if (vendor === 'qhy') {
        document.getElementById('qhy-config').style.display = 'block';
    } else if (vendor === 'svbony') {
        document.getElementById('svbony-config').style.display = 'block';
    } else if (vendor === 'touptek') {
        document.getElementById('touptek-config').style.display = 'block';
    } else if (vendor === 'playerone') {
        document.getElementById('playerone-config').style.display = 'block';
    } else if (vendor === 'weewx') {
        document.getElementById('weewx-config').style.display = 'block';
    } else if (vendor === 'gemini') {
        document.getElementById('gemini-config').style.display = 'block';
        updateGeminiConfigFields();
    } else if (vendor === 'wandererastro') {
        document.getElementById('wandererastro-config').style.display = 'block';
        updateWandererastroConfigFields();
    }

    updateZwoConfigFields();
    updateTouptekConfigFields();
    updatePlayerOneConfigFields();
    updateQhyConfigFields();
    updateAutoNumbering();
});

// Gemini covers two device types from one vendor config block: the
// Automatic Astro Focuser Pro (focuser) and the Flat Panel Cover Lite
// (CoverCalibrator). Show the relevant sub-section based on Device Type
// (same pattern as updateIoptronConfigFields).
function updateGeminiConfigFields() {
    const focuserSection = document.getElementById('gemini-focuser-fields');
    const flatPanelSection = document.getElementById('gemini-flatpanel-fields');
    if (!focuserSection || !flatPanelSection) {
        return;
    }
    const deviceTypeSelect = document.getElementById('device-type');
    const deviceType = deviceTypeSelect ? normalizeDeviceType(deviceTypeSelect.value) : '';
    const isCoverCalibrator = deviceType === 'covercalibrator';
    focuserSection.style.display = isCoverCalibrator ? 'none' : 'block';
    flatPanelSection.style.display = isCoverCalibrator ? 'block' : 'none';
    // Disable the hidden section's inputs too (not just display:none) so
    // stale portPath/baudRate/connectionType values can't leak into the
    // other device type's config if form serialization ever changes
    // (matches the QHY camera/filterwheel split from PR #142).
    setFieldGroupEnabled(focuserSection, !isCoverCalibrator);
    setFieldGroupEnabled(flatPanelSection, isCoverCalibrator);
    if (isCoverCalibrator) {
        updateGeminiFlatPanelModelFields();
    }
}

// The CoverCalibrator sub-section itself covers two hardware models (Cover
// Lite, light-only vs. Automatic FlatPanel v2, motorized cover) sharing one
// vendor+deviceType slot. Show the relevant model's fields and disable the
// other's inputs (same stale-value-leak rationale as updateGeminiConfigFields).
function updateGeminiFlatPanelModelFields() {
    const modelSelect = document.getElementById('gemini-flatpanel-model');
    const liteSection = document.getElementById('gemini-flatpanel-lite-fields');
    const v2Section = document.getElementById('gemini-flatpanel-v2-fields');
    const proSection = document.getElementById('gemini-flatpanel-pro-fields');
    if (!modelSelect || !liteSection || !v2Section || !proSection) {
        return;
    }
    const isV2 = modelSelect.value === 'v2';
    const isPro = modelSelect.value === 'pro';
    const isLite = !isV2 && !isPro;
    liteSection.style.display = isLite ? 'block' : 'none';
    v2Section.style.display = isV2 ? 'block' : 'none';
    proSection.style.display = isPro ? 'block' : 'none';
    setFieldGroupEnabled(liteSection, isLite);
    setFieldGroupEnabled(v2Section, isV2);
    setFieldGroupEnabled(proSection, isPro);
}

// WandererAstro covers three device types from one vendor config block: the
// WandererCover V4 (covercalibrator), the WandererRotator Mini (rotator) and
// the SFW filter wheels (filterwheel). Show the relevant sub-section based on
// the selected device type.
function updateWandererastroConfigFields() {
    const coverSection = document.getElementById('wandererastro-cover-fields');
    const rotatorSection = document.getElementById('wandererastro-rotator-fields');
    const filterwheelSection = document.getElementById('wandererastro-filterwheel-fields');
    const boxSection = document.getElementById('wandererastro-box-fields');
    if (!coverSection || !rotatorSection || !filterwheelSection || !boxSection) {
        return;
    }
    const deviceTypeSelect = document.getElementById('device-type');
    const deviceType = deviceTypeSelect ? normalizeDeviceType(deviceTypeSelect.value) : '';
    const isRotator = deviceType === 'rotator';
    const isFilterWheel = deviceType === 'filterwheel';
    const isBox = deviceType === 'switch';
    const isCover = !isRotator && !isFilterWheel && !isBox;
    coverSection.style.display = isCover ? 'block' : 'none';
    rotatorSection.style.display = isRotator ? 'block' : 'none';
    filterwheelSection.style.display = isFilterWheel ? 'block' : 'none';
    boxSection.style.display = isBox ? 'block' : 'none';
    // Disable the hidden sections' inputs too (not just display:none) so
    // stale portPath/baudRate/connectionType values can't leak into the
    // other device type's config (matches the Gemini/QHY split pattern).
    setFieldGroupEnabled(coverSection, isCover);
    setFieldGroupEnabled(rotatorSection, isRotator);
    setFieldGroupEnabled(filterwheelSection, isFilterWheel);
    setFieldGroupEnabled(boxSection, isBox);
}

// iOptron covers three device types from one vendor config block: the mount
// (telescope), the iMate PowerBox (switch) and the iEAF focuser. Show the
// relevant sub-section based on the selected device type.
function updateIoptronConfigFields() {
    const telescopeSection = document.getElementById('ioptron-telescope-config');
    const switchSection = document.getElementById('ioptron-switch-config');
    const focuserSection = document.getElementById('ioptron-focuser-config');
    const filterwheelSection = document.getElementById('ioptron-filterwheel-config');
    if (!telescopeSection || !switchSection || !focuserSection || !filterwheelSection) {
        return;
    }
    const deviceTypeSelect = document.getElementById('device-type');
    const deviceType = deviceTypeSelect ? normalizeDeviceType(deviceTypeSelect.value) : '';
    const isSwitch = deviceType === 'switch';
    const isFocuser = deviceType === 'focuser';
    const isFilterWheel = deviceType === 'filterwheel';
    telescopeSection.style.display = (isSwitch || isFocuser || isFilterWheel) ? 'none' : 'block';
    switchSection.style.display = isSwitch ? 'block' : 'none';
    focuserSection.style.display = isFocuser ? 'block' : 'none';
    filterwheelSection.style.display = isFilterWheel ? 'block' : 'none';
}

const ioptronConnectionType = document.getElementById('ioptron-connection-type');
if (ioptronConnectionType) {
    ioptronConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('ioptron-serial-config').style.display = type === 'serial' ? 'block' : 'none';
        document.getElementById('ioptron-network-config').style.display = type === 'network' ? 'block' : 'none';
    });
}

// Picking the iEFW model pre-selects the matching slot count so the filter
// name pickers appear without a second click (the wheel still reports its own
// slot count at connect).
const ioptronFilterwheelModel = document.getElementById('ioptron-filterwheel-model');
if (ioptronFilterwheelModel) {
    ioptronFilterwheelModel.addEventListener('change', function() {
        const slotSelect = document.getElementById('ioptron-filterwheel-slot-count');
        if (!slotSelect) {
            return;
        }
        slotSelect.value = this.value === 'iefw18' ? '8' : '5';
        slotSelect.dispatchEvent(new Event('change'));
    });
}

const ioptronFilterwheelConnectionType = document.getElementById('ioptron-filterwheel-connection-type');
if (ioptronFilterwheelConnectionType) {
    ioptronFilterwheelConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('ioptron-filterwheel-auto-config').style.display = type === 'serial' ? 'none' : 'block';
        document.getElementById('ioptron-filterwheel-serial-config').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const ioptronIeafConnectionType = document.getElementById('ioptron-ieaf-connection-type');
if (ioptronIeafConnectionType) {
    ioptronIeafConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('ioptron-ieaf-auto-config').style.display = type === 'serial' ? 'none' : 'block';
        document.getElementById('ioptron-ieaf-serial-config').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const zwoSwitchTypeSelect = document.getElementById('zwo-switch-type');
if (zwoSwitchTypeSelect) {
    zwoSwitchTypeSelect.addEventListener('change', updateZwoConfigFields);
}

const touptekSwitchTypeSelect = document.getElementById('touptek-switch-type');
if (touptekSwitchTypeSelect) {
    touptekSwitchTypeSelect.addEventListener('change', updateTouptekConfigFields);
}

const skywatcherConnectionType = document.getElementById('skywatcher-connection-type');
if (skywatcherConnectionType) {
    skywatcherConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('skywatcher-serial-config').style.display = type === 'serial' ? 'block' : 'none';
        document.getElementById('skywatcher-network-config').style.display = type === 'network' ? 'block' : 'none';
    });
}

const synscanConnectionType = document.getElementById('synscan-connection-type');
if (synscanConnectionType) {
    synscanConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('synscan-serial-config').style.display = type === 'serial' ? 'block' : 'none';
        document.getElementById('synscan-network-config').style.display = type === 'network' ? 'block' : 'none';
    });
}

const onstepConnectionType = document.getElementById('onstep-connection-type');
if (onstepConnectionType) {
    onstepConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('onstep-serial-config').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const celestronConnectionType = document.getElementById('celestron-connection-type');
if (celestronConnectionType) {
    celestronConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('celestron-serial-config').style.display = type === 'serial' ? 'block' : 'none';
        document.getElementById('celestron-network-config').style.display = type === 'network' ? 'block' : 'none';
    });
}

const zwoMountConnectionType = document.getElementById('zwo-mount-connection-type');
if (zwoMountConnectionType) {
    zwoMountConnectionType.addEventListener('change', function() {
        const type = this.value;
        // Auto-detect needs no port/host; only show the config for an explicit transport.
        document.getElementById('zwo-mount-serial-config').style.display = type === 'serial' ? 'block' : 'none';
        document.getElementById('zwo-mount-network-config').style.display = type === 'network' ? 'block' : 'none';
    });
}

const geminiConnectionType = document.getElementById('gemini-connection-type');
if (geminiConnectionType) {
    geminiConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('gemini-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('gemini-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const geminiFlatPanelConnectionType = document.getElementById('gemini-flatpanel-connection-type');
if (geminiFlatPanelConnectionType) {
    geminiFlatPanelConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('gemini-flatpanel-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('gemini-flatpanel-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const geminiFlatPanelModel = document.getElementById('gemini-flatpanel-model');
if (geminiFlatPanelModel) {
    geminiFlatPanelModel.addEventListener('change', updateGeminiFlatPanelModelFields);
}

const geminiFlatPanelV2ConnectionType = document.getElementById('gemini-flatpanel-v2-connection-type');
if (geminiFlatPanelV2ConnectionType) {
    geminiFlatPanelV2ConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('gemini-flatpanel-v2-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('gemini-flatpanel-v2-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const geminiFlatPanelProConnectionType = document.getElementById('gemini-flatpanel-pro-connection-type');
if (geminiFlatPanelProConnectionType) {
    geminiFlatPanelProConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('gemini-flatpanel-pro-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('gemini-flatpanel-pro-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const wandererastroRotatorConnectionType = document.getElementById('wandererastro-rotator-connection-type');
if (wandererastroRotatorConnectionType) {
    wandererastroRotatorConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('wandererastro-rotator-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('wandererastro-rotator-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const wandererastroFilterwheelConnectionType = document.getElementById('wandererastro-filterwheel-connection-type');
if (wandererastroFilterwheelConnectionType) {
    wandererastroFilterwheelConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('wandererastro-filterwheel-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('wandererastro-filterwheel-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const wandererastroBoxConnectionType = document.getElementById('wandererastro-box-connection-type');
if (wandererastroBoxConnectionType) {
    wandererastroBoxConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('wandererastro-box-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('wandererastro-box-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const wandererastroConnectionType = document.getElementById('wandererastro-connection-type');
if (wandererastroConnectionType) {
    wandererastroConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('wandererastro-auto-fields').style.display = type === 'auto' ? 'block' : 'none';
        document.getElementById('wandererastro-serial-fields').style.display = type === 'serial' ? 'block' : 'none';
    });
}

const deviceNumberInput = document.getElementById('device-number');
if (deviceNumberInput) {
    deviceNumberInput.addEventListener('input', () => {
        deviceNumberInput.dataset.userModified = 'true';
    });
}

// Mark any vendor index field as user-modified once edited, so auto-numbering
// leaves it alone. Driven off INDEX_FIELDS so every vendor's index field
// (not just ZWO's) is covered.
INDEX_FIELDS.forEach(field => {
    const input = document.getElementById(field.fieldId);
    if (input) {
        input.addEventListener('input', () => {
            input.dataset.userModified = 'true';
        });
    }
});

// Slot-count + per-slot filter name pickers. Every filterwheel vendor config
// gets an instance (see AGENTS.md "FilterWheel web UI" note); the factory
// wires all events and keeps the slot rows and names textarea in sync.
const zwoFilterwheelSlotUI = createFilterwheelSlotUI({
    countSelectId: 'filterwheel-slot-count',
    customInputId: 'filterwheel-slot-custom',
    slotListId: 'filterwheel-slot-list',
    namesTextareaId: 'filterwheel-names'
});

const playerOneFilterwheelSlotUI = createFilterwheelSlotUI({
    countSelectId: 'playerone-filterwheel-slot-count',
    customInputId: 'playerone-filterwheel-slot-custom',
    slotListId: 'playerone-filterwheel-slot-list',
    namesTextareaId: 'playerone-filter-names'
});

const touptekFilterwheelSlotUI = createFilterwheelSlotUI({
    countSelectId: 'touptek-filterwheel-slot-count',
    customInputId: 'touptek-filterwheel-slot-custom',
    slotListId: 'touptek-filterwheel-slot-list',
    namesTextareaId: 'touptek-filter-names'
});

const ioptronFilterwheelSlotUI = createFilterwheelSlotUI({
    countSelectId: 'ioptron-filterwheel-slot-count',
    customInputId: 'ioptron-filterwheel-slot-custom',
    slotListId: 'ioptron-filterwheel-slot-list',
    namesTextareaId: 'ioptron-filter-names'
});

const wandererFilterwheelSlotUI = createFilterwheelSlotUI({
    countSelectId: 'wandererastro-filterwheel-slot-count',
    customInputId: 'wandererastro-filterwheel-slot-custom',
    slotListId: 'wandererastro-filterwheel-slot-list',
    namesTextareaId: 'wandererastro-filter-names'
});

const qhyFilterwheelSlotUI = createFilterwheelSlotUI({
    countSelectId: 'qhy-filterwheel-slot-count',
    customInputId: 'qhy-filterwheel-slot-custom',
    slotListId: 'qhy-filterwheel-slot-list',
    namesTextareaId: 'qhy-filter-names'
});

const apertureDiameterInput = document.getElementById('aperture-diameter');
if (apertureDiameterInput) {
    apertureDiameterInput.addEventListener('input', () =>
        updateApertureAreaFromDiameter('aperture-diameter', 'aperture-area'));
}
const skywatcherApertureDiameterInput = document.getElementById('skywatcher-aperture-diameter');
if (skywatcherApertureDiameterInput) {
    skywatcherApertureDiameterInput.addEventListener('input', () =>
        updateApertureAreaFromDiameter('skywatcher-aperture-diameter', 'skywatcher-aperture-area'));
}

const synscanApertureDiameterInput = document.getElementById('synscan-aperture-diameter');
if (synscanApertureDiameterInput) {
    synscanApertureDiameterInput.addEventListener('input', () =>
        updateApertureAreaFromDiameter('synscan-aperture-diameter', 'synscan-aperture-area'));
}
const onstepApertureDiameterInput = document.getElementById('onstep-aperture-diameter');
if (onstepApertureDiameterInput) {
    onstepApertureDiameterInput.addEventListener('input', () =>
        updateApertureAreaFromDiameter('onstep-aperture-diameter', 'onstep-aperture-area'));
}
const zwoMountApertureDiameterInput = document.getElementById('zwo-mount-aperture-diameter');
if (zwoMountApertureDiameterInput) {
    zwoMountApertureDiameterInput.addEventListener('input', () =>
        updateApertureAreaFromDiameter('zwo-mount-aperture-diameter', 'zwo-mount-aperture-area'));
}

function readOptionalNumber(formData, name) {
    const rawValue = formData.get(name);
    if (rawValue === null || rawValue === undefined) {
        return null;
    }
    const trimmed = rawValue.toString().trim();
    if (trimmed === '') {
        return null;
    }
    const value = Number.parseFloat(trimmed);
    return Number.isFinite(value) ? value : null;
}

// Telescope optics are entered and displayed in millimetres (what every scope
// is sold in), but the config file and the Alpaca ITelescope API both use
// metres per the ASCOM spec. The mm<->m conversion lives only here in the UI.
const MM_PER_METER = 1000;

function opticsMetersToMm(value) {
    const meters = Number(value);
    if (!Number.isFinite(meters) || meters <= 0) {
        return null;
    }
    return Number((meters * MM_PER_METER).toFixed(3));
}

// Reads the optional optics form fields (in mm) and stores them on deviceData
// in metres. Returns false, after alerting, when a value is small enough that
// the user almost certainly typed metres into the mm field.
function applyOpticsMm(formData, deviceData, apertureField, focalField) {
    // Per-field "typed metres into the mm field" thresholds, set below any
    // real optic but above any metres-typo: camera lenses on trackers go down
    // to ~8 mm focal length (~3 mm aperture at f/2.8), while metre values top
    // out around 3.9 (C14 focal length) and 0.5 (aperture). So <5 mm focal
    // length and <1 mm aperture can only be metres typed into the mm field.
    const fields = [
        [apertureField, 'apertureDiameter', 'Aperture diameter', 1],
        [focalField, 'focalLength', 'Focal length', 5],
    ];
    for (const [formName, configKey, label, minMm] of fields) {
        const mm = readOptionalNumber(formData, formName);
        // null (empty field) and an explicit 0 both mean "unset" - the router
        // only injects values > 0 into the drivers, so don't store a 0.
        if (mm === null || mm === 0) {
            continue;
        }
        if (mm < 0) {
            alert(`${label} cannot be negative.`);
            return false;
        }
        if (mm < minMm) {
            alert(`${label} of ${mm} mm looks too small - this field is in millimetres ` +
                `(e.g. a 480 mm focal length is entered as 480). Please re-enter the value in mm.`);
            return false;
        }
        deviceData[configKey] = mm / MM_PER_METER;
    }
    return true;
}

function normalizeFilterName(name) {
    return String(name || '').toLowerCase().replace(/[^a-z0-9]/g, '');
}

// expandShorthand: when true (the live UI preview), a single all-caps token is
// split into per-character names for display. On SUBMIT it must be false so the
// raw token is sent to the server, where the C++ expand_shorthand_locked expands
// it ONLY when its length matches the wheel's real slot count. Splitting here
// without knowing the slot count would send e.g. "LRGB" as 4 names to a 5-slot
// wheel, which gets silently padded (pre-connect) or rejected (post-connect).
function parseFilterNamesInput(rawValue, expandShorthand = true) {
    if (!rawValue) {
        return [];
    }
    let names = rawValue
        .toString()
        .split(/\r?\n/)
        .map(name => name.trim())
        .filter(name => name.length > 0);
    if (expandShorthand && names.length === 1) {
        // Expand a single delimiter-less token into per-slot single-character
        // names ("LRGB" -> L,R,G,B) only when it looks like a shorthand code:
        // no lowercase letters. This keeps ordinary names like "Clear" or
        // "Ha_NB" intact instead of exploding them into characters.
        const candidate = names[0];
        if (candidate.length > 1 && !/[,\s;]/.test(candidate) && !/[a-z]/.test(candidate)) {
            names = candidate.split('');
        }
    }
    return names;
}

function resolveFilterPreset(name) {
    if (!name) {
        return null;
    }
    return FILTERWHEEL_PRESET_LOOKUP.get(normalizeFilterName(name)) || null;
}

function resolveFilterShortCode(name) {
    if (!name) {
        return '';
    }
    const normalized = normalizeFilterName(name);
    return FILTERWHEEL_SHORT_CODES.get(normalized) || name.trim();
}

function formatFilterNamesShort(names) {
    if (!Array.isArray(names)) {
        return formatSettingValue(names);
    }
    const mapped = names.map(name => resolveFilterShortCode(name)).filter(Boolean);
    return mapped.join(', ');
}

function createFilterwheelSlotUI(ids) {
    const countSelect = document.getElementById(ids.countSelectId);
    const customInput = document.getElementById(ids.customInputId);
    const slotList = document.getElementById(ids.slotListId);
    const textarea = document.getElementById(ids.namesTextareaId);
    let syncInProgress = false;

    function getSlotCount() {
        if (!countSelect) {
            return null;
        }
        const selected = countSelect.value;
        if (!selected) {
            return null;
        }
        if (selected === 'custom') {
            if (!customInput) {
                return null;
            }
            const customValue = Number.parseInt(customInput.value, 10);
            return Number.isFinite(customValue) && customValue > 0 ? customValue : null;
        }
        const parsed = Number.parseInt(selected, 10);
        return Number.isFinite(parsed) ? parsed : null;
    }

    function updateCustomVisibility() {
        if (!countSelect || !customInput) {
            return;
        }
        const showCustom = countSelect.value === 'custom';
        customInput.style.display = showCustom ? 'block' : 'none';
        if (!showCustom) {
            customInput.value = '';
        }
    }

    function buildSlotRow(index, name) {
        const row = document.createElement('div');
        row.className = 'filterwheel-slot-row';

        const label = document.createElement('span');
        label.className = 'filterwheel-slot-label';
        label.textContent = `Slot ${index}`;

        const select = document.createElement('select');
        const placeholder = document.createElement('option');
        placeholder.value = '';
        placeholder.textContent = 'Select filter';
        select.appendChild(placeholder);

        FILTERWHEEL_PRESET_OPTIONS.forEach(option => {
            const opt = document.createElement('option');
            opt.value = option.value;
            opt.textContent = option.label;
            select.appendChild(opt);
        });

        const customOpt = document.createElement('option');
        customOpt.value = FILTERWHEEL_CUSTOM_VALUE;
        customOpt.textContent = 'Custom';
        select.appendChild(customOpt);

        const rowCustomInput = document.createElement('input');
        rowCustomInput.type = 'text';
        rowCustomInput.placeholder = 'Custom name';

        const resolvedPreset = resolveFilterPreset(name);
        if (resolvedPreset) {
            select.value = resolvedPreset;
        } else if (name) {
            select.value = FILTERWHEEL_CUSTOM_VALUE;
            rowCustomInput.value = name;
        } else {
            select.value = '';
        }

        const updateCustomState = () => {
            const isCustom = select.value === FILTERWHEEL_CUSTOM_VALUE;
            row.classList.toggle('custom-active', isCustom);
            rowCustomInput.disabled = !isCustom;
            if (!isCustom) {
                rowCustomInput.value = '';
            }
        };

        select.addEventListener('change', () => {
            updateCustomState();
            syncNamesFromSlots();
        });

        rowCustomInput.addEventListener('input', () => {
            syncNamesFromSlots();
        });

        updateCustomState();

        row.appendChild(label);
        row.appendChild(select);
        row.appendChild(rowCustomInput);
        return row;
    }

    function renderSlots(slotCount, names) {
        if (!slotList) {
            return;
        }
        slotList.innerHTML = '';
        if (!slotCount || slotCount <= 0) {
            return;
        }
        for (let i = 0; i < slotCount; i += 1) {
            const defaultName = `Filter ${i + 1}`;
            const name = names[i] || defaultName;
            slotList.appendChild(buildSlotRow(i + 1, name));
        }
    }

    function syncNamesFromSlots() {
        if (syncInProgress) {
            return;
        }
        if (!textarea || !slotList) {
            return;
        }
        const rows = Array.from(slotList.querySelectorAll('.filterwheel-slot-row'));
        if (rows.length === 0) {
            return;
        }
        syncInProgress = true;
        const names = rows.map((row, index) => {
            const select = row.querySelector('select');
            const rowCustomInput = row.querySelector('input[type="text"]');
            const selected = select ? select.value : '';
            if (selected === FILTERWHEEL_CUSTOM_VALUE) {
                const customName = rowCustomInput ? rowCustomInput.value.trim() : '';
                return customName || `Filter ${index + 1}`;
            }
            if (selected) {
                return selected;
            }
            return `Filter ${index + 1}`;
        });
        textarea.value = names.join('\n');
        syncInProgress = false;
    }

    function syncSlotsFromTextarea() {
        if (syncInProgress) {
            return;
        }
        if (!textarea || !countSelect) {
            return;
        }
        syncInProgress = true;
        const names = parseFilterNamesInput(textarea.value);
        if (names.length === 0) {
            countSelect.value = '';
            if (customInput) {
                customInput.value = '';
            }
            updateCustomVisibility();
            renderSlots(0, []);
            syncInProgress = false;
            return;
        }
        const countOption = countSelect.querySelector(`option[value="${names.length}"]`);
        if (countOption) {
            countSelect.value = String(names.length);
            if (customInput) {
                customInput.value = '';
            }
        } else {
            countSelect.value = 'custom';
            if (customInput) {
                customInput.value = String(names.length);
            }
        }
        updateCustomVisibility();
        renderSlots(names.length, names);
        syncInProgress = false;
    }

    if (countSelect) {
        updateCustomVisibility();
        countSelect.addEventListener('change', () => {
            updateCustomVisibility();
            const slotCount = getSlotCount();
            if (!slotCount) {
                renderSlots(0, []);
                return;
            }
            renderSlots(slotCount, []);
            syncNamesFromSlots();
        });
    }

    if (customInput) {
        customInput.addEventListener('input', () => {
            if (!countSelect || countSelect.value !== 'custom') {
                return;
            }
            const slotCount = getSlotCount();
            if (!slotCount) {
                renderSlots(0, []);
                return;
            }
            renderSlots(slotCount, []);
            syncNamesFromSlots();
        });
    }

    if (textarea) {
        textarea.addEventListener('input', () => {
            syncSlotsFromTextarea();
        });
    }

    return { syncSlotsFromTextarea };
}

function setFieldGroupEnabled(groupEl, enabled) {
    if (!groupEl) {
        return;
    }
    const fields = groupEl.querySelectorAll('input, select, textarea');
    fields.forEach(field => {
        field.disabled = !enabled;
    });
}

function updateTouptekConfigFields() {
    const deviceTypeSelect = document.getElementById('device-type');
    if (!deviceTypeSelect) {
        return;
    }
    const cameraFields = document.getElementById('touptek-camera-fields');
    const focuserFields = document.getElementById('touptek-focuser-fields');
    const filterwheelFields = document.getElementById('touptek-filterwheel-fields');
    const switchFields = document.getElementById('touptek-switch-fields');
    const deviceType = normalizeDeviceType(deviceTypeSelect.value);
    const isCamera = deviceType === 'camera';
    const isFocuser = deviceType === 'focuser';
    const isFilterWheel = deviceType === 'filterwheel';
    const isSwitch = deviceType === 'switch';
    if (cameraFields) {
        cameraFields.style.display = isCamera ? 'block' : 'none';
        setFieldGroupEnabled(cameraFields, isCamera);
    }
    if (focuserFields) {
        focuserFields.style.display = isFocuser ? 'block' : 'none';
        setFieldGroupEnabled(focuserFields, isFocuser);
    }
    if (filterwheelFields) {
        filterwheelFields.style.display = isFilterWheel ? 'block' : 'none';
        setFieldGroupEnabled(filterwheelFields, isFilterWheel);
    }
    if (switchFields) {
        switchFields.style.display = isSwitch ? 'block' : 'none';
        setFieldGroupEnabled(switchFields, isSwitch);
    }
    // ToupTek has two switch backends selected by switchType: the camera thermal
    // switch (dew heater + fan) and the StellaVita PowerBox (GPIO). Show only the
    // relevant sub-block so hidden fields don't submit.
    const switchTypeSelect = document.getElementById('touptek-switch-type');
    const thermalFields = document.getElementById('touptek-thermal-switch-fields');
    const stellavitaFields = document.getElementById('touptek-stellavita-switch-fields');
    const isThermal = isSwitch && switchTypeSelect && switchTypeSelect.value === 'thermal';
    const isStellavita = isSwitch && !isThermal;
    if (thermalFields) {
        thermalFields.style.display = isThermal ? 'block' : 'none';
        setFieldGroupEnabled(thermalFields, isThermal);
    }
    if (stellavitaFields) {
        stellavitaFields.style.display = isStellavita ? 'block' : 'none';
        setFieldGroupEnabled(stellavitaFields, isStellavita);
    }
}

function updateQhyConfigFields() {
    const deviceTypeSelect = document.getElementById('device-type');
    if (!deviceTypeSelect) {
        return;
    }
    const cameraFields = document.getElementById('qhy-camera-fields');
    const filterwheelFields = document.getElementById('qhy-filterwheel-fields');
    const deviceType = normalizeDeviceType(deviceTypeSelect.value);
    const isCamera = deviceType === 'camera';
    const isFilterWheel = deviceType === 'filterwheel';
    if (cameraFields) {
        cameraFields.style.display = isCamera ? 'block' : 'none';
        setFieldGroupEnabled(cameraFields, isCamera);
    }
    if (filterwheelFields) {
        filterwheelFields.style.display = isFilterWheel ? 'block' : 'none';
        setFieldGroupEnabled(filterwheelFields, isFilterWheel);
    }
}

function updatePlayerOneConfigFields() {
    const deviceTypeSelect = document.getElementById('device-type');
    if (!deviceTypeSelect) {
        return;
    }
    const cameraFields = document.getElementById('playerone-camera-fields');
    const filterwheelFields = document.getElementById('playerone-filterwheel-fields');
    const switchFields = document.getElementById('playerone-switch-fields');
    const deviceType = normalizeDeviceType(deviceTypeSelect.value);
    const isCamera = deviceType === 'camera';
    const isFilterWheel = deviceType === 'filterwheel';
    const isSwitch = deviceType === 'switch';
    if (cameraFields) {
        cameraFields.style.display = isCamera ? 'block' : 'none';
        setFieldGroupEnabled(cameraFields, isCamera);
    }
    if (filterwheelFields) {
        filterwheelFields.style.display = isFilterWheel ? 'block' : 'none';
        setFieldGroupEnabled(filterwheelFields, isFilterWheel);
    }
    if (switchFields) {
        switchFields.style.display = isSwitch ? 'block' : 'none';
        setFieldGroupEnabled(switchFields, isSwitch);
    }
}

function updateZwoConfigFields() {
    const deviceTypeSelect = document.getElementById('device-type');
    const telescopeFields = document.getElementById('zwo-telescope-fields');
    const switchTypeGroup = document.getElementById('zwo-switch-type-group');
    const cameraFields = document.getElementById('zwo-camera-fields');
    const filterwheelFields = document.getElementById('zwo-filterwheel-fields');
    const focuserFields = document.getElementById('zwo-focuser-fields');
    const rotatorFields = document.getElementById('zwo-rotator-fields');
    if (!deviceTypeSelect || !switchTypeGroup) {
        return;
    }
    const deviceType = normalizeDeviceType(deviceTypeSelect.value);
    const isTelescope = deviceType === 'telescope';
    const isCamera = deviceType === 'camera';
    const isSwitch = deviceType === 'switch';
    const isFilterWheel = deviceType === 'filterwheel';
    const isFocuser = deviceType === 'focuser';
    const isRotator = deviceType === 'rotator';

    if (telescopeFields) {
        telescopeFields.style.display = isTelescope ? 'block' : 'none';
        setFieldGroupEnabled(telescopeFields, isTelescope);
    }
    switchTypeGroup.style.display = isSwitch ? 'block' : 'none';

    const switchTypeSelect = document.getElementById('zwo-switch-type');
    const switchTypeValue = switchTypeSelect ? switchTypeSelect.value : 'dewheater';
    const isAsiairSwitch = isSwitch &&
        (switchTypeValue === 'asiair' || switchTypeValue === 'asiair-plus-picm4');
    const isAsiairPlusSwitch = isSwitch && switchTypeValue === 'asiair-plus-rk3568';
    const showCameraFields = isCamera || (isSwitch && !isAsiairSwitch && !isAsiairPlusSwitch);
    if (cameraFields) {
        cameraFields.style.display = showCameraFields ? 'block' : 'none';
        setFieldGroupEnabled(cameraFields, showCameraFields);
    }

    const asiairFields = document.getElementById('zwo-asiair-fields');
    if (asiairFields) {
        asiairFields.style.display = isAsiairSwitch ? 'block' : 'none';
        setFieldGroupEnabled(asiairFields, isAsiairSwitch);
    }

    const asiairPlusFields = document.getElementById('zwo-asiair-plus-fields');
    if (asiairPlusFields) {
        asiairPlusFields.style.display = isAsiairPlusSwitch ? 'block' : 'none';
        setFieldGroupEnabled(asiairPlusFields, isAsiairPlusSwitch);
    }
    if (filterwheelFields) {
        filterwheelFields.style.display = isFilterWheel ? 'block' : 'none';
        setFieldGroupEnabled(filterwheelFields, isFilterWheel);
    }
    if (focuserFields) {
        focuserFields.style.display = isFocuser ? 'block' : 'none';
        setFieldGroupEnabled(focuserFields, isFocuser);
    }
    if (rotatorFields) {
        rotatorFields.style.display = isRotator ? 'block' : 'none';
        setFieldGroupEnabled(rotatorFields, isRotator);
    }
}

document.getElementById('device-form').addEventListener('submit', async function(e) {
    e.preventDefault();
    
    const formData = new FormData(this);
    const deviceData = {
        deviceType: formData.get('deviceType'),
        deviceNumber: parseInt(formData.get('deviceNumber')),
        vendor: formData.get('vendor'),
        connectionType: formData.get('connectionType'),
    };

    if (deviceData.vendor === 'ioptron' && normalizeDeviceType(deviceData.deviceType) === 'switch') {
        // iMate PowerBox: local GPIO, no mount connection fields. Optional GPIO
        // chip override (defaults to /dev/gpiochip1 server-side) plus optional
        // PWM dimming on DC1/DC2.
        const gpioChip = (formData.get('ioptronPowerboxGpioChip') || '').trim();
        if (gpioChip) {
            deviceData.gpioChip = gpioChip;
        }
        const dc1Pwm = formData.get('ioptronPortPwm1') === 'on';
        const dc2Pwm = formData.get('ioptronPortPwm2') === 'on';
        // Always persist the PWM frequency so a custom value survives even if
        // the user temporarily un-ticks both ports; it applies the next time a
        // port is switched back to PWM.
        const pwmFreq = Number.parseInt(formData.get('ioptronPowerboxPwmFrequency'), 10);
        if (!Number.isNaN(pwmFreq)) {
            deviceData.pwmFrequencyHz = pwmFreq;
        }
        // Always emit the positional overlay on the fixed DC3/DC1/DC2 layout
        // (index 0 is the always-on DC3 pass-through, 1 = DC1, 2 = DC2) so any
        // per-port config is not dropped on a re-save with PWM un-ticked.
        deviceData.ports = [
            {},
            { pwm: dc1Pwm },
            { pwm: dc2Pwm },
        ];
    } else if (deviceData.vendor === 'ioptron' && normalizeDeviceType(deviceData.deviceType) === 'filterwheel') {
        // iEFW: USB-serial only, fixed baud — no baudRate/network fields.
        deviceData.connectionType = formData.get('ioptronFilterwheelConnectionType') || 'auto';
        deviceData.model = formData.get('ioptronFilterwheelModel') || 'iefw15';
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('ioptronFilterwheelPortPath') || '';
        } else {
            const wheelIndex = readOptionalNumber(formData, 'ioptronFilterwheelIndex');
            deviceData.filterwheelIndex = wheelIndex !== null ? wheelIndex : 0;
        }
        // Raw token (expandShorthand=false): the server-side C++ expansion is
        // slot-count-aware and handles "LRGBSHOC" itself.
        const ioptronFilterNames = parseFilterNamesInput(formData.get('ioptronFilterNames'), false);
        if (ioptronFilterNames.length > 0) {
            deviceData.filterNames = ioptronFilterNames;
        }
    } else if (deviceData.vendor === 'ioptron' && normalizeDeviceType(deviceData.deviceType) === 'focuser') {
        // iEAF: USB-serial only, fixed baud — no baudRate/network fields.
        deviceData.connectionType = formData.get('ioptronIeafConnectionType') || 'auto';
        deviceData.model = formData.get('ioptronIeafModel') || 'ieaf';
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('ioptronIeafPortPath');
        } else {
            deviceData.focuserIndex = parseInt(formData.get('ioptronIeafFocuserIndex')) || 0;
        }
    } else if (deviceData.vendor === 'ioptron') {
        deviceData.connectionType = formData.get('ioptronConnectionType') || 'auto';
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('ioptronPortPath');
            deviceData.baudRate = parseInt(formData.get('ioptronBaudRate')) || 115200;
        } else if (deviceData.connectionType === 'network') {
            const ioptronHost = (formData.get('ioptronHost') || '').trim();
            if (ioptronHost) {
                deviceData.host = ioptronHost;
                deviceData.tcpPort = parseInt(formData.get('ioptronTcpPort')) || 4030;
            }
        }
        // "auto" needs no connection fields - port is discovered at startup

        if (!applyOpticsMm(formData, deviceData, 'apertureDiameter', 'focalLength')) {
            return;
        }
    } else if (deviceData.vendor === 'synscan') {
        deviceData.connectionType = formData.get('synscanConnectionType') || 'auto';
        deviceData.synscanVersion = formData.get('synscanVersion') || 'auto';
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('synscanPortPath');
            deviceData.baudRate = parseInt(formData.get('synscanBaudRate')) || 9600;
        } else if (deviceData.connectionType === 'network') {
            deviceData.host = formData.get('synscanHost');
            deviceData.tcpPort = parseInt(formData.get('synscanTcpPort')) || 11880;
        }
        // "auto" needs no connection fields - port is discovered at startup

        if (!applyOpticsMm(formData, deviceData, 'synscanApertureDiameter', 'synscanFocalLength')) {
            return;
        }
    } else if (deviceData.vendor === 'skywatcher') {
        deviceData.connectionType = formData.get('skywatcherConnectionType') || 'auto';
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('skywatcherPortPath');
            deviceData.baudRate = parseInt(formData.get('skywatcherBaudRate')) || 9600;
        } else if (deviceData.connectionType === 'network') {
            deviceData.host = formData.get('skywatcherHost');
            deviceData.udpPort = parseInt(formData.get('skywatcherUdpPort')) || 11880;
        }
        // "auto" needs no connection fields - the mount is discovered at startup

        if (!applyOpticsMm(formData, deviceData, 'skywatcherApertureDiameter', 'skywatcherFocalLength')) {
            return;
        }

        const skywatcherSiteLatitude = readOptionalNumber(formData, 'skywatcherSiteLatitude');
        if (skywatcherSiteLatitude !== null) {
            deviceData.siteLatitude = skywatcherSiteLatitude;
        }
        const skywatcherSiteLongitude = readOptionalNumber(formData, 'skywatcherSiteLongitude');
        if (skywatcherSiteLongitude !== null) {
            deviceData.siteLongitude = skywatcherSiteLongitude;
        }
        const skywatcherSiteElevation = readOptionalNumber(formData, 'skywatcherSiteElevation');
        if (skywatcherSiteElevation !== null) {
            deviceData.siteElevation = skywatcherSiteElevation;
        }
    } else if (deviceData.vendor === 'onstep') {
        deviceData.connectionType = formData.get('onstepConnectionType') || 'auto';
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('onstepPortPath');
            deviceData.baudRate = parseInt(formData.get('onstepBaudRate')) || 9600;
        }
        // "auto" needs no connection fields - port is discovered at startup

        if (!applyOpticsMm(formData, deviceData, 'onstepApertureDiameter', 'onstepFocalLength')) {
            return;
        }
    } else if (deviceData.vendor === 'celestron') {
        deviceData.connectionType = formData.get('celestronConnectionType') || 'auto';
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('celestronPortPath');
            deviceData.baudRate = parseInt(formData.get('celestronBaudRate')) || 9600;
        } else if (deviceData.connectionType === 'network') {
            deviceData.host = formData.get('celestronHost');
            deviceData.tcpPort = parseInt(formData.get('celestronTcpPort')) || 2000;
        }
        // "auto" needs no connection fields - port is discovered at startup

        if (!applyOpticsMm(formData, deviceData, 'celestronApertureDiameter', 'celestronFocalLength')) {
            return;
        }
    } else if (deviceData.vendor === 'bisque') {
        deviceData.host = formData.get('bisqueHost') || 'localhost';
        deviceData.tcpPort = parseInt(formData.get('bisqueTcpPort')) || 3040;

        if (!applyOpticsMm(formData, deviceData, 'bisqueApertureDiameter', 'bisqueFocalLength')) {
            return;
        }
    } else if (deviceData.vendor === 'zwo') {
        const normalizedType = normalizeDeviceType(deviceData.deviceType);
        if (normalizedType === 'telescope') {
            deviceData.connectionType = formData.get('zwoMountConnectionType') || 'serial';
            if (deviceData.connectionType === 'serial') {
                deviceData.portPath = formData.get('zwoMountPortPath');
                deviceData.baudRate = parseInt(formData.get('zwoMountBaudRate')) || 9600;
            } else {
                deviceData.host = formData.get('zwoMountHost');
                deviceData.tcpPort = parseInt(formData.get('zwoMountTcpPort')) || 4030;
            }

            if (!applyOpticsMm(formData, deviceData, 'zwoMountApertureDiameter', 'zwoMountFocalLength')) {
                return;
            }

            const siteLatitude = readOptionalNumber(formData, 'zwoMountSiteLatitude');
            if (siteLatitude !== null) {
                deviceData.siteLatitude = siteLatitude;
            }

            const siteLongitude = readOptionalNumber(formData, 'zwoMountSiteLongitude');
            if (siteLongitude !== null) {
                deviceData.siteLongitude = siteLongitude;
            }

            const siteElevation = readOptionalNumber(formData, 'zwoMountSiteElevation');
            if (siteElevation !== null) {
                deviceData.siteElevation = siteElevation;
            }

            const zwoMountSyncTimeCheckbox = document.getElementById('zwo-mount-sync-time-on-connect');
            if (zwoMountSyncTimeCheckbox) {
                deviceData.syncTimeOnConnect = zwoMountSyncTimeCheckbox.checked;
            }

        }
        if (normalizedType === 'switch') {
            const switchType = formData.get('switchType');
            if (switchType) {
                deviceData.switchType = switchType;
            }
            if (deviceData.vendor === 'zwo' &&
                (switchType === 'asiair' || switchType === 'asiair-plus-picm4')) {
                const gpioChip = formData.get('asiairGpioChip');
                if (gpioChip) {
                    deviceData.gpioChip = gpioChip;
                }
                const pwmFreq = Number.parseInt(formData.get('asiairPwmFrequency'), 10);
                if (!Number.isNaN(pwmFreq)) {
                    deviceData.pwmFrequencyHz = pwmFreq;
                }
                const ports = [];
                for (let i = 0; i < 4; i += 1) {
                    const name = formData.get('asiairPortName' + i);
                    const gpio = Number.parseInt(formData.get('asiairPortGpio' + i), 10);
                    if (Number.isNaN(gpio)) {
                        continue;
                    }
                    ports.push({
                        name: name || ('Port ' + (i + 1)),
                        gpio: gpio,
                        pwm: formData.get('asiairPortPwm' + i) === 'on',
                    });
                }
                if (ports.length > 0) {
                    deviceData.ports = ports;
                }
            }
            if (deviceData.vendor === 'zwo' && switchType === 'asiair-plus-rk3568') {
                const devicePath = formData.get('asiairPlusDevicePath');
                if (devicePath) {
                    deviceData.devicePath = devicePath;
                }
                // pwmFrequencyHz is no longer collected from the form - the
                // driver auto-sets it to 50 Hz (matching what ZWO's stock
                // daemon actually uses) for soft-PWM. Any value already in
                // the persisted config still takes effect via the router,
                // but the UI doesn't surface it.
                const plusPorts = [];
                for (let i = 0; i < 4; i += 1) {
                    const name = formData.get('asiairPlusPortName' + i);
                    plusPorts.push({
                        name: name || ('Port ' + (i + 1)),
                        pwm: formData.get('asiairPlusPortPwm' + i) === 'on',
                    });
                }
                deviceData.ports = plusPorts;
            }
        }
        if (normalizedType === 'camera' || normalizedType === 'switch') {
            const cameraIndex = Number.parseInt(formData.get('cameraIndex'), 10);
            if (!Number.isNaN(cameraIndex)) {
                deviceData.cameraIndex = cameraIndex;
            }
            const cameraIdValue = formData.get('cameraId');
            if (cameraIdValue !== null && cameraIdValue !== undefined && cameraIdValue !== '') {
                const cameraId = Number.parseInt(cameraIdValue, 10);
                if (!Number.isNaN(cameraId)) {
                    deviceData.cameraId = cameraId;
                }
            }
        }
        if (normalizedType === 'filterwheel') {
            const filterwheelIndex = Number.parseInt(formData.get('filterwheelIndex'), 10);
            if (!Number.isNaN(filterwheelIndex)) {
                deviceData.filterwheelIndex = filterwheelIndex;
            }
            const filterwheelIdValue = formData.get('filterwheelId');
            if (filterwheelIdValue !== null && filterwheelIdValue !== undefined && filterwheelIdValue !== '') {
                const filterwheelId = Number.parseInt(filterwheelIdValue, 10);
                if (!Number.isNaN(filterwheelId)) {
                    deviceData.filterwheelId = filterwheelId;
                }
            }
            const filterNamesRaw = formData.get('filterNames');
            // Submit: don't expand shorthand client-side - send the raw token and
            // let the slot-count-aware C++ expansion handle it (see parseFilterNamesInput).
            const names = parseFilterNamesInput(filterNamesRaw, false);
            if (names.length > 0) {
                deviceData.filterNames = names;
            }
        }
        if (normalizedType === 'focuser') {
            const focuserIndex = Number.parseInt(formData.get('focuserIndex'), 10);
            if (!Number.isNaN(focuserIndex)) {
                deviceData.focuserIndex = focuserIndex;
            }
            const focuserIdValue = formData.get('focuserId');
            if (focuserIdValue !== null && focuserIdValue !== undefined && focuserIdValue !== '') {
                const focuserId = Number.parseInt(focuserIdValue, 10);
                if (!Number.isNaN(focuserId)) {
                    deviceData.focuserId = focuserId;
                }
            }
        }
        if (normalizedType === 'rotator') {
            const rotatorIndex = Number.parseInt(formData.get('rotatorIndex'), 10);
            if (!Number.isNaN(rotatorIndex)) {
                deviceData.rotatorIndex = rotatorIndex;
            }
            const rotatorIdValue = formData.get('rotatorId');
            if (rotatorIdValue !== null && rotatorIdValue !== undefined && rotatorIdValue !== '') {
                const rotatorId = Number.parseInt(rotatorIdValue, 10);
                if (!Number.isNaN(rotatorId)) {
                    deviceData.rotatorId = rotatorId;
                }
            }
        }
    } else if (deviceData.vendor === 'qhy' && normalizeDeviceType(deviceData.deviceType) === 'filterwheel') {
        // Integrated CFW (e.g. miniCam8M): unique field names so hidden
        // fields don't collide with the QHY camera device's own cameraId/
        // cameraIndex when both sections are present in the same form.
        const qhyCfwCameraId = formData.get('qhyCfwCameraId');
        if (qhyCfwCameraId && qhyCfwCameraId.trim() !== '') {
            deviceData.cameraId = qhyCfwCameraId.trim();
        } else {
            const qhyCfwCameraIndex = readOptionalNumber(formData, 'qhyCfwCameraIndex');
            deviceData.cameraIndex = qhyCfwCameraIndex !== null ? qhyCfwCameraIndex : 0;
        }
        const qhyFilterNames = parseFilterNamesInput(formData.get('qhyFilterNames'), false);
        if (qhyFilterNames.length > 0) {
            deviceData.filterNames = qhyFilterNames;
        }
    } else if (deviceData.vendor === 'qhy') {
        const qhyCameraId = formData.get('cameraId');
        if (qhyCameraId && qhyCameraId.trim() !== '') {
            deviceData.cameraId = qhyCameraId.trim();
        } else {
            const qhyCameraIndex = readOptionalNumber(formData, 'qhyCameraIndex');
            deviceData.cameraIndex = qhyCameraIndex !== null ? qhyCameraIndex : 0;
        }
    } else if (deviceData.vendor === 'svbony') {
        const svbonyCameraIndex = readOptionalNumber(formData, 'svbonyCameraIndex');
        deviceData.cameraIndex = svbonyCameraIndex !== null ? svbonyCameraIndex : 0;
    } else if (deviceData.vendor === 'touptek' && normalizeDeviceType(deviceData.deviceType) === 'switch') {
        // Unique field name (not "switchType") to avoid the FormData collision
        // with ZWO's switch-type select, which also submits while hidden.
        const touptekSwitchType = formData.get('touptekSwitchType') || 'stellavita';
        deviceData.switchType = touptekSwitchType;
        if (touptekSwitchType === 'thermal') {
            // Camera thermal switch: dew heater + fan, bound by camera index and
            // sharing the camera's SDK connection.
            const thermalCameraIndex = readOptionalNumber(formData, 'touptekThermalCameraIndex');
            deviceData.cameraIndex = thermalCameraIndex !== null ? thermalCameraIndex : 0;
        } else {
            // StellaVita PowerBox: local GPIO, no camera/focuser index. Optional
            // GPIO chip override (defaults to /dev/gpiochip0 server-side) plus
            // optional PWM dimming on any of the four ports.
            const gpioChip = (formData.get('touptekPowerboxGpioChip') || '').trim();
            if (gpioChip) {
                deviceData.gpioChip = gpioChip;
            }
            const portPwm = [0, 1, 2, 3].map(function(i) {
                return formData.get('touptekPortPwm' + i) === 'on';
            });
            // Always persist the PWM frequency so a custom value survives even if
            // the user temporarily un-ticks every port; it applies the next time a
            // port is switched back to PWM.
            const pwmFreq = Number.parseInt(formData.get('touptekPowerboxPwmFrequency'), 10);
            if (!Number.isNaN(pwmFreq)) {
                deviceData.pwmFrequencyHz = pwmFreq;
            }
            // Always emit the positional ports overlay (even all-false) so any
            // per-port config carried in the saved device (e.g. names) is not
            // dropped on a re-save with every PWM box un-ticked.
            deviceData.ports = portPwm.map(function(pwm) {
                return { pwm: pwm };
            });
        }
    } else if (deviceData.vendor === 'touptek') {
        if (normalizeDeviceType(deviceData.deviceType) === 'focuser') {
            // Unique field name (not the bare 'focuserId') to avoid the
            // FormData collision with ZWO's focuser-id input, which appears
            // first in the DOM and would otherwise win formData.get().
            const touptekFocuserId = formData.get('touptekFocuserId');
            if (touptekFocuserId && touptekFocuserId.trim() !== '') {
                deviceData.focuserId = touptekFocuserId.trim();
            } else {
                const touptekFocuserIndex = readOptionalNumber(formData, 'touptekFocuserIndex');
                deviceData.focuserIndex = touptekFocuserIndex !== null ? touptekFocuserIndex : 0;
            }
        } else if (normalizeDeviceType(deviceData.deviceType) === 'filterwheel') {
            // Bind by SDK id string when supplied (like focuserId), else by index.
            const touptekWheelId = formData.get('touptekFilterwheelId');
            if (touptekWheelId && touptekWheelId.trim() !== '') {
                deviceData.filterwheelId = touptekWheelId.trim();
            } else {
                const touptekWheelIndex = readOptionalNumber(formData, 'touptekFilterwheelIndex');
                deviceData.filterwheelIndex = touptekWheelIndex !== null ? touptekWheelIndex : 0;
            }
            const touptekFilterNames = parseFilterNamesInput(formData.get('touptekFilterNames'), false);
            if (touptekFilterNames.length > 0) {
                deviceData.filterNames = touptekFilterNames;
            }
        } else {
            const touptekCameraIndex = readOptionalNumber(formData, 'touptekCameraIndex');
            deviceData.cameraIndex = touptekCameraIndex !== null ? touptekCameraIndex : 0;
        }
    } else if (deviceData.vendor === 'playerone') {
        if (normalizeDeviceType(deviceData.deviceType) === 'filterwheel') {
            const playerOneWheelIndex = readOptionalNumber(formData, 'playerOneFilterwheelIndex');
            deviceData.filterwheelIndex = playerOneWheelIndex !== null ? playerOneWheelIndex : 0;
            const playerOneFilterNames = parseFilterNamesInput(formData.get('playerOneFilterNames'), false);
            if (playerOneFilterNames.length > 0) {
                deviceData.filterNames = playerOneFilterNames;
            }
        } else if (normalizeDeviceType(deviceData.deviceType) === 'switch') {
            const playerOneSwitchCameraIndex = readOptionalNumber(formData, 'playerOneSwitchCameraIndex');
            deviceData.cameraIndex = playerOneSwitchCameraIndex !== null ? playerOneSwitchCameraIndex : 0;
        } else {
            const playerOneCameraIndex = readOptionalNumber(formData, 'playerOneCameraIndex');
            deviceData.cameraIndex = playerOneCameraIndex !== null ? playerOneCameraIndex : 0;
        }
    } else if (deviceData.vendor === 'weewx') {
        deviceData.weewxUrl = formData.get('weewxUrl');
        const pollInterval = readOptionalNumber(formData, 'pollIntervalSeconds');
        if (pollInterval !== null) {
            deviceData.pollIntervalSeconds = pollInterval;
        }
        const timeoutMs = readOptionalNumber(formData, 'timeoutMs');
        if (timeoutMs !== null) {
            deviceData.timeoutMs = timeoutMs;
        }
    } else if (deviceData.vendor === 'gemini' && normalizeDeviceType(deviceData.deviceType) === 'covercalibrator') {
        const geminiFlatPanelModelEl = document.getElementById('gemini-flatpanel-model');
        const modelValue = geminiFlatPanelModelEl ? geminiFlatPanelModelEl.value : 'lite';
        const isV2 = modelValue === 'v2';
        const isPro = modelValue === 'pro';
        deviceData.flatPanelModel = isPro ? 'pro' : (isV2 ? 'v2' : 'lite');
        if (isPro) {
            const geminiFlatPanelProConnType = document.getElementById('gemini-flatpanel-pro-connection-type');
            deviceData.connectionType = geminiFlatPanelProConnType ? geminiFlatPanelProConnType.value : 'auto';
            if (deviceData.connectionType === 'auto') {
                const panelIndex = readOptionalNumber(formData, 'geminiFlatPanelProIndex');
                deviceData.panelIndex = panelIndex !== null ? panelIndex : 0;
            } else if (deviceData.connectionType === 'serial') {
                deviceData.portPath = formData.get('geminiFlatPanelProPortPath') || '';
                const baudRate = readOptionalNumber(formData, 'geminiFlatPanelProBaudRate');
                if (baudRate !== null) {
                    deviceData.baudRate = baudRate;
                }
            }
        } else if (isV2) {
            const geminiFlatPanelV2ConnType = document.getElementById('gemini-flatpanel-v2-connection-type');
            deviceData.connectionType = geminiFlatPanelV2ConnType ? geminiFlatPanelV2ConnType.value : 'auto';
            if (deviceData.connectionType === 'auto') {
                const panelIndex = readOptionalNumber(formData, 'geminiFlatPanelV2Index');
                deviceData.panelIndex = panelIndex !== null ? panelIndex : 0;
            } else if (deviceData.connectionType === 'serial') {
                deviceData.portPath = formData.get('geminiFlatPanelV2PortPath') || '';
                const baudRate = readOptionalNumber(formData, 'geminiFlatPanelV2BaudRate');
                if (baudRate !== null) {
                    deviceData.baudRate = baudRate;
                }
            }
        } else {
            const geminiFlatPanelConnType = document.getElementById('gemini-flatpanel-connection-type');
            deviceData.connectionType = geminiFlatPanelConnType ? geminiFlatPanelConnType.value : 'auto';
            if (deviceData.connectionType === 'auto') {
                const panelIndex = readOptionalNumber(formData, 'geminiFlatPanelIndex');
                deviceData.panelIndex = panelIndex !== null ? panelIndex : 0;
            } else if (deviceData.connectionType === 'serial') {
                deviceData.portPath = formData.get('geminiFlatPanelPortPath') || '';
                const baudRate = readOptionalNumber(formData, 'geminiFlatPanelBaudRate');
                if (baudRate !== null) {
                    deviceData.baudRate = baudRate;
                }
            }
        }
    } else if (deviceData.vendor === 'gemini') {
        const geminiConnType = document.getElementById('gemini-connection-type');
        deviceData.connectionType = geminiConnType ? geminiConnType.value : 'auto';
        if (deviceData.connectionType === 'auto') {
            const focuserIndex = readOptionalNumber(formData, 'geminiFocuserIndex');
            deviceData.focuserIndex = focuserIndex !== null ? focuserIndex : 0;
        } else if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('portPath') || '';
            const baudRate = readOptionalNumber(formData, 'baudRate');
            if (baudRate !== null) {
                deviceData.baudRate = baudRate;
            }
        }
    } else if (deviceData.vendor === 'wandererastro' && normalizeDeviceType(deviceData.deviceType) === 'filterwheel') {
        const wandererFwConnType = document.getElementById('wandererastro-filterwheel-connection-type');
        deviceData.connectionType = wandererFwConnType ? wandererFwConnType.value : 'auto';
        if (deviceData.connectionType === 'auto') {
            const wheelIndex = readOptionalNumber(formData, 'wandererastroFilterwheelIndex');
            deviceData.wandererFilterwheelIndex = wheelIndex !== null ? wheelIndex : 0;
        } else if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('wandererastroFilterwheelPortPath') || '';
            const baudRate = readOptionalNumber(formData, 'wandererastroFilterwheelBaudRate');
            if (baudRate !== null) {
                deviceData.baudRate = baudRate;
            }
        }
        // Raw token (expandShorthand=false): the server-side C++ expansion is
        // slot-count-aware and handles "LRGBSHOC" itself.
        const wandererFilterNames = parseFilterNamesInput(formData.get('wandererastroFilterNames'), false);
        if (wandererFilterNames.length > 0) {
            deviceData.filterNames = wandererFilterNames;
        }
    } else if (deviceData.vendor === 'wandererastro' && normalizeDeviceType(deviceData.deviceType) === 'rotator') {
        const wandererRotatorConnType = document.getElementById('wandererastro-rotator-connection-type');
        deviceData.connectionType = wandererRotatorConnType ? wandererRotatorConnType.value : 'auto';
        if (deviceData.connectionType === 'auto') {
            const rotatorIndex = readOptionalNumber(formData, 'wandererastroRotatorIndex');
            deviceData.rotatorIndex = rotatorIndex !== null ? rotatorIndex : 0;
        } else if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('wandererastroRotatorPortPath') || '';
            const baudRate = readOptionalNumber(formData, 'wandererastroRotatorBaudRate');
            if (baudRate !== null) {
                deviceData.baudRate = baudRate;
            }
        }
    } else if (deviceData.vendor === 'wandererastro' && normalizeDeviceType(deviceData.deviceType) === 'switch') {
        deviceData.switchType = 'wandererbox-pro-v3';
        const wandererBoxConnType = document.getElementById('wandererastro-box-connection-type');
        deviceData.connectionType = wandererBoxConnType ? wandererBoxConnType.value : 'auto';
        if (deviceData.connectionType === 'auto') {
            const boxIndex = readOptionalNumber(formData, 'wandererastroBoxIndex');
            deviceData.boxIndex = boxIndex !== null ? boxIndex : 0;
        } else if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('wandererastroBoxPortPath') || '';
            const baudRate = readOptionalNumber(formData, 'wandererastroBoxBaudRate');
            if (baudRate !== null) {
                deviceData.baudRate = baudRate;
            }
        }
    } else if (deviceData.vendor === 'wandererastro') {
        const wandererConnType = document.getElementById('wandererastro-connection-type');
        deviceData.connectionType = wandererConnType ? wandererConnType.value : 'auto';
        if (deviceData.connectionType === 'auto') {
            const coverIndex = readOptionalNumber(formData, 'wandererastroCoverIndex');
            deviceData.coverIndex = coverIndex !== null ? coverIndex : 0;
        } else if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('wandererastroPortPath') || '';
            const baudRate = readOptionalNumber(formData, 'wandererastroBaudRate');
            if (baudRate !== null) {
                deviceData.baudRate = baudRate;
            }
        }
    } else if (deviceData.vendor === 'astroasis') {
        const astroasisHidPath = (formData.get('astroasisHidPath') || '').trim();
        if (astroasisHidPath !== '') {
            deviceData.hidPath = astroasisHidPath;
        } else {
            const astroasisFocuserIndex = readOptionalNumber(formData, 'astroasisFocuserIndex');
            deviceData.focuserIndex = astroasisFocuserIndex !== null ? astroasisFocuserIndex : 0;
        }
    }

    const messageDiv = document.getElementById('form-message');
    messageDiv.style.display = 'none';

    try {
        const isEditing = this.dataset.editing === 'true';
        if (isEditing) {
            const originalDeviceType = this.dataset.originalDeviceType || deviceData.deviceType;
            const originalDeviceNumber = Number.parseInt(this.dataset.originalDeviceNumber, 10);
            const originalVendor = this.dataset.originalVendor || deviceData.vendor;

            if (!Number.isNaN(originalDeviceNumber)) {
                const removeResponse = await fetch(API_BASE + '/management/v1/removedevice', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({
                        deviceType: originalDeviceType,
                        deviceNumber: originalDeviceNumber,
                        vendor: originalVendor
                    })
                });

                const removeResult = await removeResponse.json();
                if (removeResult.ErrorNumber !== 0) {
                    const message = removeResult.ErrorMessage || '';
                    if (!message.toLowerCase().includes('device not found')) {
                        messageDiv.style.display = 'block';
                        messageDiv.className = 'message error';
                        messageDiv.textContent = `Error updating device: ${message}`;
                        return;
                    }
                }
            }
        }

        const response = await fetch(API_BASE + '/management/v1/configuredevice', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify(deviceData)
        });

        const result = await response.json();
        
        messageDiv.style.display = 'block';
        if (result.ErrorNumber === 0) {
            // Clear the form first (resetDeviceForm hides the message div), then
            // surface the success message so it stays visible until we navigate.
            resetDeviceForm();
            messageDiv.style.display = 'block';
            messageDiv.className = 'message success';
            messageDiv.textContent = isEditing ? 'Device updated successfully!' : 'Device configured successfully!';
            setTimeout(() => {
                loadDevices();
                showTab('devices');
            }, 1500);
        } else {
            messageDiv.className = 'message error';
            messageDiv.textContent = `Error: ${result.ErrorMessage}`;
        }
    } catch (error) {
        messageDiv.style.display = 'block';
        messageDiv.className = 'message error';
        messageDiv.textContent = `Error: ${error.message}`;
    }
});

document.getElementById('device-form').addEventListener('reset', function() {
    setEditMode(false);
});

// Utility functions
function parseResponseValue(value) {
    if (value === undefined || value === null) {
        return null;
    }
    // The server returns the Alpaca "Value" as structured JSON, so an object or
    // array arrives ready to use. Only attempt to parse a string when it clearly
    // encodes a JSON object/array - this keeps backward compatibility with an
    // older server that double-encoded structured payloads as a string, while
    // never coercing a plain scalar string (e.g. a "12345" serial or "true"
    // text property) into a number/boolean.
    if (typeof value === 'string') {
        const trimmed = value.trim();
        if (trimmed.startsWith('{') || trimmed.startsWith('[')) {
            try {
                return JSON.parse(trimmed);
            } catch (e) {
                return value;
            }
        }
        return value;
    }
    return value;
}

function renderDeviceSettings(config) {
    if (!config || typeof config !== 'object') {
        return '';
    }

    const labelMap = new Map([
        ['connectionType', 'Connection Type'],
        ['portPath', 'Serial Port'],
        ['baudRate', 'Baud Rate'],
        ['host', 'Host'],
        ['tcpPort', 'TCP Port'],
        ['synscanVersion', 'SynScan V3/V4 Version'],
        ['cameraIndex', 'Camera Index'],
        ['cameraId', 'Camera ID'],
        ['filterwheelIndex', 'Filter Wheel Index'],
        ['filterwheelId', 'Filter Wheel ID'],
        ['filterNames', 'Filter Names'],
        ['focuserIndex', 'Focuser Index'],
        ['focuserId', 'Focuser ID'],
        ['rotatorIndex', 'Rotator Index'],
        ['rotatorId', 'Rotator ID'],
        ['responseTimeoutMs', 'Response Timeout (ms)'],
    ]);

    const rows = [];
    const addRow = (key, label) => {
        if (config[key] === undefined || config[key] === null || config[key] === '') {
            return;
        }
        const value = key === 'filterNames'
            ? formatFilterNamesShort(config[key])
            : formatSettingValue(config[key]);
        rows.push(`
            <div class="setting-row">
                <span class="setting-label">${escapeHtml(label)}</span>
                <span class="setting-value">${escapeHtml(value)}</span>
            </div>
        `);
    };
    const addRowValue = (label, value) => {
        if (value === undefined || value === null || value === '') {
            return;
        }
        const formatted = formatSettingValue(value);
        rows.push(`
            <div class="setting-row">
                <span class="setting-label">${escapeHtml(label)}</span>
                <span class="setting-value">${escapeHtml(formatted)}</span>
            </div>
        `);
    };

    labelMap.forEach((label, key) => addRow(key, label));

    // Optics are stored in metres (Alpaca/ASCOM units) but always shown in mm.
    addRowValue('Aperture Diameter (mm)', opticsMetersToMm(config.apertureDiameter));
    addRowValue('Focal Length (mm)', opticsMetersToMm(config.focalLength));

    const apertureDiameter = Number(config.apertureDiameter);
    if (Number.isFinite(apertureDiameter) && apertureDiameter > 0) {
        const radius = apertureDiameter / 2;
        const area = Math.PI * radius * radius;
        addRowValue('Aperture Area (m^2)', area.toFixed(6));
    }

    const hiddenKeys = new Set(['vendor', 'deviceType', 'deviceNumber',
        'apertureDiameter', 'focalLength']);

    Object.keys(config)
        .filter(key => !labelMap.has(key) && !hiddenKeys.has(key))
        .sort()
        .forEach(key => addRow(key, humanizeSettingKey(key)));

    if (!rows.length) {
        return '';
    }

    return `
        <div class="device-settings">
            <h4>Configured Settings</h4>
            <div class="settings-grid">
                ${rows.join('')}
            </div>
        </div>
    `;
}

function formatSettingValue(value) {
    if (typeof value === 'boolean') {
        return value ? 'true' : 'false';
    }
    if (value && typeof value === 'object') {
        try {
            return JSON.stringify(value);
        } catch (e) {
            return String(value);
        }
    }
    return value !== undefined && value !== null ? value.toString() : '';
}

function humanizeSettingKey(key) {
    return key
        .replace(/[_-]+/g, ' ')
        .replace(/([a-z0-9])([A-Z])/g, '$1 $2')
        .replace(/^\w/, match => match.toUpperCase());
}

// Escapes for BOTH element content and (quoted) attribute values: quotes are
// escaped too, so interpolating into a "..."/'...' HTML attribute cannot break
// out of it. Never interpolate untrusted text into an UNQUOTED attribute or an
// inline event handler regardless.
function escapeHtml(text) {
    return String(text)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    loadDevices();
    loadServerInfo();
    refreshServerClockOffset();
    setInterval(updateServerClock, 1000);
    setInterval(refreshServerClockOffset, 60000);
    loadLogSettings();
    loadLogFiles();
    wifiInit();
    updateVendorOptions();

    document.querySelectorAll('.section-toggle').forEach(button => {
        button.addEventListener('click', () => {
            const card = button.closest('.section-card');
            if (!card) {
                return;
            }
            const isCollapsed = card.classList.toggle('collapsed');
            button.setAttribute('aria-expanded', isCollapsed ? 'false' : 'true');
        });
    });

    const refreshLogFiles = document.getElementById('log-files-refresh');
    if (refreshLogFiles) {
        refreshLogFiles.addEventListener('click', loadLogFiles);
    }
    const deleteAllLogFilesBtn = document.getElementById('log-files-delete-all');
    if (deleteAllLogFilesBtn) {
        deleteAllLogFilesBtn.addEventListener('click', deleteAllLogFiles);
    }
    const closeLogViewer = document.getElementById('log-file-viewer-close');
    if (closeLogViewer) {
        closeLogViewer.addEventListener('click', clearLogFileViewer);
    }
});

// ---------------------------------------------------------------------------
// Info buttons + bottom info sheet (shared pattern with OpenAstro Ara).
// Markup-driven: any <button class="info-btn" data-info-title="..."
// data-info-text="..."> opens the sheet. One sheet instance for the page.

function ensureInfoSheet() {
    let sheet = document.getElementById('info-sheet');
    if (sheet) return sheet;
    sheet = document.createElement('div');
    sheet.id = 'info-sheet';
    sheet.className = 'info-sheet';
    sheet.innerHTML =
        '<div class="info-sheet-header">' +
        '<span class="info-sheet-icon">&#9432;</span>' +
        '<span class="info-sheet-title" id="info-sheet-title"></span>' +
        '<button class="info-sheet-close" type="button" aria-label="Close">&#10005;</button>' +
        '</div>' +
        '<div class="info-sheet-body" id="info-sheet-body"></div>';
    document.body.appendChild(sheet);
    sheet.querySelector('.info-sheet-close').addEventListener('click', closeInfoSheet);
    document.addEventListener('click', (e) => {
        if (sheet.classList.contains('open') && !sheet.contains(e.target) &&
            !e.target.closest('.info-btn')) {
            closeInfoSheet();
        }
    });
    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') closeInfoSheet();
    });
    return sheet;
}

function openInfoSheet(title, text) {
    const sheet = ensureInfoSheet();
    document.getElementById('info-sheet-title').textContent = title;
    document.getElementById('info-sheet-body').textContent = text;
    sheet.classList.add('open');
}

function closeInfoSheet() {
    const sheet = document.getElementById('info-sheet');
    if (sheet) sheet.classList.remove('open');
}

document.addEventListener('click', (e) => {
    const btn = e.target.closest('.info-btn');
    if (btn) {
        openInfoSheet(btn.dataset.infoTitle || 'Info', btn.dataset.infoText || '');
    }
});

// ---------------------------------------------------------------------------
// WiFi (see docs/wifi-manager-design.md). Apple-Settings-style interaction:
// toggles take effect immediately, the network list is tappable, and hotspot
// settings save themselves on change. No explicit save buttons.

const WIFI_BASE = '/management/v1/wifi';
const WIFI_COUNTRIES = ['', 'US', 'CA', 'MX', 'GB', 'IE', 'DE', 'FR', 'ES', 'IT', 'NL', 'BE', 'CH', 'AT', 'SE', 'NO', 'FI', 'DK', 'PL', 'CZ', 'PT', 'GR', 'AU', 'NZ', 'JP', 'KR', 'CN', 'TW', 'IN', 'BR', 'AR', 'CL', 'ZA'];

let wifiState = { status: null, ap: null, profiles: [], busy: false };

function wifiEl(id) { return document.getElementById(id); }

function wifiMessage(text, isError) {
    const el = wifiEl('wifi-message');
    if (!el) return;
    el.textContent = text || '';
    el.style.color = isError ? '#f08a7a' : '';
}

async function wifiApi(path, method, body) {
    const options = { method: method || 'GET' };
    if (body !== undefined) {
        options.headers = { 'Content-Type': 'application/json' };
        options.body = JSON.stringify(body);
    }
    const response = await fetch(WIFI_BASE + path, options);
    const result = await response.json();
    if (result.ErrorNumber !== 0) throw new Error(result.ErrorMessage || 'unknown error');
    return result.Value;
}

function wifiSignalIcon(percent) {
    if (percent >= 66) return '███';
    if (percent >= 33) return '██░';
    return '█░░';
}

async function wifiRefresh() {
    const section = wifiEl('wifi-section');
    if (!section) return;
    try {
        const status = await wifiApi('/status');
        wifiState.status = status;
        if (!status.Available) { section.style.display = 'none'; return; }
        section.style.display = '';

        const radioToggle = wifiEl('wifi-radio-toggle');
        if (radioToggle) radioToggle.checked = !!status.WirelessEnabled;

        const apActive = !!status.ApActive;
        const clientConnected = wifiClientConnected(status);
        // On dual-interface boards status.Ssid is the client side, so the
        // hotspot's own name comes from the ap config (fetched below; one
        // refresh behind at worst). Single-radio: status.Ssid IS the hotspot.
        const apSsid = apActive
            ? ((status.DeviceCount > 1 ? (wifiState.ap && wifiState.ap.Ssid) : status.Ssid) || '')
            : '';
        let summary;
        if (!status.WirelessEnabled) summary = 'Off';
        else if (apActive && clientConnected) summary = status.Ssid + ' + Hotspot';
        else if (apActive) summary = 'Hotspot: ' + (apSsid || 'on');
        else if (clientConnected) summary = status.Ssid;
        else summary = 'Not Connected';
        const summaryEl = wifiEl('wifi-summary');
        if (summaryEl) summaryEl.textContent = summary;

        const line = wifiEl('wifi-status-line');
        if (line) {
            // Highlight the line whenever the radio is doing something
            // (joined a network or broadcasting the hotspot) so the state
            // is obvious at a glance; plain grey when idle or off.
            line.classList.toggle('wifi-status-active', !!(status.WirelessEnabled && (clientConnected || apActive)));
            if (!status.WirelessEnabled) {
                line.textContent = 'WiFi is off. The hotspot and network connections are unavailable.';
            } else if (clientConnected) {
                const band = status.FrequencyMhz > 5000 ? '5 GHz' : '2.4 GHz';
                line.textContent = 'Connected to ' + status.Ssid + ' (' + band +
                    (status.Ip4Address ? ', ' + status.Ip4Address : '') + ').' +
                    (apActive ? ' Hotspot' + (apSsid ? ' "' + apSsid + '"' : '') + ' is also on.' : '');
            } else if (apActive) {
                line.textContent = 'Hotspot' + (apSsid ? ' "' + apSsid + '"' : '') + ' is on. Join it and open http://172.24.1.1:6800/';
            } else {
                line.textContent = 'Not connected to any network.';
            }
        }

        const apState = wifiEl('wifi-ap-substate');
        if (apState) apState.textContent = apActive ? 'On' : 'Off';
        const apToggle = wifiEl('wifi-ap-toggle');
        if (apToggle) apToggle.checked = apActive;

        const has5 = (status.Capabilities || {}).Freq5GHz || status.ScanSees5GHz;
        const btn5 = document.querySelector('#wifi-ap-band button[data-band="a"]');
        if (btn5) btn5.style.display = has5 ? '' : 'none';

        const countrySel = wifiEl('wifi-country');
        if (countrySel && status.Country !== undefined) countrySel.value = status.Country || '';

        try {
            const ap = await wifiApi('/ap');
            wifiState.ap = ap;
            if (ap.Configured) {
                const ssidInput = wifiEl('wifi-ap-ssid');
                if (ssidInput && document.activeElement !== ssidInput) ssidInput.value = ap.Ssid || '';
                wifiSetBandUi(ap.Band || 'a');
            }
        } catch (e) { /* ap config is optional */ }

        wifiState.profiles = await wifiApi('/profiles');
        await wifiRenderNetworks(false);
    } catch (e) {
        const line = wifiEl('wifi-status-line');
        if (line) {
            line.classList.remove('wifi-status-active');
            line.textContent = 'WiFi status unavailable: ' + e.message;
        }
    }
}

// True when the device is joined to a network as a client. On dual-interface
// boards (DeviceCount > 1, e.g. OPi 4 Pro ap0 + wlan0) the hotspot and a
// client join run concurrently, and status.Ssid reports the client side, so
// ApActive must not mask it. On single-radio boards an active hotspot means
// status.Ssid is the hotspot's own name, not a joined network.
function wifiClientConnected(status) {
    return !!(status.Ssid && status.State === 'connected' && (!status.ApActive || status.DeviceCount > 1));
}

// Render the network list from the last scan + saved profiles. rescan=true
// triggers a fresh scan first (slower).
async function wifiRenderNetworks(rescan) {
    const list = wifiEl('wifi-networks');
    if (!list) return;
    const status = wifiState.status || {};
    if (!status.WirelessEnabled) {
        list.innerHTML = '<p class="wifi-substatus">Turn WiFi on to see nearby networks.</p>';
        return;
    }
    if (rescan) list.innerHTML = '<p class="wifi-substatus">Scanning...</p>';
    let networks = [];
    try {
        networks = await wifiApi('/scan');
    } catch (e) {
        list.innerHTML = '<p class="wifi-substatus">Scan failed: ' + e.message + '</p>';
        return;
    }
    const profiles = wifiState.profiles || [];
    const savedBySsid = {};
    for (const p of profiles) {
        if (p.Mode !== 'ap') savedBySsid[p.Ssid] = p;
    }
    const activeSsid = wifiClientConnected(status) ? status.Ssid : null;

    list.innerHTML = '';
    const inRange = new Set();
    for (const n of networks) {
        if (n.Ssid === (wifiState.ap && wifiState.ap.Ssid)) continue;  // own hotspot
        inRange.add(n.Ssid);
        const saved = savedBySsid[n.Ssid];
        const isActive = n.Ssid === activeSsid;
        const row = document.createElement('div');
        row.className = 'wifi-net-row' + (isActive ? ' active' : '');
        row.innerHTML =
            '<span class="wifi-net-check">' + (isActive ? '✓' : '') + '</span>' +
            '<span class="wifi-net-name">' + escapeHtml(n.Ssid) + (saved && !isActive ? ' <small>saved</small>' : '') + '</span>' +
            '<span class="wifi-net-meta">' + (n.Security !== 'Open' ? '🔒 ' : '') +
            (n.FrequencyMhz > 5000 ? '5' : '2.4') + ' GHz <span class="wifi-signal">' + wifiSignalIcon(n.SignalPercent) + '</span></span>';
        if (!isActive) {
            row.addEventListener('click', () => saved ? wifiConnectSaved(saved) : wifiJoinNew(n));
        }
        if (saved) {
            const forget = document.createElement('button');
            forget.className = 'btn btn-secondary btn-small';
            forget.textContent = 'Forget';
            forget.addEventListener('click', (e) => { e.stopPropagation(); wifiForget(saved); });
            row.appendChild(forget);
        }
        list.appendChild(row);
    }

    // Saved networks that are not in range right now: manageable (forget).
    const outOfRange = profiles.filter((p) => p.Mode !== 'ap' && !inRange.has(p.Ssid));
    if (outOfRange.length) {
        const title = document.createElement('p');
        title.className = 'wifi-substatus';
        title.textContent = 'Saved, not in range:';
        list.appendChild(title);
        for (const p of outOfRange) {
            const row = document.createElement('div');
            row.className = 'wifi-net-row dim';
            row.innerHTML = '<span class="wifi-net-check"></span><span class="wifi-net-name">' + escapeHtml(p.Ssid) + '</span>';
            const forget = document.createElement('button');
            forget.className = 'btn btn-secondary btn-small';
            forget.textContent = 'Forget';
            forget.addEventListener('click', (e) => { e.stopPropagation(); wifiForget(p); });
            row.appendChild(forget);
            list.appendChild(row);
        }
    }
    if (!list.children.length) {
        list.innerHTML = '<p class="wifi-substatus">No networks found.</p>';
    }
}

function wifiScanClicked() { wifiRenderNetworks(true); }

async function wifiJoinNew(network) {
    let passphrase = '';
    if (network.Security !== 'Open') {
        passphrase = prompt('Password for "' + network.Ssid + '":');
        if (passphrase === null) return;
    }
    if (!wifiConfirmSwitch('join "' + network.Ssid + '"')) return;
    try {
        await wifiApi('/profiles', 'PUT', { Ssid: network.Ssid, Passphrase: passphrase, Autoconnect: true, Priority: 0 });
        const profiles = await wifiApi('/profiles');
        const match = profiles.find((p) => p.Ssid === network.Ssid && p.Mode !== 'ap');
        if (match) await wifiApi('/connect', 'PUT', { Uuid: match.Uuid });
        wifiMessage('Joining ' + network.Ssid + '...');
        setTimeout(wifiRefresh, 8000);
    } catch (e) {
        wifiMessage('Could not join: ' + e.message, true);
    }
}

async function wifiConnectSaved(profile) {
    if (!wifiConfirmSwitch('switch to "' + profile.Ssid + '"')) return;
    try {
        await wifiApi('/connect', 'PUT', { Uuid: profile.Uuid });
        wifiMessage('Connecting to ' + profile.Ssid + '...');
        setTimeout(wifiRefresh, 8000);
    } catch (e) {
        wifiMessage('Could not connect: ' + e.message, true);
    }
}

async function wifiForget(profile) {
    const connectedNow = wifiClientConnected(wifiState.status || {}) && (wifiState.status || {}).Ssid === profile.Ssid;
    if (!confirm('Forget "' + profile.Ssid + '"?' + (connectedNow ? '\n\nThe device is connected to this network right now and will disconnect from it.' : ''))) return;
    try {
        await wifiApi('/profiles/' + encodeURIComponent(profile.Uuid), 'DELETE');
        wifiState.profiles = await wifiApi('/profiles');
        await wifiRenderNetworks(false);
    } catch (e) {
        wifiMessage('Could not forget: ' + e.message, true);
    }
}

// One shared "this may drop your connection" warning, only when it can.
function wifiConfirmSwitch(action) {
    const status = wifiState.status || {};
    // Dual-interface boards keep the hotspot up while the client interface
    // switches, so skip the warning only when the browser is provably ON the
    // hotspot: the page served from the fleet-wide AP address. A browser on
    // the client network still drops (PR #202 review).
    if (status.DeviceCount > 1 && status.ApActive && location.hostname === '172.24.1.1') return true;
    const onWifi = status.Ssid && location.hostname !== 'localhost';
    if (!onWifi) return true;
    return confirm('The device will ' + action + '. If you are connected to it over WiFi right now, this page will lose connection while it switches.');
}

// 5 GHz needs a regulatory country: under the world domain most 5 GHz
// channels are blocked, so the AP can beacon on a channel clients refuse.
// The server rejects Band "a" too; this is the friendly front-end check.
function wifiCountryIsSet() {
    const sel = wifiEl('wifi-country');
    return !!(sel && sel.value);
}

function wifiRequireCountryFor5GHz() {
    alert('Set the regulatory country before using 5 GHz.\n\nWithout it, most 5 GHz channels are blocked and the hotspot may not be visible to your devices.');
    const sel = wifiEl('wifi-country');
    if (sel) sel.focus();
}

function wifiSetBandUi(band) {
    document.querySelectorAll('#wifi-ap-band button').forEach((b) => {
        b.classList.toggle('active', b.dataset.band === band);
    });
}

function wifiSelectedBand() {
    const active = document.querySelector('#wifi-ap-band button.active');
    return active ? active.dataset.band : 'a';
}

// Apply the hotspot config. Called by the toggle and by field changes; field
// changes keep whatever on/off state the toggle shows (auto-save).
async function wifiApplyAp(enabled, fromToggle) {
    if (wifiState.busy) return;
    const ssid = (wifiEl('wifi-ap-ssid') || {}).value || '';
    const passphrase = (wifiEl('wifi-ap-pass') || {}).value || '';
    if (!ssid) { wifiMessage('The hotspot needs a name.', true); return; }
    if (wifiSelectedBand() === 'a' && !wifiCountryIsSet()) {
        wifiRequireCountryFor5GHz();
        if (fromToggle) { const t = wifiEl('wifi-ap-toggle'); if (t) t.checked = false; }
        return;
    }
    const configured = wifiState.ap && wifiState.ap.Configured;
    if (!configured && !passphrase) {
        wifiMessage('Set a hotspot password first (8-63 characters).', true);
        if (fromToggle) { const t = wifiEl('wifi-ap-toggle'); if (t) t.checked = false; }
        return;
    }
    if (enabled && fromToggle && !wifiConfirmSwitch('start the hotspot "' + ssid + '"')) {
        const t = wifiEl('wifi-ap-toggle'); if (t) t.checked = false;
        return;
    }
    wifiState.busy = true;
    try {
        await wifiApi('/ap', 'PUT', { Ssid: ssid, Passphrase: passphrase, Band: wifiSelectedBand(), Channel: 0, Enabled: enabled });
        const passInput = wifiEl('wifi-ap-pass');
        if (passInput) passInput.value = '';
        wifiMessage(fromToggle ? (enabled ? 'Hotspot starting...' : 'Hotspot turned off.') : 'Saved.');
        setTimeout(wifiRefresh, enabled && fromToggle ? 5000 : 2000);
    } catch (e) {
        wifiMessage('Hotspot: ' + e.message, true);
        if (fromToggle) { const t = wifiEl('wifi-ap-toggle'); if (t) t.checked = !enabled; }
    } finally {
        wifiState.busy = false;
    }
}

function wifiInit() {
    const section = wifiEl('wifi-section');
    if (!section) return;

    const countrySel = wifiEl('wifi-country');
    if (countrySel && countrySel.options.length === 0) {
        for (const cc of WIFI_COUNTRIES) {
            const opt = document.createElement('option');
            opt.value = cc;
            opt.textContent = cc || 'Not set';
            countrySel.appendChild(opt);
        }
        countrySel.addEventListener('change', async () => {
            if (!countrySel.value) return;
            try {
                await wifiApi('/country', 'PUT', { Alpha2: countrySel.value });
                wifiMessage('Country set to ' + countrySel.value + '.');
            } catch (e) {
                wifiMessage('Country: ' + e.message, true);
            }
        });
    }

    const radioToggle = wifiEl('wifi-radio-toggle');
    if (radioToggle) {
        radioToggle.addEventListener('change', async () => {
            try {
                await wifiApi('/radio', 'PUT', { Enabled: radioToggle.checked });
                setTimeout(wifiRefresh, 2000);
            } catch (e) {
                wifiMessage('WiFi: ' + e.message, true);
                radioToggle.checked = !radioToggle.checked;
            }
        });
    }

    const apToggle = wifiEl('wifi-ap-toggle');
    if (apToggle) {
        apToggle.addEventListener('change', () => wifiApplyAp(apToggle.checked, true));
    }
    for (const id of ['wifi-ap-ssid', 'wifi-ap-pass']) {
        const el = wifiEl(id);
        if (el) el.addEventListener('change', () => wifiApplyAp((wifiEl('wifi-ap-toggle') || {}).checked || false, false));
    }
    document.querySelectorAll('#wifi-ap-band button').forEach((b) => {
        b.addEventListener('click', () => {
            if (b.dataset.band === 'a' && !wifiCountryIsSet()) {
                wifiRequireCountryFor5GHz();
                return;
            }
            wifiSetBandUi(b.dataset.band);
            wifiApplyAp((wifiEl('wifi-ap-toggle') || {}).checked || false, false);
        });
    });
    const passToggle = wifiEl('wifi-ap-pass-toggle');
    if (passToggle) {
        passToggle.addEventListener('click', () => {
            const input = wifiEl('wifi-ap-pass');
            if (!input) return;
            const showing = input.type === 'text';
            input.type = showing ? 'password' : 'text';
            passToggle.textContent = showing ? 'Show' : 'Hide';
        });
    }

    wifiRefresh();
}
