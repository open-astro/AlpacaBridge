// AlpacaHTTP Web UI
const API_BASE = '';
const LOGGING_ENDPOINT = '/management/v1/loglevel';
const LOGS_ENDPOINT = '/management/v1/logs';
const LOG_HISTORY_ENDPOINT = '/management/v1/loghistory';
const QUIET_LOG_LEVEL = 'WARNING';
const LOG_LEVEL_ORDER = ['TRACE', 'DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL'];
let lastLogHistoryLimit = 2000;

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
        const response = await fetch(API_BASE + '/management/v1/configureddevices');
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

        currentDevices = devices;
        devicesList.innerHTML = devices.map((device, index) => {
            const config = device.Config || device.config || null;
            const vendor = (device.Vendor || (config && config.vendor) || '—').toString();
            const settingsHtml = renderDeviceSettings(config);
            const deviceName = device.DeviceName || device.Name || 'Unknown Device';
            return `
            <div class="device-card">
                <h3>${escapeHtml(deviceName)}</h3>
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

function updateApertureAreaFromDiameter() {
    const diameterEl = document.getElementById('aperture-diameter');
    const areaEl = document.getElementById('aperture-area');
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

    const connectionTypeEl = document.getElementById('connection-type');
    if (connectionTypeEl) {
        const connectionType = config.connectionType || 'serial';
        setFormValue('connection-type', connectionType);
        connectionTypeEl.dispatchEvent(new Event('change'));
    }

    setFormValue('port-path', config.portPath);
    setFormValue('baud-rate', config.baudRate);
    setFormValue('host', config.host);
    setFormValue('tcp-port', config.tcpPort);
    setFormValue('aperture-diameter', config.apertureDiameter);
    setFormValue('focal-length', config.focalLength);
    setFormValue('camera-index', config.cameraIndex);
    setFormValue('camera-id', config.cameraId);
    setFormValue('zwo-switch-type', config.switchType);
    updateApertureAreaFromDiameter();

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
    const ioptronOption = vendorSelect.querySelector('option[value="ioptron"]');
    if (ioptronOption) {
        ioptronOption.disabled = !isTelescope;
        ioptronOption.hidden = !isTelescope;
    }
    const zwoOption = vendorSelect.querySelector('option[value="zwo"]');
    if (zwoOption) {
        const zwoAllowed = isCamera || isSwitch;
        zwoOption.disabled = !zwoAllowed;
        zwoOption.hidden = !zwoAllowed;
    }

    if (!isTelescope && vendorSelect.value === 'ioptron') {
        vendorSelect.value = '';
    }
    if (!isCamera && !isSwitch && vendorSelect.value === 'zwo') {
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
    } else if (vendor === 'zwo') {
        document.getElementById('zwo-config').style.display = 'block';
    }

    updateZwoConfigFields();
    updateAutoNumbering();
});

document.getElementById('connection-type').addEventListener('change', function() {
    const type = this.value;
    document.getElementById('serial-config').style.display = type === 'serial' ? 'block' : 'none';
    document.getElementById('network-config').style.display = type === 'network' ? 'block' : 'none';
});

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

const apertureDiameterInput = document.getElementById('aperture-diameter');
if (apertureDiameterInput) {
    apertureDiameterInput.addEventListener('input', updateApertureAreaFromDiameter);
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

function updateZwoConfigFields() {
    const deviceTypeSelect = document.getElementById('device-type');
    const switchTypeGroup = document.getElementById('zwo-switch-type-group');
    if (!deviceTypeSelect || !switchTypeGroup) {
        return;
    }
    const deviceType = normalizeDeviceType(deviceTypeSelect.value);
    switchTypeGroup.style.display = deviceType === 'switch' ? 'block' : 'none';
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
        if (deviceData.connectionType === 'serial') {
            deviceData.portPath = formData.get('portPath');
            // Default to 115200 for modern iOptron mounts if the field is empty/invalid
            deviceData.baudRate = parseInt(formData.get('baudRate')) || 115200;
        } else {
            deviceData.host = formData.get('host');
            deviceData.tcpPort = parseInt(formData.get('tcpPort')) || 4030;
        }

        const apertureDiameter = readOptionalNumber(formData, 'apertureDiameter');
        if (apertureDiameter !== null) {
            deviceData.apertureDiameter = apertureDiameter;
        }

        const focalLength = readOptionalNumber(formData, 'focalLength');
        if (focalLength !== null) {
            deviceData.focalLength = focalLength;
        }
    } else if (deviceData.vendor === 'zwo') {
        if (deviceData.deviceType === 'switch') {
            const switchType = formData.get('switchType');
            if (switchType) {
                deviceData.switchType = switchType;
            }
        }
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
        ['cameraIndex', 'Camera Index'],
        ['cameraId', 'Camera ID'],
        ['apertureDiameter', 'Aperture Diameter (m)'],
        ['focalLength', 'Focal Length (m)'],
        ['responseTimeoutMs', 'Response Timeout (ms)'],
    ]);

    const rows = [];
    const addRow = (key, label) => {
        if (config[key] === undefined || config[key] === null || config[key] === '') {
            return;
        }
        const value = formatSettingValue(config[key]);
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

    Object.keys(config)
        .filter(key => !labelMap.has(key))
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

    const logHistoryToggle = document.getElementById('log-history-toggle');
    if (logHistoryToggle) {
        logHistoryToggle.addEventListener('change', handleLogHistoryToggleChange);
    }
});
