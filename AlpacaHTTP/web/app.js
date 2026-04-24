// AlpacaHTTP Web UI
const API_BASE = '';
const LOGGING_ENDPOINT = '/management/v1/loglevel';
const LOGS_ENDPOINT = '/management/v1/logs';
const LOG_HISTORY_ENDPOINT = '/management/v1/loghistory';
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
let lastLogHistoryLimit = 2000;

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

function setLogHistoryControlsDisabled(disabled) {
    const toggle = document.getElementById('log-history-toggle');
    if (toggle) {
        toggle.disabled = disabled;
    }
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

async function requestLogHistoryUpdate(limit) {
    const statusEl = document.getElementById('log-history-status');
    if (!statusEl) {
        return;
    }

    setLogHistoryControlsDisabled(true);
    statusEl.textContent = 'Updating log history...';

    try {
        const response = await fetch(API_BASE + LOG_HISTORY_ENDPOINT, {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({limit})
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
        const nextLimit = Number(payload.Limit);
        const unlimited = payload.Unlimited === true || nextLimit === 0;
        if (!unlimited && Number.isFinite(nextLimit) && nextLimit > 0) {
            lastLogHistoryLimit = nextLimit;
        }

        const toggle = document.getElementById('log-history-toggle');
        if (toggle) {
            toggle.checked = unlimited;
        }
        statusEl.textContent = unlimited
            ? 'Log history: unlimited (stored until restart)'
            : `Log history: last ${lastLogHistoryLimit} lines`;
    } catch (error) {
        statusEl.textContent = `Failed to update log history: ${error.message}`;
        await loadLogHistorySettings();
    } finally {
        setLogHistoryControlsDisabled(false);
    }
}

// Tab management
function showTab(tabName) {
    // Hide all tabs
    document.querySelectorAll('.tab-content').forEach(tab => {
        tab.classList.remove('active');
    });
    document.querySelectorAll('.tab').forEach(btn => {
        btn.classList.remove('active');
    });

    // Show selected tab
    document.getElementById(tabName + '-tab').classList.add('active');
    event.target.classList.add('active');
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

function getNextZwoCameraIndex() {
    const used = new Set();
    currentDevices.forEach(device => {
        const vendor = extractVendor(device);
        const type = extractDeviceType(device);
        if (vendor !== 'zwo' || type !== 'camera') {
            return;
        }
        const config = extractDeviceConfig(device);
        const indexValue = config && config.cameraIndex;
        const parsed = Number.parseInt(indexValue, 10);
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

function maybeAutoFillZwoCameraIndex() {
    const form = document.getElementById('device-form');
    const vendorSelect = document.getElementById('vendor');
    const deviceTypeSelect = document.getElementById('device-type');
    const cameraIndexInput = document.getElementById('camera-index');
    const cameraIdInput = document.getElementById('camera-id');
    if (!form || !vendorSelect || !deviceTypeSelect || !cameraIndexInput) {
        return;
    }
    if (form.dataset.editing === 'true') {
        return;
    }
    if (vendorSelect.value !== 'zwo' || normalizeDeviceType(deviceTypeSelect.value) !== 'camera') {
        return;
    }
    if (cameraIdInput && cameraIdInput.value.trim() !== '') {
        return;
    }
    if (cameraIndexInput.dataset.userModified === 'true') {
        return;
    }
    const nextIndex = getNextZwoCameraIndex();
    if (nextIndex === null) {
        return;
    }
    cameraIndexInput.value = nextIndex;
}

function updateAutoNumbering() {
    maybeAutoFillDeviceNumber();
    maybeAutoFillZwoCameraIndex();
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
            devicesList.innerHTML = `<p class="error">Error: ${data.ErrorMessage || 'Unknown error'}</p>`;
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
            const vendor = (device.Vendor || (config && config.vendor) || '—').toString();
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
                </div>
                ${settingsHtml}
                <div class="device-actions">
                    <button class="btn btn-secondary btn-small btn-edit-device" data-device-index="${index}" type="button">Edit</button>
                    <button class="btn btn-danger btn-small" onclick="deleteDevice('${escapeHtml(device.DeviceType)}', ${device.DeviceNumber})">Delete</button>
                </div>
            </div>
        `;
        }).join('');

        document.querySelectorAll('.btn-edit-device').forEach(button => {
            button.addEventListener('click', handleEditDeviceClick);
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

function handleEditDeviceClick(event) {
    const button = event.currentTarget;
    const index = Number.parseInt(button.dataset.deviceIndex, 10);
    const device = currentDevices[index];
    if (!device) {
        return;
    }
    startEditDevice(device);
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
            const cameraIndexInput = document.getElementById('camera-index');
            if (deviceNumberInput) {
                delete deviceNumberInput.dataset.userModified;
            }
            if (cameraIndexInput) {
                delete cameraIndexInput.dataset.userModified;
            }
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

    const diameter = Number.parseFloat(diameterEl.value);
    if (!Number.isFinite(diameter) || diameter <= 0) {
        areaEl.value = '';
        return;
    }

    const radius = diameter / 2;
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

    document.getElementById('vendor').dispatchEvent(new Event('change'));

    if (vendor === 'ioptron') {
        const ioptronConnectionType = config.connectionType || 'auto';
        setFormValue('ioptron-connection-type', ioptronConnectionType);
        if (ioptronConnectionType === 'serial') {
            setFormValue('ioptron-port-path', config.portPath);
            setFormValue('ioptron-baud-rate', config.baudRate);
        } else if (ioptronConnectionType === 'network') {
            setFormValue('ioptron-host', config.host);
            setFormValue('ioptron-tcp-port', config.tcpPort);
        }
        setFormValue('aperture-diameter', config.apertureDiameter);
        setFormValue('focal-length', config.focalLength);
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
        setFormValue('synscan-aperture-diameter', config.apertureDiameter);
        setFormValue('synscan-focal-length', config.focalLength);
        updateApertureAreaFromDiameter('synscan-aperture-diameter', 'synscan-aperture-area');
        const synscanConnectionTypeEl = document.getElementById('synscan-connection-type');
        if (synscanConnectionTypeEl) {
            synscanConnectionTypeEl.dispatchEvent(new Event('change'));
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
        setFormValue('celestron-aperture-diameter', config.apertureDiameter);
        setFormValue('celestron-focal-length', config.focalLength);
        const celestronConnectionTypeEl = document.getElementById('celestron-connection-type');
        if (celestronConnectionTypeEl) {
            celestronConnectionTypeEl.dispatchEvent(new Event('change'));
        }
    } else if (vendor === 'bisque') {
        setFormValue('bisque-host', config.host || 'localhost');
        setFormValue('bisque-tcp-port', config.tcpPort || 3040);
        setFormValue('bisque-aperture-diameter', config.apertureDiameter);
        setFormValue('bisque-focal-length', config.focalLength);
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
        setFormValue('zwo-mount-aperture-diameter', config.apertureDiameter);
        setFormValue('zwo-mount-focal-length', config.focalLength);
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
    } else if (vendor === 'qhy') {
        setFormValue('qhy-camera-index', config.cameraIndex);
        setFormValue('qhy-camera-id', config.cameraId);
    } else if (vendor === 'svbony') {
        setFormValue('svbony-camera-index', config.cameraIndex);
    } else if (vendor === 'touptek') {
        setFormValue('touptek-camera-index', config.cameraIndex);
    } else if (vendor === 'playerone') {
        setFormValue('playerone-camera-index', config.cameraIndex);
    } else if (vendor === 'weewx') {
        setFormValue('weewx-url', config.weewxUrl);
        setFormValue('weewx-poll-interval', config.pollIntervalSeconds);
        setFormValue('weewx-timeout', config.timeoutMs);
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
        syncFilterwheelSlotsFromTextarea();
    }

    setFormValue('focuser-index', config.focuserIndex);
    setFormValue('focuser-id', config.focuserId);
    setFormValue('rotator-index', config.rotatorIndex);
    setFormValue('rotator-id', config.rotatorId);
    setFormValue('zwo-switch-type', config.switchType);

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

    const configureTabButton = document.querySelector('.tab[onclick*="configure"]');
    if (configureTabButton) {
        configureTabButton.click();
    }
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
            serverInfo.innerHTML = `<p class="error">Error: ${data.ErrorMessage || 'Unknown error'}</p>`;
            return;
        }

        const desc = parseResponseValue(data.Value) || {};
        const serverName = resolveDescriptionValue(desc, ['ServerName', 'serverName']) || '—';
        const manufacturer = resolveDescriptionValue(desc, ['Manufacturer', 'manufacturer']) || '—';
        const manufacturerVersion = resolveDescriptionValue(desc, ['ManufacturerVersion', 'manufacturerVersion', 'Version', 'version']) || '—';
        const location = resolveDescriptionValue(desc, ['Location', 'location']) || '';

        serverInfo.innerHTML = `
            <div class="server-info-grid">
                ${renderServerInfoRow('Server Name', serverName)}
                ${renderServerInfoRow('Manufacturer', manufacturer)}
                ${renderServerInfoRow('Version', manufacturerVersion)}
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
    const displayValue = value !== undefined && value !== null && value !== '' ? value : '—';
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

function refreshServerInfo() {
    loadServerInfo();
    loadLogSettings();
    loadLogHistorySettings();
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

async function loadLogHistorySettings() {
    const statusEl = document.getElementById('log-history-status');
    if (!statusEl) {
        return;
    }

    setLogHistoryControlsDisabled(true);
    statusEl.textContent = 'Loading log history settings...';

    try {
        const response = await fetch(API_BASE + LOG_HISTORY_ENDPOINT);
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
        const limit = Number(payload.Limit);
        const unlimited = payload.Unlimited === true || limit === 0;
        if (!unlimited && Number.isFinite(limit) && limit > 0) {
            lastLogHistoryLimit = limit;
        }

        const toggle = document.getElementById('log-history-toggle');
        if (toggle) {
            toggle.checked = unlimited;
        }

        statusEl.textContent = unlimited
            ? 'Log history: unlimited (stored until restart)'
            : `Log history: last ${lastLogHistoryLimit} lines`;
    } catch (error) {
        statusEl.textContent = `Unable to load log history settings: ${error.message}`;
    } finally {
        setLogHistoryControlsDisabled(false);
    }
}

async function handleLogHistoryToggleChange(event) {
    const isUnlimited = event.target.checked;
    const nextLimit = isUnlimited ? 0 : (lastLogHistoryLimit || 2000);
    await requestLogHistoryUpdate(nextLimit);
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

async function downloadLogs() {
    const statusEl = document.getElementById('log-level-status');
    const previousStatus = statusEl ? statusEl.textContent : '';
    if (statusEl) {
        statusEl.textContent = 'Preparing log download...';
    }

    try {
        const response = await fetch(API_BASE + LOGS_ENDPOINT + '?format=plain&download=1');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const blob = await response.blob();
        const now = new Date();
        const timestamp = now.toISOString().replace(/[:.]/g, '-');
        const filename = `alpacahttp-logs-${timestamp}.txt`;
        const url = URL.createObjectURL(blob);
        const anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = filename;
        document.body.appendChild(anchor);
        anchor.click();
        anchor.remove();
        URL.revokeObjectURL(url);

        if (statusEl) {
            statusEl.textContent = previousStatus;
        }
    } catch (error) {
        if (statusEl) {
            statusEl.textContent = `Failed to download logs: ${error.message}`;
            setTimeout(() => {
                statusEl.textContent = previousStatus;
            }, 4000);
        }
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
    const ioptronOption = vendorSelect.querySelector('option[value="ioptron"]');
    if (ioptronOption) {
        ioptronOption.disabled = !isTelescope;
        ioptronOption.hidden = !isTelescope;
    }
    const synscanOption = vendorSelect.querySelector('option[value="synscan"]');
    if (synscanOption) {
        synscanOption.disabled = !isTelescope;
        synscanOption.hidden = !isTelescope;
    }
    const celestronOption = vendorSelect.querySelector('option[value="celestron"]');
    if (celestronOption) {
        celestronOption.disabled = !isTelescope;
        celestronOption.hidden = !isTelescope;
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
        qhyOption.disabled = !isCamera;
        qhyOption.hidden = !isCamera;
    }
    const svbonyOption = vendorSelect.querySelector('option[value="svbony"]');
    if (svbonyOption) {
        svbonyOption.disabled = !isCamera;
        svbonyOption.hidden = !isCamera;
    }
    const touptekOption = vendorSelect.querySelector('option[value="touptek"]');
    if (touptekOption) {
        touptekOption.disabled = !isCamera;
        touptekOption.hidden = !isCamera;
    }
    const playerOneOption = vendorSelect.querySelector('option[value="playerone"]');
    if (playerOneOption) {
        playerOneOption.disabled = !isCamera;
        playerOneOption.hidden = !isCamera;
    }
    const weewxOption = vendorSelect.querySelector('option[value="weewx"]');
    if (weewxOption) {
        weewxOption.disabled = !isObservingConditions;
        weewxOption.hidden = !isObservingConditions;
    }
    const geminiOption = vendorSelect.querySelector('option[value="gemini"]');
    if (geminiOption) {
        geminiOption.disabled = !isFocuser;
        geminiOption.hidden = !isFocuser;
    }

    if (!isTelescope && vendorSelect.value === 'ioptron') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'synscan') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'celestron') {
        vendorSelect.value = '';
    }
    if (!isTelescope && vendorSelect.value === 'bisque') {
        vendorSelect.value = '';
    }
    if (!isTelescope && !isCamera && !isSwitch && !isFilterWheel && !isFocuser && !isRotator &&
        vendorSelect.value === 'zwo') {
        vendorSelect.value = '';
    }
    if (!isCamera && vendorSelect.value === 'qhy') {
        vendorSelect.value = '';
    }
    if (!isCamera && vendorSelect.value === 'svbony') {
        vendorSelect.value = '';
    }
    if (!isCamera && vendorSelect.value === 'touptek') {
        vendorSelect.value = '';
    }
    if (!isCamera && vendorSelect.value === 'playerone') {
        vendorSelect.value = '';
    }
    if (!isObservingConditions && vendorSelect.value === 'weewx') {
        vendorSelect.value = '';
    }
    if (!isFocuser && vendorSelect.value === 'gemini') {
        vendorSelect.value = '';
    }

    vendorSelect.dispatchEvent(new Event('change'));
}

document.getElementById('device-type').addEventListener('change', updateVendorOptions);

document.getElementById('vendor').addEventListener('change', function() {
    const vendor = this.value;
    const configs = document.querySelectorAll('.vendor-config');
    configs.forEach(config => config.style.display = 'none');
    
    if (vendor === 'ioptron') {
        document.getElementById('ioptron-config').style.display = 'block';
    } else if (vendor === 'synscan') {
        document.getElementById('synscan-config').style.display = 'block';
    } else if (vendor === 'celestron') {
        document.getElementById('celestron-config').style.display = 'block';
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
    }

    updateZwoConfigFields();
    updateAutoNumbering();
});

const ioptronConnectionType = document.getElementById('ioptron-connection-type');
if (ioptronConnectionType) {
    ioptronConnectionType.addEventListener('change', function() {
        const type = this.value;
        document.getElementById('ioptron-serial-config').style.display = type === 'serial' ? 'block' : 'none';
        document.getElementById('ioptron-network-config').style.display = type === 'network' ? 'block' : 'none';
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

const deviceNumberInput = document.getElementById('device-number');
if (deviceNumberInput) {
    deviceNumberInput.addEventListener('input', () => {
        deviceNumberInput.dataset.userModified = 'true';
    });
}

const cameraIndexInput = document.getElementById('camera-index');
if (cameraIndexInput) {
    cameraIndexInput.addEventListener('input', () => {
        cameraIndexInput.dataset.userModified = 'true';
    });
}

const filterwheelIndexInput = document.getElementById('filterwheel-index');
if (filterwheelIndexInput) {
    filterwheelIndexInput.addEventListener('input', () => {
        filterwheelIndexInput.dataset.userModified = 'true';
    });
}

const filterwheelSlotCountSelect = document.getElementById('filterwheel-slot-count');
const filterwheelSlotCustomInput = document.getElementById('filterwheel-slot-custom');
const filterwheelNamesTextarea = document.getElementById('filterwheel-names');

if (filterwheelSlotCountSelect) {
    updateFilterwheelSlotCountVisibility();
    filterwheelSlotCountSelect.addEventListener('change', () => {
        updateFilterwheelSlotCountVisibility();
        const slotCount = getFilterwheelSlotCount();
        if (!slotCount) {
            renderFilterwheelSlots(0, []);
            return;
        }
        renderFilterwheelSlots(slotCount, []);
        syncFilterwheelNamesFromSlots();
    });
}

if (filterwheelSlotCustomInput) {
    filterwheelSlotCustomInput.addEventListener('input', () => {
        if (!filterwheelSlotCountSelect || filterwheelSlotCountSelect.value !== 'custom') {
            return;
        }
        const slotCount = getFilterwheelSlotCount();
        if (!slotCount) {
            renderFilterwheelSlots(0, []);
            return;
        }
        renderFilterwheelSlots(slotCount, []);
        syncFilterwheelNamesFromSlots();
    });
}

if (filterwheelNamesTextarea) {
    filterwheelNamesTextarea.addEventListener('input', () => {
        syncFilterwheelSlotsFromTextarea();
    });
}

const focuserIndexInput = document.getElementById('focuser-index');
if (focuserIndexInput) {
    focuserIndexInput.addEventListener('input', () => {
        focuserIndexInput.dataset.userModified = 'true';
    });
}

const rotatorIndexInput = document.getElementById('rotator-index');
if (rotatorIndexInput) {
    rotatorIndexInput.addEventListener('input', () => {
        rotatorIndexInput.dataset.userModified = 'true';
    });
}

const apertureDiameterInput = document.getElementById('aperture-diameter');
if (apertureDiameterInput) {
    apertureDiameterInput.addEventListener('input', () =>
        updateApertureAreaFromDiameter('aperture-diameter', 'aperture-area'));
}
const synscanApertureDiameterInput = document.getElementById('synscan-aperture-diameter');
if (synscanApertureDiameterInput) {
    synscanApertureDiameterInput.addEventListener('input', () =>
        updateApertureAreaFromDiameter('synscan-aperture-diameter', 'synscan-aperture-area'));
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

function normalizeFilterName(name) {
    return String(name || '').toLowerCase().replace(/[^a-z0-9]/g, '');
}

function parseFilterNamesInput(rawValue) {
    if (!rawValue) {
        return [];
    }
    let names = rawValue
        .toString()
        .split(/\r?\n/)
        .map(name => name.trim())
        .filter(name => name.length > 0);
    if (names.length === 1) {
        const candidate = names[0];
        if (candidate.length > 1 && !/[,\s;]/.test(candidate)) {
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

function getFilterwheelSlotCount() {
    const countSelect = document.getElementById('filterwheel-slot-count');
    const customInput = document.getElementById('filterwheel-slot-custom');
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

function updateFilterwheelSlotCountVisibility() {
    const countSelect = document.getElementById('filterwheel-slot-count');
    const customInput = document.getElementById('filterwheel-slot-custom');
    if (!countSelect || !customInput) {
        return;
    }
    const showCustom = countSelect.value === 'custom';
    customInput.style.display = showCustom ? 'block' : 'none';
    if (!showCustom) {
        customInput.value = '';
    }
}

function buildFilterwheelSlotRow(index, name) {
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

    const customInput = document.createElement('input');
    customInput.type = 'text';
    customInput.placeholder = 'Custom name';

    const resolvedPreset = resolveFilterPreset(name);
    if (resolvedPreset) {
        select.value = resolvedPreset;
    } else if (name) {
        select.value = FILTERWHEEL_CUSTOM_VALUE;
        customInput.value = name;
    } else {
        select.value = '';
    }

    const updateCustomState = () => {
        const isCustom = select.value === FILTERWHEEL_CUSTOM_VALUE;
        row.classList.toggle('custom-active', isCustom);
        customInput.disabled = !isCustom;
        if (!isCustom) {
            customInput.value = '';
        }
    };

    select.addEventListener('change', () => {
        updateCustomState();
        syncFilterwheelNamesFromSlots();
    });

    customInput.addEventListener('input', () => {
        syncFilterwheelNamesFromSlots();
    });

    updateCustomState();

    row.appendChild(label);
    row.appendChild(select);
    row.appendChild(customInput);
    return row;
}

let filterwheelSyncInProgress = false;

function renderFilterwheelSlots(slotCount, names) {
    const slotList = document.getElementById('filterwheel-slot-list');
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
        slotList.appendChild(buildFilterwheelSlotRow(i + 1, name));
    }
}

function syncFilterwheelNamesFromSlots() {
    if (filterwheelSyncInProgress) {
        return;
    }
    const textarea = document.getElementById('filterwheel-names');
    const slotList = document.getElementById('filterwheel-slot-list');
    if (!textarea || !slotList) {
        return;
    }
    const rows = Array.from(slotList.querySelectorAll('.filterwheel-slot-row'));
    if (rows.length === 0) {
        return;
    }
    filterwheelSyncInProgress = true;
    const names = rows.map((row, index) => {
        const select = row.querySelector('select');
        const customInput = row.querySelector('input[type="text"]');
        const selected = select ? select.value : '';
        if (selected === FILTERWHEEL_CUSTOM_VALUE) {
            const customName = customInput ? customInput.value.trim() : '';
            return customName || `Filter ${index + 1}`;
        }
        if (selected) {
            return selected;
        }
        return `Filter ${index + 1}`;
    });
    textarea.value = names.join('\n');
    filterwheelSyncInProgress = false;
}

function syncFilterwheelSlotsFromTextarea() {
    if (filterwheelSyncInProgress) {
        return;
    }
    const textarea = document.getElementById('filterwheel-names');
    const countSelect = document.getElementById('filterwheel-slot-count');
    const customInput = document.getElementById('filterwheel-slot-custom');
    if (!textarea || !countSelect) {
        return;
    }
    filterwheelSyncInProgress = true;
    const names = parseFilterNamesInput(textarea.value);
    if (names.length === 0) {
        countSelect.value = '';
        if (customInput) {
            customInput.value = '';
        }
        updateFilterwheelSlotCountVisibility();
        renderFilterwheelSlots(0, []);
        filterwheelSyncInProgress = false;
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
    updateFilterwheelSlotCountVisibility();
    renderFilterwheelSlots(names.length, names);
    filterwheelSyncInProgress = false;
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

    const showCameraFields = isCamera || isSwitch;
    if (cameraFields) {
        cameraFields.style.display = showCameraFields ? 'block' : 'none';
        setFieldGroupEnabled(cameraFields, showCameraFields);
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

    if (deviceData.vendor === 'ioptron') {
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
        // "auto" needs no connection fields — port is discovered at startup

        const apertureDiameter = readOptionalNumber(formData, 'apertureDiameter');
        if (apertureDiameter !== null) {
            deviceData.apertureDiameter = apertureDiameter;
        }

        const focalLength = readOptionalNumber(formData, 'focalLength');
        if (focalLength !== null) {
            deviceData.focalLength = focalLength;
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
        // "auto" needs no connection fields — port is discovered at startup

        const apertureDiameter = readOptionalNumber(formData, 'synscanApertureDiameter');
        if (apertureDiameter !== null) {
            deviceData.apertureDiameter = apertureDiameter;
        }

        const focalLength = readOptionalNumber(formData, 'synscanFocalLength');
        if (focalLength !== null) {
            deviceData.focalLength = focalLength;
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
        // "auto" needs no connection fields — port is discovered at startup

        const apertureDiameter = readOptionalNumber(formData, 'celestronApertureDiameter');
        if (apertureDiameter !== null) {
            deviceData.apertureDiameter = apertureDiameter;
        }

        const focalLength = readOptionalNumber(formData, 'celestronFocalLength');
        if (focalLength !== null) {
            deviceData.focalLength = focalLength;
        }
    } else if (deviceData.vendor === 'bisque') {
        deviceData.host = formData.get('bisqueHost') || 'localhost';
        deviceData.tcpPort = parseInt(formData.get('bisqueTcpPort')) || 3040;

        const apertureDiameter = readOptionalNumber(formData, 'bisqueApertureDiameter');
        if (apertureDiameter !== null) {
            deviceData.apertureDiameter = apertureDiameter;
        }

        const focalLength = readOptionalNumber(formData, 'bisqueFocalLength');
        if (focalLength !== null) {
            deviceData.focalLength = focalLength;
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

            const apertureDiameter = readOptionalNumber(formData, 'zwoMountApertureDiameter');
            if (apertureDiameter !== null) {
                deviceData.apertureDiameter = apertureDiameter;
            }

            const focalLength = readOptionalNumber(formData, 'zwoMountFocalLength');
            if (focalLength !== null) {
                deviceData.focalLength = focalLength;
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
            const names = parseFilterNamesInput(filterNamesRaw);
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
    } else if (deviceData.vendor === 'qhy') {
        const qhyCameraId = formData.get('cameraId');
        if (qhyCameraId && qhyCameraId.trim() !== '') {
            deviceData.cameraId = qhyCameraId.trim();
        } else {
            const qhyCameraIndex = readOptionalNumber(formData, 'cameraIndex');
            deviceData.cameraIndex = qhyCameraIndex !== null ? qhyCameraIndex : 0;
        }
    } else if (deviceData.vendor === 'svbony') {
        const svbonyCameraIndex = readOptionalNumber(formData, 'cameraIndex');
        deviceData.cameraIndex = svbonyCameraIndex !== null ? svbonyCameraIndex : 0;
    } else if (deviceData.vendor === 'touptek') {
        const touptekCameraIndex = readOptionalNumber(formData, 'cameraIndex');
        deviceData.cameraIndex = touptekCameraIndex !== null ? touptekCameraIndex : 0;
    } else if (deviceData.vendor === 'playerone') {
        const playerOneCameraIndex = readOptionalNumber(formData, 'cameraIndex');
        deviceData.cameraIndex = playerOneCameraIndex !== null ? playerOneCameraIndex : 0;
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
    } else if (deviceData.vendor === 'gemini') {
        const geminiConnType = document.getElementById('gemini-connection-type');
        deviceData.connectionType = geminiConnType ? geminiConnType.value : 'auto';
        if (deviceData.connectionType === 'auto') {
            const focuserIndex = readOptionalNumber(formData, 'focuserIndex');
            deviceData.focuserIndex = focuserIndex !== null ? focuserIndex : 0;
        } else if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('portPath') || '';
            const baudRate = readOptionalNumber(formData, 'baudRate');
            if (baudRate !== null) {
                deviceData.baudRate = baudRate;
            }
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
            messageDiv.className = 'message success';
            messageDiv.textContent = isEditing ? 'Device updated successfully!' : 'Device configured successfully!';
            this.reset();
            setEditMode(false);
            setTimeout(() => {
                loadDevices();
                showTab('devices');
                document.querySelector('.tab').click();
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
    if (typeof value === 'string') {
        try {
            return JSON.parse(value);
        } catch (e) {
            return value;
        }
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
        ['apertureDiameter', 'Aperture Diameter (m)'],
        ['focalLength', 'Focal Length (m)'],
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

    const apertureDiameter = Number(config.apertureDiameter);
    if (Number.isFinite(apertureDiameter) && apertureDiameter > 0) {
        const radius = apertureDiameter / 2;
        const area = Math.PI * radius * radius;
        addRowValue('Aperture Area (m^2)', area.toFixed(6));
    }

    const hiddenKeys = new Set(['vendor', 'deviceType', 'deviceNumber']);

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

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    loadDevices();
    loadServerInfo();
    loadLogSettings();
    loadLogHistorySettings();
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

    const logHistoryToggle = document.getElementById('log-history-toggle');
    if (logHistoryToggle) {
        logHistoryToggle.addEventListener('change', handleLogHistoryToggleChange);
    }
});
