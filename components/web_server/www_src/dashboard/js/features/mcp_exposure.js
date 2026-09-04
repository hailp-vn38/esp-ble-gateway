// --- MCP feature permission controls ---
const mcpControls = {
    async toggleExposure(deviceId, featureId, enable) {
        const payload = {device_id: deviceId, feature_id: featureId, enabled: enable};
        return api.request('/api/mcp/exposures', {
            method: 'PUT',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(payload)
        });
    },

    renderExposureRow(deviceId, feature) {
        const enabled = feature.mcp_control?.enabled === true && feature.mcp_control.health === 'enabled';
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
        const health = feature.mcp_control?.health || 'missing';
        stateLabel.className = enabled ? 'text-xs font-semibold text-brand-700' : 'text-xs font-semibold text-gray-500';
        stateLabel.textContent = health === 'needs_review' ? i18n.t('device_detail.mcp_needs_review') :
            health === 'orphaned' || health === 'missing' ? i18n.t('device_detail.mcp_unavailable') :
            i18n.t(enabled ? 'device_detail.mcp_enabled' : 'device_detail.mcp_disabled');
        const switchLabel = document.createElement('label');
        switchLabel.className = 'settings-switch';
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.checked = enabled;
        input.disabled = health !== 'enabled' && health !== 'disabled';
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

    renderFeatures(deviceId, features) {
        const rows = document.getElementById('mcp-feature-controls');
        rows.replaceChildren();
        if (!features.length) {
            const empty = document.createElement('div');
            empty.className = 'p-5 text-sm text-gray-500';
            empty.textContent = i18n.t('device_detail.mcp_empty');
            rows.appendChild(empty);
            return;
        }
        features.forEach(feature => rows.appendChild(this.renderExposureRow(deviceId, feature)));
    },

    async onToggle(deviceId, feature, input, stateLabel) {
        const enabled = feature.mcp_control?.enabled === true && feature.mcp_control.health === 'enabled';
        input.disabled = true;
        try {
            const result = await this.toggleExposure(deviceId, feature.feature_id, !enabled);
            feature.mcp_control.enabled = result.control_enabled === true;
            feature.mcp_control.health = result.health || 'missing';
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
            input.disabled = feature.mcp_control?.health !== 'enabled' &&
                feature.mcp_control?.health !== 'disabled';
        }
    }
};
