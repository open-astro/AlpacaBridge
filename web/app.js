// AlpacaHTTP Web UI
const API_BASE = '';

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
            devicesList.innerHTML = `
                <div class="empty-state">
                    <p>No devices configured</p>
                    <p>Go to the "Configure" tab to add a device</p>
                </div>
            `;
            return;
        }

        devicesList.innerHTML = devices.map(device => `
            <div class="device-card">
                <h3>${escapeHtml(device.DeviceName)}</h3>
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
                </div>
                <div class="device-actions">
                    <button class="btn btn-danger btn-small" onclick="deleteDevice('${escapeHtml(device.DeviceType)}', ${device.DeviceNumber})">Delete</button>
                </div>
            </div>
        `).join('');
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

// Refresh devices
function refreshDevices() {
    loadDevices();
}

// Delete device
async function deleteDevice(deviceType, deviceNumber) {
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
                deviceType: deviceType,
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

        // Handle Value field - it can be a string (JSON), an object, or undefined
        let desc;
        if (data.Value !== undefined && data.Value !== null) {
            if (typeof data.Value === 'string') {
                // JSON string - parse it
                try {
                    desc = JSON.parse(data.Value);
                } catch (e) {
                    console.error('Failed to parse Value as JSON:', e, 'Value was:', data.Value);
                    serverInfo.innerHTML = `<p class="error">Error loading server info: Invalid JSON format in Value field</p>`;
                    return;
                }
            } else if (typeof data.Value === 'object') {
                // Already an object
                desc = data.Value;
            } else {
                console.warn('Unexpected Value type:', typeof data.Value, data.Value);
                desc = data.Value;
            }
        } else {
            desc = {};
        }
        
        serverInfo.innerHTML = `
            <pre>${JSON.stringify(desc, null, 2)}</pre>
        `;
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
    }
}

function refreshServerInfo() {
    loadServerInfo();
}

// Shutdown server
async function shutdownServer() {
    if (!confirm('Are you sure you want to shutdown the server? This will stop all services.')) {
        return;
    }
    
    try {
        const response = await fetch(API_BASE + '/management/v1/shutdown', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            }
        });
        
        const result = await response.json();
        
        if (result.ErrorNumber === 0) {
            alert('Server shutdown initiated. The server will stop shortly.');
            // Optionally redirect or show a message
            setTimeout(() => {
                document.body.innerHTML = '<div style="text-align: center; padding: 50px;"><h1>Server Shutdown</h1><p>The server has been shut down.</p></div>';
            }, 1000);
        } else {
            alert('Error shutting down server: ' + result.ErrorMessage);
        }
    } catch (error) {
        alert('Error shutting down server: ' + error.message);
    }
}

// Device form handling
document.getElementById('vendor').addEventListener('change', function() {
    const vendor = this.value;
    const configs = document.querySelectorAll('.vendor-config');
    configs.forEach(config => config.style.display = 'none');
    
    if (vendor === 'ioptron') {
        document.getElementById('ioptron-config').style.display = 'block';
    }
});

document.getElementById('connection-type').addEventListener('change', function() {
    const type = this.value;
    document.getElementById('serial-config').style.display = type === 'serial' ? 'block' : 'none';
    document.getElementById('network-config').style.display = type === 'network' ? 'block' : 'none';
});

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
    }

    const messageDiv = document.getElementById('form-message');
    messageDiv.style.display = 'none';

    try {
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
            messageDiv.textContent = 'Device configured successfully!';
            this.reset();
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

// Utility functions
function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    loadDevices();
    loadServerInfo();
});
