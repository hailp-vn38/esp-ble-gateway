// --- MCP feature permission controls ---
const mcpControls = {
    loadId: 0,
    enabledByDevice: new Map(),

    async loadExposures(deviceId) {
        return api.request(`/api/mcp/exposures?device_id=${encodeURIComponent(deviceId)}`);
    },

    async toggleExposure(deviceId, featureId, enable) {
        const payload = {device_id: deviceId, feature_id: featureId, enabled: enable};
        return api.request('/api/mcp/exposures', {
            method: 'PUT',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(payload)
        });
    },

    renderExposureRow(deviceId, feature) {
        const enabled = feature.control_enabled === true && feature.health === 'enabled';
        const row = document.createElement('div');
        row.className = 'flex flex-col sm:flex-row sm:items-center justify-between gap-4 p-5';

        const identity = document.createElement('div');
        identity.className = 'min-w-0';
        const heading = document.createElement('div');
        heading.className = 'flex flex-wrap items-center gap-2';
        const label = document.createElement('h4');
        label.className = 'text-sm font-semibold text-gray-800 break-words';
        label.textContent = feature.semantic_name || feature.feature_id;
        heading.appendChild(label);
        if (feature.semantic_name) {
            const badge = document.createElement('span');
            badge.className = 'inline-flex rounded-full bg-blue-100 px-2 py-0.5 text-xs font-semibold text-blue-700';
            badge.textContent = feature.property || feature.value_type || 'feature';
            heading.appendChild(badge);
        }
        const featureId = document.createElement('p');
        featureId.className = 'mt-1 text-xs font-mono text-gray-600 break-all';
        featureId.textContent = feature.feature_id;
        const description = document.createElement('p');
        description.className = 'mt-1 text-xs text-gray-500';
        description.textContent = i18n.t('device_detail.mcp_row_desc');
        identity.append(heading, featureId, description);

        const control = document.createElement('div');
        control.className = 'flex items-center justify-between sm:justify-end gap-3 flex-shrink-0';
        const stateLabel = document.createElement('span');
        stateLabel.className = enabled ? 'text-xs font-semibold text-brand-700' : 'text-xs font-semibold text-gray-500';
        stateLabel.textContent = i18n.t(enabled ? 'device_detail.mcp_enabled' : 'device_detail.mcp_disabled');
        const switchLabel = document.createElement('label');
        switchLabel.className = 'settings-switch';
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.checked = enabled;
        input.setAttribute('role', 'switch');
        input.setAttribute('aria-checked', String(enabled));
        input.setAttribute('aria-label', `${feature.semantic_name || feature.feature_id}: ${stateLabel.textContent}`);
        const track = document.createElement('span');
        track.className = 'settings-switch-track';
        input.onchange = () => this.onToggle(deviceId, feature, input, stateLabel);
        switchLabel.append(input, track);
        control.append(stateLabel, switchLabel);
        row.append(identity, control);
        return row;
    },

    async loadDevice(deviceId) {
        const requestId = ++this.loadId;
        const rows = document.getElementById('mcp-feature-controls');
        rows.innerHTML = `<div class="p-5 text-sm text-gray-400"><i class="ph ph-spinner animate-spin mr-2"></i>${i18n.t('device_detail.mcp_loading')}</div>`;

        try {
            const data = await this.loadExposures(deviceId);
            if (requestId !== this.loadId || state.selectedDeviceDetail?.id !== deviceId) return;
            rows.replaceChildren();
            const features = Array.isArray(data.features) ? data.features : [];
            if (!features.length) {
                const empty = document.createElement('div');
                empty.className = 'p-5 text-sm text-gray-500';
                empty.textContent = i18n.t('device_detail.mcp_empty');
                rows.appendChild(empty);
                return;
            }
            features.forEach(feature => rows.appendChild(this.renderExposureRow(deviceId, feature)));
        } catch (error) {
            if (requestId !== this.loadId || state.selectedDeviceDetail?.id !== deviceId) return;
            rows.replaceChildren();
            const failure = document.createElement('div');
            failure.className = 'p-5 bg-amber-50';
            const title = document.createElement('p');
            title.className = 'text-sm font-semibold text-amber-800';
            title.textContent = i18n.t('device_detail.mcp_load_error');
            const message = document.createElement('p');
            message.className = 'text-xs text-amber-700 mt-1 break-words';
            message.textContent = error.message;
            const hint = document.createElement('p');
            hint.className = 'text-xs text-amber-700 mt-2';
            hint.textContent = i18n.t('device_detail.mcp_token_hint');
            failure.append(title, message, hint);
            rows.appendChild(failure);
        }
    },

    async onToggle(deviceId, feature, input, stateLabel) {
        const enabled = feature.control_enabled === true && feature.health === 'enabled';
        input.disabled = true;
        try {
            await this.toggleExposure(deviceId, feature.feature_id, !enabled);
            feature.control_enabled = !enabled;
            feature.health = !enabled ? 'enabled' : 'orphaned';
            input.checked = !enabled;
            input.setAttribute('aria-checked', String(!enabled));
            stateLabel.textContent = i18n.t(!enabled ? 'device_detail.mcp_enabled' : 'device_detail.mcp_disabled');
            stateLabel.className = !enabled ? 'text-xs font-semibold text-brand-700' : 'text-xs font-semibold text-gray-500';
            ui.showToast(i18n.t('device_detail.mcp_updated'), 'success');
        } catch (error) {
            input.checked = enabled;
            input.setAttribute('aria-checked', String(enabled));
            ui.showToast(error.message, 'error');
        } finally {
            input.disabled = false;
        }
    }
};
