// --- Connected Devices Logic ---
const devices = {
    schemaLoadId: 0,
    currentSchemaState: 'unknown',
    currentFeatures: [],
    currentTools: [],
    loadPromise: null,
    _eventsInitialized: false,
    _deviceResyncTimer: null,
    _syncPromise: null,
    _syncRequested: false,
    _pendingSchemaRefresh: null,

    _initEvents() {
        if (this._eventsInitialized) return;
        this._eventsInitialized = true;

        events.on('device.connection', (ev) => {
            this._applyConnectionEvent(ev);
        });

        events.on('device.changed', (ev) => {
            this._scheduleDeviceResync();
        });

        events.on('feature.state', (ev) => {
            this._handleFeatureStateEvent(ev);
        });

        events.on('device.schema', (ev) => {
            this._handleSchemaEvent(ev);
        });

        events.on('resync:required', () => {
            this._syncFromSnapshot('resync');
        });

        events.on('ws:connected', () => {
            this._syncFromSnapshot('ws:connected');
        });

        events.on('ws:disconnected', () => {
            ui.setRealtimeBanner('show');
        });
    },

    async _syncFromSnapshot(reason) {
        if (this._syncPromise) {
            this._syncRequested = true;
            return;
        }

        this._syncRequested = false;
        const doSync = async () => {
            try {
                const snapshot = await api.getDevicesSnapshot();
                this._applyDeviceSnapshot(snapshot.devices);
                events.goLive(snapshot.eventSeq);
                ui.setRealtimeBanner('hide');
            } catch (e) {
                // Will retry on reconnect
            }
        };

        this._syncPromise = doSync();
        try {
            await this._syncPromise;
        } finally {
            this._syncPromise = null;
            if (this._syncRequested) {
                this._syncFromSnapshot('queued');
            }
        }
    },

    _applyDeviceSnapshot(devicesSnapshot) {
        const selectedId = state.selectedDeviceDetail?.id ?? null;

        state.connectedDevices = devicesSnapshot;
        state.devicesLoaded = true;

        if (selectedId) {
            const selected = state.connectedDevices.find(
                item => item.id === selectedId
            );

            if (selected) {
                state.selectedDeviceDetail = selected;
                this.renderConnectionState(selected);
                this.renderDeviceHeader(selected);
            } else {
                // Device was removed
                state.selectedDeviceDetail = null;
                nav.switchTab('devices');
            }
        }

        // Reconcile pending open device
        if (state.pendingOpenDeviceId) {
            const pending = state.connectedDevices.find(
                d => d.id === state.pendingOpenDeviceId
            );
            if (pending) {
                state.pendingOpenDeviceId = null;
                this.openDetailView(pending);
            }
        }

        this.renderGrid();

        // Notify scanner to reconcile managed devices
        if (typeof scanner !== 'undefined' && scanner.reconcileManagedDevices) {
            scanner.reconcileManagedDevices();
        }
    },

    _applyConnectionEvent(ev) {
        const dev = state.connectedDevices.find(d => d.id === ev.deviceId);
        if (dev) {
            // device.connection is emitted by on_device_ready()/disconnect,
            // so connected=true means command-usable GATT READY.  The
            // intermediate CONNECTING state only comes from REST snapshots
            // where connected=true and ready=false.
            const ready = ev.connected === true;
            dev.connected = ready;
            dev.ready = ready;
            dev.status = ready ? 'online' : 'offline';
            this.renderGrid();
            // Update detail view if open for this device
            if (state.selectedDeviceDetail && state.selectedDeviceDetail.id === ev.deviceId) {
                state.selectedDeviceDetail.connected = ready;
                state.selectedDeviceDetail.ready = ready;
                state.selectedDeviceDetail.status = dev.status;
                this.renderConnectionState(state.selectedDeviceDetail);
                if (this.currentFeatures.length > 0) {
                    this.renderFeatures(
                        this.currentFeatures,
                        this.currentTools,
                        state.selectedDeviceDetail);
                }
                document.getElementById('feature-offline-notice').classList.toggle(
                    'hidden', ready);
            }
        }
    },

    _handleFeatureStateEvent(ev) {
        // Cache feature state for all devices
        const deviceMap = state.featureStateByDevice;
        if (!deviceMap.has(ev.deviceId)) {
            deviceMap.set(ev.deviceId, new Map());
        }
        const featureMap = deviceMap.get(ev.deviceId);
        const key = `${ev.featureId}:${ev.propertyId}`;
        featureMap.set(key, {
            valueType: ev.valueType,
            value: ev.value,
            updatedAtMs: ev.updatedAtMs
        });

        // Update visible controls only if this is the selected device
        if (!state.selectedDeviceDetail) return;
        if (state.selectedDeviceDetail.id !== ev.deviceId) return;

        for (const feat of this.currentFeatures) {
            if (feat.feature_id === ev.featureId &&
                feat.property_id === ev.propertyId) {
                if (!feat.state) feat.state = {};
                feat.state.valid = true;
                if (ev.valueType === 'bool') feat.state.value_bool = ev.value;
                else if (ev.valueType === 'int') feat.state.value_int = ev.value;
                feat.state.updated_at_ms = ev.updatedAtMs;
                this.renderFeatures(this.currentFeatures, this.currentTools, state.selectedDeviceDetail);
                break;
            }
        }
    },

    _handleSchemaEvent(ev) {
        // Cache schema revision
        state.schemaRevisionByDevice.set(ev.deviceId, ev.revision);

        if (!state.selectedDeviceDetail) return;
        if (state.selectedDeviceDetail.id !== ev.deviceId) return;

        // Schema changed for selected device — reload schema once
        this.loadSchema(state.selectedDeviceDetail, true);
    },

    _scheduleDeviceResync() {
        if (this._deviceResyncTimer) return;
        this._deviceResyncTimer = setTimeout(() => {
            this._deviceResyncTimer = null;
            void this._syncFromSnapshot('device.changed');
        }, 75);
    },

    renderDeviceHeader(dev) {
        document.getElementById('detail-name').textContent = dev.customName;
        document.getElementById('detail-mac').textContent = dev.mac;
    },

    async load() {
        this._initEvents();
        events.init();

        if (this.loadPromise) return this.loadPromise;

        const loadOperation = this._initialLoad();
        this.loadPromise = loadOperation;
        try {
            return await loadOperation;
        } finally {
            if (this.loadPromise === loadOperation) this.loadPromise = null;
        }
    },

    async _initialLoad() {
        try {
            const snapshot = await api.getDevicesSnapshot();
            this._applyDeviceSnapshot(snapshot.devices);
            events.goLive(snapshot.eventSeq);
            return true;
        } catch(e) {
            // REST snapshot failed — will retry on ws:connected
            return false;
        }
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
            const statusStyles = {
                online: ['bg-green-500', 'bg-green-400', 'Connected'],
                connecting: ['bg-yellow-500', 'bg-yellow-400', 'Connecting…'],
                offline: ['bg-gray-400', 'bg-gray-200', 'Offline']
            };
            const st = statusStyles[dev.status] || statusStyles.offline;
            const safeName = escapeHtml(dev.customName);
            const safeMac = escapeHtml(dev.mac);

            card.innerHTML = `
                <!-- Colored top accent -->
                <div class="absolute top-0 left-0 w-full h-1 ${st[1]}"></div>

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
                            <span class="w-2 h-2 rounded-full ${st[0]} mr-2"></span>
                            ${st[2]}
                        </span>
                    </div>
                </div>
            `;
            grid.appendChild(card);
        });
    },

    openDetailView(dev, updateRoute = true) {
        state.selectedDeviceDetail = dev;
        this.currentFeatures = [];
        this.currentTools = [];
        document.getElementById('device-advanced-section').open = false;
        this.renderDeviceHeader(dev);

        const duplicateIdentifier = dev.id === dev.mac;
        document.getElementById('detail-identifier-row').classList.toggle('hidden', !duplicateIdentifier);
        document.getElementById('detail-device-id-row').classList.toggle('hidden', duplicateIdentifier);
        document.getElementById('detail-ble-address-row').classList.toggle('hidden', duplicateIdentifier);
        document.getElementById('detail-identifier').textContent = dev.id;
        document.getElementById('detail-device-id').textContent = dev.id;
        document.getElementById('detail-ble-address').textContent = dev.mac;

        this.renderConnectionState(dev);
        this.renderSchemaState('loading');
        document.getElementById('feature-offline-notice').classList.toggle(
            'hidden', dev.status === 'online');

        // Icon
        const iconContainer = document.getElementById('detail-icon');
        iconContainer.className = 'w-12 h-12 rounded-lg flex items-center justify-center mr-4 text-2xl flex-shrink-0 bg-gray-100';
        iconContainer.innerHTML = '<i class="ph ph-bluetooth text-gray-500"></i>';

        i18n.applyTranslations();
        nav.switchTab('device-detail', updateRoute);
        this.loadSchema(dev);
    },

    renderConnectionState(device) {
        const status = device.status || 'offline';
        const styles = {
            online: ['bg-green-500', 'text-green-600', 'device_detail.online'],
            connecting: ['bg-yellow-500', 'text-yellow-600', 'device_detail.connecting'],
            offline: ['bg-gray-400', 'text-gray-500', 'device_detail.offline']
        };
        const s = styles[status] || styles.offline;
        const markup = `<span class="w-2.5 h-2.5 rounded-full ${s[0]} mr-2"></span><span class="${s[1]}">${i18n.t(s[2])}</span>`;
        document.getElementById('detail-status').innerHTML = markup;
        document.getElementById('detail-summary-connection').innerHTML = markup;
    },

    renderSchemaState(schemaState) {
        const normalized = schemaState === 'discovering' ? 'loading' : (schemaState || 'unknown');
        this.currentSchemaState = normalized;
        const styles = {
            ready: ['bg-green-500', 'text-green-700'],
            loading: ['bg-blue-500', 'text-blue-700'],
            stale: ['bg-amber-500', 'text-amber-700'],
            error: ['bg-red-500', 'text-red-700'],
            unknown: ['bg-gray-400', 'text-gray-600']
        };
        const style = styles[normalized] || styles.unknown;
        const key = styles[normalized] ? normalized : 'unknown';
        document.getElementById('detail-summary-schema').innerHTML = `<span class="inline-flex items-center"><span class="w-2.5 h-2.5 rounded-full ${style[0]} mr-2"></span><span class="${style[1]}">${i18n.t(`device_detail.schema_${key}`)}</span></span>`;
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

            // Optimistic local header update
            state.selectedDeviceDetail.customName = newName;
            this.renderDeviceHeader(state.selectedDeviceDetail);
            
            // device.changed will provide authoritative snapshot
            ui.showToast(i18n.t('device_detail.updated'), "success");
            ui.closeEditModal();
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

    async sendToggle(feature) {
        if (!state.selectedDeviceDetail || !feature) return;
        const command = feature.write_command || 'toggle';
        const valueType = feature.property_id === 1 ? 'boolean' : 'none';
        const value = feature.state && feature.state.valid ? !feature.state.value_bool : true;
        await this.sendCommand(command, valueType, value);
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

    async refreshSchema() {
        const device = state.selectedDeviceDetail;
        if (!device) return;
        const button = document.getElementById('btn-refresh-schema');
        const label = button.querySelector('span');
        button.disabled = true;
        button.querySelector('i').classList.add('animate-spin');
        label.textContent = i18n.t('device_detail.refreshing');
        this.renderSchemaState('loading');
        this._pendingSchemaRefresh = device.id;
        try {
            await api.refreshDeviceSchema(device.id);
            // device.schema WS event will trigger loadSchema; wait for event
            // delivery with a UX-only timeout, not a polling fallback.
            const timeoutPromise = new Promise(resolve => setTimeout(resolve, 15000));
            const schemaPromise = new Promise(resolve => {
                const handler = (ev) => {
                    if (ev.deviceId === device.id) {
                        events.off('device.schema', handler);
                        resolve(true);
                    }
                };
                events.on('device.schema', handler);
                // Also resolve if resync happens
                const resyncHandler = () => {
                    events.off('resync:required', resyncHandler);
                    resolve(true);
                };
                events.on('resync:required', resyncHandler);
            });
            await Promise.race([schemaPromise, timeoutPromise]);
            events.off('device.schema', () => {});
            events.off('resync:required', () => {});
            this._pendingSchemaRefresh = null;
            const loaded = await this.loadSchema(device, true);
            if (loaded) ui.showToast(i18n.t('device_detail.schema_updated'), 'success');
            else ui.showToast(i18n.t('device_detail.schema_load_error'), 'error');
        } catch (error) {
            ui.showToast(error.message, 'error');
            this.renderSchemaState('error');
        } finally {
            this._pendingSchemaRefresh = null;
            button.disabled = false;
            button.querySelector('i').classList.remove('animate-spin');
            label.textContent = i18n.t('device_detail.refresh');
        }
    },

    async loadSchema(device, syncMcpAfterLoad = false) {
        const loadId = ++this.schemaLoadId;
        const container = document.getElementById('feature-cards');
        this.currentFeatures = [];
        this.currentTools = [];
        container.replaceChildren();
        this.renderSchemaState('loading');
        this.renderFeaturesLoading(container);
        if (!syncMcpAfterLoad) void mcpTools.loadDevice(device.id);
        try {
            const snapshot = await api.getDeviceSchema(device.id);
            if (loadId !== this.schemaLoadId ||
                state.selectedDeviceDetail?.id !== device.id) return;
            this.renderSchemaState(snapshot.state);
            const features = Array.isArray(snapshot.features) ? snapshot.features : [];
            const tools = Array.isArray(snapshot.tools) ? snapshot.tools : [];
            this.currentFeatures = features;
            this.currentTools = tools;
            this.renderFeatures(features, tools, device);
            if (syncMcpAfterLoad) await mcpTools.loadDevice(device.id);
            return true;
        } catch (error) {
            if (loadId !== this.schemaLoadId ||
                state.selectedDeviceDetail?.id !== device.id) return;
            this.renderSchemaState('error');
            this.currentFeatures = [];
            this.currentTools = [];
            this.renderFeaturesError(container, error, device);
            return false;
        }
    },

    renderFeaturesLoading(container) {
        const loading = document.createElement('div');
        loading.className = 'rounded-lg border border-gray-200 bg-gray-50 p-5 text-sm text-gray-500';
        loading.innerHTML = `<i class="ph ph-spinner animate-spin mr-2"></i>${i18n.t('device_detail.loading_features')}`;
        container.appendChild(loading);
    },

    renderFeatures(features, tools, device) {
        const container = document.getElementById('feature-cards');
        container.replaceChildren();
        if (!features.length) {
            this.renderFeaturesEmptyState(container, device);
            return;
        }
        features.forEach(feature =>
            container.appendChild(this.renderFeatureCard(feature, tools, device)));
    },

    renderFeatureCard(feature, tools, device) {
        const card = document.createElement('div');
        card.className = 'rounded-lg border border-gray-200 bg-gray-50 p-4';

        const header = document.createElement('div');
        header.className = 'flex items-center justify-between mb-3';
        const titleGroup = document.createElement('div');
        titleGroup.className = 'flex items-center gap-2';

        const featureId = document.createElement('span');
        featureId.className = 'text-sm font-semibold text-gray-800';
        featureId.textContent = feature.feature_id;
        titleGroup.appendChild(featureId);

        if (feature.template && feature.template.semantic_name) {
            const badge = document.createElement('span');
            badge.className = 'inline-flex items-center rounded-full bg-blue-100 px-2 py-0.5 text-xs font-semibold text-blue-700';
            badge.textContent = feature.template.semantic_name;
            titleGroup.appendChild(badge);
        } else {
            const badge = document.createElement('span');
            badge.className = 'inline-flex items-center rounded-full bg-gray-100 px-2 py-0.5 text-xs font-semibold text-gray-500';
            badge.textContent = i18n.t('device_detail.unsupported');
            titleGroup.appendChild(badge);
        }
        header.appendChild(titleGroup);

        /* Feature state indicator */
        if (feature.state && feature.state.valid) {
            const stateText = document.createElement('span');
            stateText.className = 'text-xs text-gray-500 font-mono';
            stateText.textContent = feature.state.value_bool ? 'ON' : (feature.state.value_int || '');
            header.appendChild(stateText);
        }
        card.appendChild(header);

        /* Control area */
        const control = document.createElement('div');
        control.className = 'flex flex-col sm:flex-row gap-2';

        if (feature.template && feature.write_command) {
            const tpl = feature.template;
            if (tpl.primary_property === 1) {
                /* ON/OFF property → toggle button */
                const isOn = feature.state && feature.state.valid && feature.state.value_bool;
                const toggleBtn = document.createElement('button');
                toggleBtn.type = 'button';
                toggleBtn.disabled = device.status !== 'online';
                toggleBtn.className = isOn
                    ? 'px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm font-medium disabled:opacity-50 disabled:cursor-not-allowed'
                    : 'px-4 py-2 bg-white border border-gray-300 text-gray-700 rounded-lg hover:bg-gray-100 transition-colors text-sm font-medium disabled:opacity-50 disabled:cursor-not-allowed';
                toggleBtn.textContent = isOn ? i18n.t('device_detail.turn_off') : i18n.t('device_detail.turn_on');
                toggleBtn.onclick = () => this.sendToggle(feature);
                control.appendChild(toggleBtn);
            } else if (tpl.primary_property === 2) {
                /* LEVEL property → slider or integer input */
                const tool = feature.writable_tool_index >= 0 ? tools[feature.writable_tool_index] : null;
                const min = tool ? tool.min_value : 0;
                const max = tool ? tool.max_value : 100;
                const step = tool ? tool.step : 1;
                const input = document.createElement('input');
                input.type = 'range';
                input.min = min;
                input.max = max;
                input.step = step;
                input.value = (feature.state && feature.state.valid) ? feature.state.value_int : min;
                input.disabled = device.status !== 'online';
                input.className = 'flex-1';
                const applyBtn = document.createElement('button');
                applyBtn.type = 'button';
                applyBtn.disabled = device.status !== 'online';
                applyBtn.className = 'px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm font-medium disabled:opacity-50 disabled:cursor-not-allowed';
                applyBtn.textContent = i18n.t('device_detail.apply');
                applyBtn.onclick = () => this.sendCommand(feature.write_command, 'integer', Number(input.value), [input, applyBtn]);
                control.append(input, applyBtn);
            } else {
                /* Unknown property → action button */
                const actionBtn = document.createElement('button');
                actionBtn.type = 'button';
                actionBtn.disabled = device.status !== 'online';
                actionBtn.className = 'w-full sm:w-auto px-4 py-2 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm font-medium flex-shrink-0 disabled:opacity-50 disabled:cursor-not-allowed';
                actionBtn.textContent = i18n.t('device_detail.run');
                actionBtn.onclick = () => this.sendCommand(feature.write_command, 'none', null, [actionBtn]);
                control.appendChild(actionBtn);
            }
        } else {
            /* No template or no write command → unsupported */
            const unsupported = document.createElement('span');
            unsupported.className = 'text-xs text-gray-400';
            unsupported.textContent = i18n.t('device_detail.no_write_tool');
            control.appendChild(unsupported);
        }
        card.appendChild(control);
        return card;
    },

    renderFeaturesEmptyState(container, device) {
        const empty = document.createElement('div');
        empty.className = 'rounded-lg border border-dashed border-gray-300 p-6 text-center';
        empty.innerHTML = `<i class="ph ph-plugs text-2xl text-gray-400"></i><h4 class="mt-2 text-sm font-semibold text-gray-800">${i18n.t('device_detail.no_features')}</h4><p class="mt-1 text-xs text-gray-500">${i18n.t('device_detail.no_features_desc')}</p>`;
        const retry = document.createElement('button');
        retry.className = 'mt-4 px-4 py-2 bg-brand-50 text-brand-700 rounded-lg hover:bg-brand-100 text-sm font-medium';
        retry.textContent = i18n.t('device_detail.refresh_schema');
        retry.onclick = () => this.refreshSchema(device);
        empty.appendChild(retry);
        container.appendChild(empty);
    },

    renderFeaturesError(container, error, device) {
        container.replaceChildren();
        const failure = document.createElement('div');
        failure.className = 'rounded-lg border border-red-200 bg-red-50 p-5';
        const title = document.createElement('h4');
        title.className = 'text-sm font-semibold text-red-800';
        title.textContent = i18n.t('device_detail.schema_load_error');
        const message = document.createElement('p');
        message.className = 'text-xs text-red-700 mt-1 break-words';
        message.textContent = error.message;
        const retry = document.createElement('button');
        retry.className = 'mt-3 px-3 py-2 bg-white border border-red-200 text-red-700 rounded-lg hover:bg-red-100 text-sm font-medium';
        retry.textContent = i18n.t('device_detail.retry');
        retry.onclick = () => this.loadSchema(device);
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
            if (state.isScanning) await scanner.stopScan();

            // Call backend API — do NOT re-enter load()
            await api.addDevice(newDevice);
            
            // Remove from scan results
            state.scannedDevices = state.scannedDevices.filter(d => d.mac !== newDevice.mac);
            
            ui.showToast(`Device saved. Connecting...`, "success");
            
            ui.closeModal();
            
            // Remove scan row
            const el = document.getElementById(`scanned-${newDevice.mac.replace(/:/g, '')}`);
            if(el) {
                el.classList.add('scale-95', 'opacity-0');
                setTimeout(() => el.remove(), 200);
            }
            
            // Set pending open — device.changed snapshot will open detail
            state.pendingOpenDeviceId = newDevice.id;

            // If WS is degraded, do one controlled REST recovery
            if (!events.connected || !events.live) {
                await this._syncFromSnapshot('local-add');
            }

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

            // Clear caches for deleted device
            state.featureStateByDevice.delete(deviceId);
            state.schemaRevisionByDevice.delete(deviceId);
            state.selectedDeviceDetail = null;
            this.currentFeatures = [];
            this.currentTools = [];
            this._pendingSchemaRefresh = null;

            ui.showToast(i18n.t('device_detail.removed'), "info");
            
            if(fromDetailView || state.selectedDeviceDetail?.id === deviceId) {
                nav.switchTab('devices');
            }
            
            // device.changed will provide authoritative snapshot
        } catch(e) {
            ui.showToast(i18n.t('device_detail.remove_failed'), "error");
        }
    }
};
