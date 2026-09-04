// --- ESP32 API Interface ---
const api = {
    _pendingGetRequests: new Map(),

    async _performRequestWithMeta(path, options) {
        const response = await fetch(path, options);
        let data;
        try {
            data = await response.json();
        } catch (_) {
            throw new Error(`Invalid response from ${path}`);
        }
        if (!response.ok || data.success === false) {
            throw new Error(data.message || `HTTP ${response.status}`);
        }
        const rawSeq = response.headers.get('X-Gateway-Event-Seq');
        const eventSeq = rawSeq ? Number(rawSeq) : 0;
        return { data, eventSeq };
    },
    requestWithMeta(path, options = {}) {
        const requestOptions = {...options, credentials: 'same-origin'};
        const method = (requestOptions.method || 'GET').toUpperCase();

        // Several dashboard lifecycles can ask for the same snapshot at once
        // (route restoration, WebSocket startup, and the initial view load).
        // Share only concurrent GETs; mutations and later refreshes must always
        // reach the gateway.
        if (method !== 'GET') {
            return this._performRequestWithMeta(path, requestOptions);
        }

        const pending = this._pendingGetRequests.get(path);
        if (pending) return pending;

        const request = this._performRequestWithMeta(path, requestOptions)
            .finally(() => {
                if (this._pendingGetRequests.get(path) === request) {
                    this._pendingGetRequests.delete(path);
                }
            });
        this._pendingGetRequests.set(path, request);
        return request;
    },
    async request(path, options = {}) {
        const { data } = await this.requestWithMeta(path, options);
        return data;
    },
    async getDevicesSnapshot() {
        const { data, eventSeq } = await this.requestWithMeta('/api/devices');
        return {
            eventSeq,
            devices: data.map(device => ({
                id: device.device_id,
                mac: device.ble_addr || device.device_id,
                addrType: device.ble_addr_type || 0,
                customName: device.name,
                connected: device.connected,
                ready: device.ready,
                status: device.ready ? 'online' : (device.connected ? 'connecting' : 'offline'),
                rssi: null,
                capabilities: device.capabilities || null,
                controls: Array.isArray(device.controls) ? device.controls : [],
                controlsTruncated: device.controls_truncated === true
            }))
        };
    },
    async getDevices() {
        const list = await this.request('/api/devices');
        return list.map(device => ({
            id: device.device_id,
            mac: device.ble_addr || device.device_id,
            addrType: device.ble_addr_type || 0,
            customName: device.name,
            connected: device.connected,
            ready: device.ready,
            status: device.ready ? 'online' : (device.connected ? 'connecting' : 'offline'),
            rssi: null,
            capabilities: device.capabilities || null,
            controls: Array.isArray(device.controls) ? device.controls : [],
            controlsTruncated: device.controls_truncated === true
        }));
    },
    async getDeviceDetailSnapshot(deviceId) {
        const {data, eventSeq} = await this.requestWithMeta(
            `/api/devices/detail?device_id=${encodeURIComponent(deviceId)}`
        );
        return {detail: data, eventSeq};
    },
    async addDevice(data) {
        return this.request('/api/devices', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
                device_id: data.id,
                name: data.customName,
                ble_addr: data.mac,
                ble_addr_type: data.addrType
            })
        });
    },
    async removeDevice(deviceId) {
        return this.request(`/api/devices?device_id=${encodeURIComponent(deviceId)}`, {
            method: 'DELETE'
        });
    },
    async startScan() {
        return this.request('/api/ble/scan', {method: 'POST'});
    },
    async stopScan() {
        return this.request('/api/ble/scan', {method: 'DELETE'});
    },
    async getScanResults() {
        const result = await this.request('/api/ble/scan');
        return {
            scanning: result.scanning,
            devices: result.devices.map(device => ({
                mac: device.ble_addr,
                addrType: device.addr_type || 0,
                name: device.name || 'Unknown Device',
                rssi: device.rssi
            }))
        };
    },
    async updateDevice(deviceId, data) {
        return this.request('/api/devices', {
            method: 'PUT',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
                device_id: deviceId,
                name: data.customName
            })
        });
    },
    getDeviceSchema(deviceId) {
        return this.request(`/api/devices/schema?device_id=${encodeURIComponent(deviceId)}`);
    },
    async getDeviceSchemaSnapshot(deviceId) {
        const { data, eventSeq } = await this.requestWithMeta(
            `/api/devices/schema?device_id=${encodeURIComponent(deviceId)}`
        );
        return { schema: data, eventSeq };
    },
    refreshDeviceSchema(deviceId) {
        return this.request('/api/devices/schema/refresh', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({device_id: deviceId})
        });
    },
    sendCommand(deviceId, command, valueType = 'none', value = null) {
        const payload = {device_id: deviceId, command};
        if (valueType === 'boolean') payload.bool_value = Boolean(value);
        if (valueType === 'integer') payload.int_value = Number(value);
        return this.request('/api/command', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(payload)
        });
    },
    restart() {
        return this.request('/api/restart', {method: 'POST'});
    }
};
