// --- Connected Devices Logic ---
const devices = {
    capabilitiesLoadId: 0,
    loadPromise: null,
    connectionRefreshes: new Map(),

    async load() {
        // Navigation can invoke switchTab() once from the click handler and
        // once again from hashchange. Share the in-flight request so opening
        // the Devices tab does not issue duplicate API calls.
        if (this.loadPromise) return this.loadPromise;

        const loadOperation = this.loadFresh();
        this.loadPromise = loadOperation;
        try {
            return await loadOperation;
        } finally {
            if (this.loadPromise === loadOperation) this.loadPromise = null;
        }
    },

    async loadFresh() {
        try {
            state.connectedDevices = await api.getDevices();
            state.devicesLoaded = true;
            this.renderGrid();
            return true;
        } catch(e) {
            ui.showToast("Failed to load devices", "error");
            return false;
        }
    },

    refreshConnectionUntilOnline(deviceId) {
        if (this.connectionRefreshes.has(deviceId)) return;

        const refreshToken = {};
        this.connectionRefreshes.set(deviceId, refreshToken);
        void (async () => {
            try {
                // BLE connection establishment continues after add_device
                // returns. Reconcile the runtime status for a bounded period
                // so the card changes from offline to online without reload.
                for (let attempt = 0; attempt < 12; attempt++) {
                    const device = state.connectedDevices.find(dev => dev.id === deviceId);
                    if (!device || device.status === 'online') return;

                    await new Promise(resolve => setTimeout(resolve, 1000));
                    if (this.connectionRefreshes.get(deviceId) !== refreshToken) return;
                    await this.load();
                }
            } finally {
                if (this.connectionRefreshes.get(deviceId) === refreshToken) {
                    this.connectionRefreshes.delete(deviceId);
                }
            }
        })();
    },

    renderGrid() {
        const grid = document.getElementById('devices-grid');
        const emptyState = document.getElementById('devices-empty');
        
        grid.innerHTML = '';
        
        if (state.connectedDevices.length === 0) {
            grid.classList.add('hidden');
            emptyState.classList.remove('hidden');
            emptyState.classList.add('flex');
            return;
        }
        
        emptyState.classList.add('hidden');
        emptyState.classList.remove('flex');
        grid.classList.remove('hidden');

        state.connectedDevices.forEach(dev => {
            const card = document.createElement('div');
            card.className = "bg-white rounded-xl border border-gray-200 p-5 shadow-sm hover:shadow-md transition-all relative overflow-hidden group cursor-pointer hover:border-brand-300";
            card.onclick = () => this.openDetailView(dev);
            
            // Status indicator color
            const statusColor = dev.status === 'online' ? 'bg-green-500' : 'bg-gray-400';
            const typeIconMap = {
                'sensor': 'ph-thermometer text-orange-500 bg-orange-50',
                'actuator': 'ph-plugs-connected text-blue-500 bg-blue-50',
                'beacon': 'ph-broadcast text-purple-500 bg-purple-50',
                'other': 'ph-bluetooth text-gray-500 bg-gray-100'
            };
            const iconClass = typeIconMap[dev.type] || typeIconMap['other'];
            const safeName = escapeHtml(dev.customName);
            const safeMac = escapeHtml(dev.mac);

            card.innerHTML = `
                <!-- Colored top accent -->
                <div class="absolute top-0 left-0 w-full h-1 ${dev.status === 'online' ? 'bg-green-400' : 'bg-gray-200'}"></div>
                
                <div class="flex justify-between items-start mb-4">
                    <div class="flex items-center">
                        <div class="w-10 h-10 rounded-lg flex items-center justify-center mr-3 ${iconClass.split(' ').slice(1).join(' ')}">
                            <i class="ph ${iconClass.split(' ')[0]} text-2xl"></i>
                        </div>
                        <div>
                            <h3 class="font-bold text-gray-900 truncate pr-2 max-w-[150px]" title="${safeName}">${safeName}</h3>
                            <p class="text-xs text-gray-500 font-mono">${safeMac}</p>
                        </div>
                    </div>
                    
                    <div class="text-gray-300 group-hover:text-brand-500 transition-colors">
                        <i class="ph ph-caret-right text-xl"></i>
                    </div>
                </div>
                
                <div class="mt-4 pt-4 border-t border-gray-100">
                    <div class="flex justify-between items-center text-sm">
                        <span class="flex items-center text-gray-600">
                            <span class="w-2 h-2 rounded-full ${statusColor} mr-2"></span>
                            ${dev.status === 'online' ? 'Connected' : 'Offline'}
                        </span>
                        <span class="text-xs text-gray-400 capitalize">${escapeHtml(dev.type)}</span>
                    </div>
                </div>
            `;
            grid.appendChild(card);
        });
    },

    openDetailView(dev, updateRoute = true) {
        state.selectedDeviceDetail = dev;
        
        // Update UI elements
        document.getElementById('detail-name').innerText = dev.customName;
        document.getElementById('detail-mac').innerText = dev.mac;
        document.getElementById('detail-type').innerText = dev.type;
        document.getElementById('detail-header-type').innerText = dev.type;
        document.getElementById('detail-device-id').innerText = dev.id;
        document.getElementById('detail-ble-address').innerText = dev.mac;
        
        // Status
        const statusEl = document.getElementById('detail-status');
        if(dev.status === 'online') {
            statusEl.innerHTML = `<span class="w-2.5 h-2.5 rounded-full bg-green-500 mr-2 animate-pulse"></span> <span class="text-green-600">Online</span>`;
        } else {
            statusEl.innerHTML = `<span class="w-2.5 h-2.5 rounded-full bg-gray-400 mr-2"></span> <span class="text-gray-500">Offline</span>`;
        }

        // Icon
        const typeIconMap = {
            'sensor': 'ph-thermometer text-orange-500 bg-orange-50',
            'actuator': 'ph-plugs-connected text-blue-500 bg-blue-50',
            'beacon': 'ph-broadcast text-purple-500 bg-purple-50',
            'other': 'ph-bluetooth text-gray-500 bg-gray-100'
        };
        const iconClass = typeIconMap[dev.type] || typeIconMap['other'];
        const iconContainer = document.getElementById('detail-icon');
        iconContainer.className = `w-12 h-12 rounded-lg flex items-center justify-center mr-4 text-2xl flex-shrink-0 ${iconClass.split(' ').slice(1).join(' ')}`;
        iconContainer.innerHTML = `<i class="ph ${iconClass.split(' ')[0]}"></i>`;

        // Load Logs (device log panel removed)
        this.loadCapabilities(dev);

        // Switch view
        nav.switchTab('device-detail', updateRoute);
    },

    async saveEdit(event) {
        if(event) event.preventDefault();
        if(!state.selectedDeviceDetail) return;
        
        const newName = document.getElementById('input-edit-name').value.trim();
        const newType = document.getElementById('input-edit-type').value;
        const btn = document.getElementById('btn-save-edit');
        
        if(!newName) {
            ui.showToast("Name cannot be empty", "error");
            document.getElementById('input-edit-name').focus();
            return;
        }

        if(newName === state.selectedDeviceDetail.customName && newType === state.selectedDeviceDetail.type) {
            ui.closeEditModal();
            return;
        }

        const originalHtml = btn.innerHTML;
        btn.innerHTML = `<i class="ph ph-spinner animate-spin mr-1.5 text-lg"></i> Saving...`;
        btn.disabled = true;
        btn.classList.add('opacity-80', 'cursor-not-allowed');

        try {
            await api.updateDevice(state.selectedDeviceDetail.id, { customName: newName, type: newType });
            
            // Update local state
            const devIndex = state.connectedDevices.findIndex(d => d.id === state.selectedDeviceDetail.id);
            if(devIndex > -1) {
                state.connectedDevices[devIndex].customName = newName;
                state.connectedDevices[devIndex].type = newType;
                
                // Update current detail view
                this.openDetailView(state.connectedDevices[devIndex]);
                
                ui.showToast("Device updated", "success");
                ui.closeEditModal();
            }
        } catch(e) {
            ui.showToast(`Failed to update device: ${e.message}`, "error");
        } finally {
            btn.innerHTML = originalHtml;
            btn.disabled = false;
            btn.classList.remove('opacity-80', 'cursor-not-allowed');
        }
    },

    confirmDeleteDetail() {
        if(!state.selectedDeviceDetail) return;
        this.remove(state.selectedDeviceDetail.id, true);
    },

    async sendToggle() {
        if (!state.selectedDeviceDetail) return;
        await this.sendCommand('toggle', 'boolean', true);
    },

    async sendCustomCommand() {
        const command = document.getElementById('input-device-command').value.trim();
        if (!command) {
            ui.showToast('Command cannot be empty', 'error');
            return;
        }
        await this.sendCommand(command, 'boolean', true);
    },

    async sendCommand(command, valueType = 'none', value = null) {
        const device = state.selectedDeviceDetail;
        if (!device) return;
        try {
            const result = await api.sendCommand(device.id, command, valueType, value);
            ui.showToast(result.message || `Command ${command} completed`, 'success');
        } catch (error) {
            ui.showToast(error.message, 'error');
        }
    },

    async refreshCapabilities() {
        const device = state.selectedDeviceDetail;
        if (!device) return;
        try {
            await api.refreshCapabilities(device.id);
            ui.showToast('Capability refresh queued', 'success');
            setTimeout(() => this.loadCapabilities(device), 2500);
        } catch (error) {
            ui.showToast(error.message, 'error');
        }
    },

    async loadCapabilities(device) {
        const loadId = ++this.capabilitiesLoadId;
        mcpTools.loadDevice(device.id);
        const stateText = document.getElementById('capability-state');
        const controls = document.getElementById('capability-controls');
        const legacy = document.getElementById('legacy-command-controls');
        controls.replaceChildren();
        stateText.textContent = 'Loading capabilities...';
        try {
            const response = await api.getCapabilities(device.id);
            // Opening a detail view also updates the URL hash, which
            // can trigger a second route restore and request. Only the
            // latest request may render, otherwise both responses
            // append the same command controls.
            if (loadId !== this.capabilitiesLoadId ||
                state.selectedDeviceDetail?.id !== device.id) return;
            const snapshot = response.data || {};
            stateText.textContent = `Capability state: ${snapshot.state || 'unknown'}`;
            const commands = Array.isArray(snapshot.commands) ? snapshot.commands : [];
            legacy.classList.toggle('hidden', commands.length > 0 && snapshot.state === 'ready');
            commands.forEach(capability => {
                const row = document.createElement('div');
                row.className = 'p-4 rounded-lg border border-gray-200 bg-gray-50 space-y-2';
                const label = document.createElement('label');
                label.className = 'block text-sm font-medium text-gray-700';
                label.textContent = capability.label || capability.name;
                row.appendChild(label);

                if (capability.value_type === 'integer') {
                    const wrapper = document.createElement('div');
                    wrapper.className = 'flex gap-2';
                    const input = document.createElement('input');
                    input.type = 'number';
                    input.min = capability.minimum;
                    input.max = capability.maximum;
                    input.step = capability.step || 1;
                    input.value = capability.minimum;
                    input.className = 'min-w-0 flex-1 px-3 py-2 border border-gray-300 rounded-lg text-sm';
                    const button = document.createElement('button');
                    button.className = 'px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm';
                    button.textContent = capability.unit ? `Set ${capability.unit}` : 'Set';
                    button.onclick = () => this.sendCommand(capability.name, 'integer', input.value);
                    wrapper.append(input, button);
                    row.appendChild(wrapper);
                } else if (capability.value_type === 'boolean') {
                    const wrapper = document.createElement('div');
                    wrapper.className = 'grid grid-cols-2 gap-2';
                    ['On', 'Off'].forEach((text, index) => {
                        const button = document.createElement('button');
                        button.className = 'px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm';
                        button.textContent = text;
                        button.onclick = () => this.sendCommand(capability.name, 'boolean', index === 0);
                        wrapper.appendChild(button);
                    });
                    row.appendChild(wrapper);
                } else {
                    const button = document.createElement('button');
                    button.className = 'w-full px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm';
                    button.textContent = capability.destructive ? 'Run (confirmation required)' : 'Run';
                    button.onclick = () => {
                        if (!capability.destructive || confirm(`Run ${capability.name}?`)) {
                            this.sendCommand(capability.name);
                        }
                    };
                    row.appendChild(button);
                }
                controls.appendChild(row);
            });
        } catch (error) {
            if (loadId !== this.capabilitiesLoadId ||
                state.selectedDeviceDetail?.id !== device.id) return;
            stateText.textContent = `Capabilities unavailable: ${error.message}`;
            legacy.classList.remove('hidden');
        }
    },

    async addDeviceFromModal() {
        if (!state.selectedDeviceForAdd) return;

        const nameInput = document.getElementById('input-custom-name').value.trim();
        const typeInput = document.getElementById('input-device-type').value;
        const btn = document.getElementById('btn-confirm-add');
        
        if(!nameInput) {
            ui.showToast("Please enter a custom name.", "error");
            document.getElementById('input-custom-name').focus();
            return;
        }

        // UI Loading state
        const originalHtml = btn.innerHTML;
        btn.innerHTML = `<i class="ph ph-spinner animate-spin mr-1.5 text-lg"></i> Provisioning...`;
        btn.disabled = true;
        btn.classList.add('opacity-80', 'cursor-not-allowed');

        const newDevice = {
            id: state.selectedDeviceForAdd.mac,
            mac: state.selectedDeviceForAdd.mac,
            addrType: state.selectedDeviceForAdd.addrType,
            customName: nameInput,
            type: typeInput,
            rssi: state.selectedDeviceForAdd.rssi,
            status: 'offline'
        };

        try {
            ui.showToast(`Adding ${newDevice.customName}...`, "info");

            // NimBLE cannot start a connection while discovery is active.
            // End the scan first so add_device can connect immediately
            // instead of waiting for the reconnect supervisor.
            if (state.isScanning) await scanner.stopScan();

            // Call backend API
            await api.addDevice(newDevice);
            
            // Update local state immediately so an active scan cannot add
            // the newly connected device back before the device list reloads.
            state.scannedDevices = state.scannedDevices.filter(d => d.mac !== newDevice.mac);
            if (!state.connectedDevices.some(d => d.mac === newDevice.mac)) {
                state.connectedDevices.push(newDevice);
            }
            
            ui.showToast(`Added ${newDevice.customName} successfully`, "success");
            
            ui.closeModal();
            
            // Always remove the item, including when scanning has stopped
            // and this was the final result in the list.
            const el = document.getElementById(`scanned-${newDevice.mac.replace(/:/g, '')}`);
            if(el) {
                el.classList.add('scale-95', 'opacity-0');
                setTimeout(() => el.remove(), 200);
            }
            
            await this.load();
            this.refreshConnectionUntilOnline(newDevice.id);

        } catch(e) {
            ui.showToast(`Failed to add device: ${e.message}`, "error");
        } finally {
            // Reset button state
            btn.innerHTML = originalHtml;
            btn.disabled = false;
            btn.classList.remove('opacity-80', 'cursor-not-allowed');
        }
    },

    async remove(deviceId, fromDetailView = false) {
        if(!confirm("Are you sure you want to remove this device from the gateway?")) return;
        
        try {
            await api.removeDevice(deviceId);
            this.connectionRefreshes.delete(deviceId);
            state.connectedDevices = state.connectedDevices.filter(d => d.id !== deviceId);

            ui.showToast("Device removed", "info");
            
            if(fromDetailView) {
                nav.switchTab('devices');
            } else {
                this.renderGrid();
            }
            
        } catch(e) {
            ui.showToast("Failed to remove device", "error");
        }
    }
};
