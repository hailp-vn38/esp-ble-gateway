// --- Connected Devices Logic ---
const devices = {
    capabilitiesLoadId: 0,
    currentCapabilityState: 'unknown',
    currentCapabilities: [],
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
            const safeName = escapeHtml(dev.customName);
            const safeMac = escapeHtml(dev.mac);

            card.innerHTML = `
                <!-- Colored top accent -->
                <div class="absolute top-0 left-0 w-full h-1 ${dev.status === 'online' ? 'bg-green-400' : 'bg-gray-200'}"></div>

                <div class="flex justify-between items-start mb-4">
                    <div class="flex items-center">
                        <div class="w-10 h-10 rounded-lg flex items-center justify-center mr-3 bg-gray-100">
                            <i class="ph ph-bluetooth text-gray-500 text-2xl"></i>
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
                    </div>
                </div>
            `;
            grid.appendChild(card);
        });
    },

    openDetailView(dev, updateRoute = true) {
        state.selectedDeviceDetail = dev;
        this.currentCapabilities = [];
        document.getElementById('device-advanced-section').open = false;
        document.getElementById('detail-name').textContent = dev.customName;
        document.getElementById('detail-mac').textContent = dev.mac;

        const duplicateIdentifier = dev.id === dev.mac;
        document.getElementById('detail-identifier-row').classList.toggle('hidden', !duplicateIdentifier);
        document.getElementById('detail-device-id-row').classList.toggle('hidden', duplicateIdentifier);
        document.getElementById('detail-ble-address-row').classList.toggle('hidden', duplicateIdentifier);
        document.getElementById('detail-identifier').textContent = dev.id;
        document.getElementById('detail-device-id').textContent = dev.id;
        document.getElementById('detail-ble-address').textContent = dev.mac;

        this.renderConnectionState(dev);
        this.renderCapabilityState('loading');
        document.getElementById('capability-offline-notice').classList.toggle(
            'hidden', dev.status === 'online');

        // Icon
        const iconContainer = document.getElementById('detail-icon');
        iconContainer.className = 'w-12 h-12 rounded-lg flex items-center justify-center mr-4 text-2xl flex-shrink-0 bg-gray-100';
        iconContainer.innerHTML = '<i class="ph ph-bluetooth text-gray-500"></i>';

        i18n.applyTranslations();
        nav.switchTab('device-detail', updateRoute);
        this.loadCapabilities(dev);
    },

    renderConnectionState(device) {
        const online = device.status === 'online';
        const markup = `<span class="w-2.5 h-2.5 rounded-full ${online ? 'bg-green-500' : 'bg-gray-400'} mr-2"></span><span class="${online ? 'text-green-600' : 'text-gray-500'}">${i18n.t(online ? 'device_detail.online' : 'device_detail.offline')}</span>`;
        document.getElementById('detail-status').innerHTML = markup;
        document.getElementById('detail-summary-connection').innerHTML = markup;
    },

    renderCapabilityState(capabilityState) {
        const normalized = capabilityState === 'discovering' ? 'loading' : (capabilityState || 'unknown');
        this.currentCapabilityState = normalized;
        const styles = {
            ready: ['bg-green-500', 'text-green-700'],
            loading: ['bg-blue-500', 'text-blue-700'],
            stale: ['bg-amber-500', 'text-amber-700'],
            error: ['bg-red-500', 'text-red-700'],
            unknown: ['bg-gray-400', 'text-gray-600']
        };
        const style = styles[normalized] || styles.unknown;
        const key = styles[normalized] ? normalized : 'unknown';
        document.getElementById('detail-summary-capabilities').innerHTML = `<span class="inline-flex items-center"><span class="w-2.5 h-2.5 rounded-full ${style[0]} mr-2"></span><span class="${style[1]}">${i18n.t(`device_detail.capability_${key}`)}</span></span>`;
    },

    async saveEdit(event) {
        if(event) event.preventDefault();
        if(!state.selectedDeviceDetail) return;
        
        const newName = document.getElementById('input-edit-name').value.trim();
        const btn = document.getElementById('btn-save-edit');

        if(!newName) {
            ui.showToast(i18n.t('device_detail.name_required'), "error");
            document.getElementById('input-edit-name').focus();
            return;
        }

        if(newName === state.selectedDeviceDetail.customName) {
            ui.closeEditModal();
            return;
        }

        const originalHtml = btn.innerHTML;
        btn.innerHTML = `<i class="ph ph-spinner animate-spin mr-1.5 text-lg"></i> ${i18n.t('device_detail.saving')}`;
        btn.disabled = true;
        btn.classList.add('opacity-80', 'cursor-not-allowed');

        try {
            await api.updateDevice(state.selectedDeviceDetail.id, { customName: newName });

            // Update local state
            const devIndex = state.connectedDevices.findIndex(d => d.id === state.selectedDeviceDetail.id);
            if(devIndex > -1) {
                state.connectedDevices[devIndex].customName = newName;

                // Update current detail view
                this.openDetailView(state.connectedDevices[devIndex]);
                
                ui.showToast(i18n.t('device_detail.updated'), "success");
                ui.closeEditModal();
            }
        } catch(e) {
            ui.showToast(`${i18n.t('device_detail.update_failed')}: ${e.message}`, "error");
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
            ui.showToast(i18n.t('device_detail.command_required'), 'error');
            return;
        }
        const valueType = document.getElementById('input-command-value-type').value;
        const rawValue = document.getElementById('input-command-value').value.trim();
        let value = null;
        if (valueType === 'boolean') {
            if (!['true', 'false'].includes(rawValue.toLowerCase())) {
                ui.showToast(i18n.t('device_detail.boolean_value_error'), 'error');
                return;
            }
            value = rawValue.toLowerCase() === 'true';
        } else if (valueType === 'integer') {
            if (!/^-?\d+$/.test(rawValue)) {
                ui.showToast(i18n.t('device_detail.integer_value_error'), 'error');
                return;
            }
            value = Number(rawValue);
        }
        await this.sendCommand(command, valueType, value);
    },

    updateCustomCommandValueInput() {
        const valueType = document.getElementById('input-command-value-type').value;
        const wrapper = document.getElementById('custom-command-value-wrapper');
        const input = document.getElementById('input-command-value');
        wrapper.classList.toggle('hidden', valueType === 'none');
        input.placeholder = valueType === 'boolean' ? 'true / false' : '';
        input.inputMode = valueType === 'integer' ? 'numeric' : 'text';
    },

    async sendCommand(command, valueType = 'none', value = null, controls = []) {
        const device = state.selectedDeviceDetail;
        if (!device) return;
        if (device.status !== 'online') {
            ui.showToast(i18n.t('device_detail.offline_command_error'), 'error');
            return;
        }
        controls.forEach(control => control.disabled = true);
        try {
            const result = await api.sendCommand(device.id, command, valueType, value);
            ui.showToast(result.message || i18n.t('device_detail.command_completed'), 'success');
        } catch (error) {
            ui.showToast(error.message, 'error');
        } finally {
            controls.forEach(control => control.disabled = false);
        }
    },

    async refreshCapabilities() {
        const device = state.selectedDeviceDetail;
        if (!device) return;
        const button = document.getElementById('btn-refresh-capabilities');
        const label = button.querySelector('span');
        button.disabled = true;
        button.querySelector('i').classList.add('animate-spin');
        label.textContent = i18n.t('device_detail.refreshing');
        this.renderCapabilityState('loading');
        try {
            await api.refreshCapabilities(device.id);
            await new Promise(resolve => setTimeout(resolve, 2500));
            const loaded = await this.loadCapabilities(device, true);
            if (loaded) ui.showToast(i18n.t('device_detail.capabilities_updated'), 'success');
        } catch (error) {
            ui.showToast(error.message, 'error');
            this.renderCapabilityState('error');
        } finally {
            button.disabled = false;
            button.querySelector('i').classList.remove('animate-spin');
            label.textContent = i18n.t('device_detail.refresh');
        }
    },

    async loadCapabilities(device, syncMcpAfterLoad = false) {
        const loadId = ++this.capabilitiesLoadId;
        const controls = document.getElementById('capability-controls');
        this.currentCapabilities = [];
        controls.replaceChildren();
        this.renderCapabilityState('loading');
        this.renderCapabilitiesLoading(controls);
        if (!syncMcpAfterLoad) void mcpTools.loadDevice(device.id);
        try {
            const response = await api.getCapabilities(device.id);
            // Opening a detail view also updates the URL hash, which
            // can trigger a second route restore and request. Only the
            // latest request may render, otherwise both responses
            // append the same command controls.
            if (loadId !== this.capabilitiesLoadId ||
                state.selectedDeviceDetail?.id !== device.id) return;
            const snapshot = response.data || {};
            this.renderCapabilityState(snapshot.state);
            const commands = Array.isArray(snapshot.commands) ? snapshot.commands : [];
            this.currentCapabilities = commands;
            this.renderCapabilities(commands, device);
            if (syncMcpAfterLoad) await mcpTools.loadDevice(device.id);
            return true;
        } catch (error) {
            if (loadId !== this.capabilitiesLoadId ||
                state.selectedDeviceDetail?.id !== device.id) return;
            this.renderCapabilityState('error');
            this.currentCapabilities = [];
            this.renderCapabilityError(controls, error, device);
            return false;
        }
    },

    renderCapabilitiesLoading(container) {
        const loading = document.createElement('div');
        loading.className = 'rounded-lg border border-gray-200 bg-gray-50 p-5 text-sm text-gray-500';
        loading.innerHTML = `<i class="ph ph-spinner animate-spin mr-2"></i>${i18n.t('device_detail.loading_capabilities')}`;
        container.appendChild(loading);
    },

    renderCapabilities(commands, device) {
        const controls = document.getElementById('capability-controls');
        controls.replaceChildren();
        if (!commands.length) {
            this.renderCapabilityEmptyState(controls, device);
            return;
        }
        commands.forEach(capability =>
            controls.appendChild(this.renderCapabilityControl(capability, device)));
    },

    renderCapabilityControl(capability, device) {
        const row = document.createElement('div');
        row.className = `flex flex-col md:flex-row md:items-center justify-between gap-4 p-4 rounded-lg border ${capability.destructive ? 'border-red-200 bg-red-50/50' : 'border-gray-200 bg-gray-50'}`;
        const description = document.createElement('div');
        description.className = 'min-w-0';
        const heading = document.createElement('div');
        heading.className = 'flex flex-wrap items-center gap-2';
        const label = document.createElement('h4');
        label.className = 'text-sm font-semibold text-gray-800 break-words';
        label.textContent = capability.label || capability.name;
        heading.appendChild(label);
        if (capability.destructive) {
            const badge = document.createElement('span');
            badge.className = 'inline-flex items-center rounded-full bg-red-100 px-2 py-0.5 text-xs font-semibold text-red-700';
            badge.textContent = `⚠ ${i18n.t('device_detail.destructive')}`;
            heading.appendChild(badge);
        }
        const copy = document.createElement('p');
        copy.className = 'text-xs text-gray-500 mt-1 break-words';
        copy.textContent = capability.description || capability.name;
        description.append(heading, copy);

        const control = capability.value_type === 'integer'
            ? this.renderIntegerControl(capability, device)
            : (capability.value_type === 'boolean'
                ? this.renderBooleanControl(capability, device)
                : this.renderActionControl(capability, device));
        row.append(description, control);
        return row;
    },

    renderBooleanControl(capability, device) {
        const wrapper = document.createElement('div');
        wrapper.className = 'grid grid-cols-2 gap-2 w-full sm:w-auto flex-shrink-0';
        const buttons = [true, false].map((value, index) => {
            const button = document.createElement('button');
            button.type = 'button';
            button.disabled = device.status !== 'online';
            button.className = index === 0
                ? 'px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm font-medium disabled:opacity-50 disabled:cursor-not-allowed'
                : 'px-4 py-2 bg-white border border-gray-300 text-gray-700 rounded-lg hover:bg-gray-100 transition-colors text-sm font-medium disabled:opacity-50 disabled:cursor-not-allowed';
            button.textContent = i18n.t(value ? 'device_detail.on' : 'device_detail.off');
            button.onclick = () => this.sendCommand(capability.name, 'boolean', value, buttons);
            return button;
        });
        wrapper.append(...buttons);
        return wrapper;
    },

    renderIntegerControl(capability, device) {
        const container = document.createElement('div');
        container.className = 'w-full md:w-auto flex-shrink-0';
        const wrapper = document.createElement('div');
        wrapper.className = 'flex flex-col sm:flex-row gap-2';
        const input = document.createElement('input');
        input.type = 'number';
        if (Number.isFinite(capability.minimum)) input.min = capability.minimum;
        if (Number.isFinite(capability.maximum)) input.max = capability.maximum;
        input.step = capability.step || 1;
        input.value = Number.isFinite(capability.minimum) ? capability.minimum : 0;
        input.disabled = device.status !== 'online';
        input.className = 'w-full sm:w-40 px-3 py-2 border border-gray-300 rounded-lg text-sm disabled:bg-gray-100';
        const button = document.createElement('button');
        button.type = 'button';
        button.disabled = device.status !== 'online';
        button.className = 'px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm font-medium disabled:opacity-50 disabled:cursor-not-allowed';
        button.textContent = i18n.t('device_detail.apply');
        button.onclick = () => this.sendCommand(capability.name, 'integer', input.value, [input, button]);
        wrapper.append(input);
        if (capability.unit) {
            const unit = document.createElement('span');
            unit.className = 'self-center text-sm text-gray-500';
            unit.textContent = capability.unit;
            wrapper.appendChild(unit);
        }
        wrapper.appendChild(button);
        container.appendChild(wrapper);
        const rangeParts = [];
        if (Number.isFinite(capability.minimum) && Number.isFinite(capability.maximum)) rangeParts.push(`${capability.minimum}–${capability.maximum}`);
        if (capability.step) rangeParts.push(`${i18n.t('device_detail.step')}: ${capability.step}`);
        if (rangeParts.length) {
            const range = document.createElement('p');
            range.className = 'text-xs text-gray-400 mt-1';
            range.textContent = rangeParts.join(' · ');
            container.appendChild(range);
        }
        return container;
    },

    renderActionControl(capability, device) {
        const button = document.createElement('button');
        button.type = 'button';
        button.disabled = device.status !== 'online';
        button.className = capability.destructive
            ? 'w-full md:w-auto px-4 py-2 bg-white border border-red-300 text-red-700 rounded-lg hover:bg-red-100 transition-colors text-sm font-medium flex-shrink-0 disabled:opacity-50 disabled:cursor-not-allowed'
            : 'w-full md:w-auto px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm font-medium flex-shrink-0 disabled:opacity-50 disabled:cursor-not-allowed';
        button.textContent = capability.destructive
            ? i18n.t('device_detail.run_confirm')
            : i18n.t('device_detail.run');
        button.onclick = () => {
            const label = capability.label || capability.name;
            if (!capability.destructive || confirm(i18n.t('device_detail.run_destructive_confirm').replace('{name}', label))) {
                this.sendCommand(capability.name, 'none', null, [button]);
            }
        };
        return button;
    },

    renderCapabilityEmptyState(container, device) {
        const empty = document.createElement('div');
        empty.className = 'rounded-lg border border-dashed border-gray-300 p-6 text-center';
        empty.innerHTML = `<i class="ph ph-plugs text-2xl text-gray-400"></i><h4 class="mt-2 text-sm font-semibold text-gray-800">${i18n.t('device_detail.no_capabilities')}</h4><p class="mt-1 text-xs text-gray-500">${i18n.t('device_detail.no_capabilities_desc')}</p>`;
        const retry = document.createElement('button');
        retry.className = 'mt-4 px-4 py-2 bg-brand-50 text-brand-700 rounded-lg hover:bg-brand-100 text-sm font-medium';
        retry.textContent = i18n.t('device_detail.refresh_capabilities');
        retry.onclick = () => this.refreshCapabilities(device);
        empty.appendChild(retry);
        container.appendChild(empty);
    },

    renderCapabilityError(container, error, device) {
        container.replaceChildren();
        const failure = document.createElement('div');
        failure.className = 'rounded-lg border border-red-200 bg-red-50 p-5';
        const title = document.createElement('h4');
        title.className = 'text-sm font-semibold text-red-800';
        title.textContent = i18n.t('device_detail.capabilities_load_error');
        const message = document.createElement('p');
        message.className = 'text-xs text-red-700 mt-1 break-words';
        message.textContent = error.message;
        const retry = document.createElement('button');
        retry.className = 'mt-3 px-3 py-2 bg-white border border-red-200 text-red-700 rounded-lg hover:bg-red-100 text-sm font-medium';
        retry.textContent = i18n.t('device_detail.retry');
        retry.onclick = () => this.loadCapabilities(device);
        failure.append(title, message, retry);
        container.appendChild(failure);
    },

    async addDeviceFromModal() {
        if (!state.selectedDeviceForAdd) return;

        const nameInput = document.getElementById('input-custom-name').value.trim();
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
        const device = state.connectedDevices.find(dev => dev.id === deviceId);
        const name = device?.customName || deviceId;
        let message = i18n.t('device_detail.remove_confirm').replace('{name}', name);
        if ((mcpTools.enabledByDevice.get(deviceId) || 0) > 0) {
            message += `\n\n${i18n.t('device_detail.remove_mcp_warning')}`;
        }
        if(!confirm(message)) return;
        
        try {
            await api.removeDevice(deviceId);
            this.connectionRefreshes.delete(deviceId);
            state.connectedDevices = state.connectedDevices.filter(d => d.id !== deviceId);

            ui.showToast(i18n.t('device_detail.removed'), "info");
            
            if(fromDetailView) {
                nav.switchTab('devices');
            } else {
                this.renderGrid();
            }
            
        } catch(e) {
            ui.showToast(i18n.t('device_detail.remove_failed'), "error");
        }
    }
};
