// --- MCP Tools Logic ---
const mcpTools = {
    loadId: 0,
    enabledByDevice: new Map(),

    async loadExposures(deviceId) {
        return api.request(`/api/mcp/exposures?device_id=${encodeURIComponent(deviceId)}`);
    },

    async toggleExposure(deviceId, command, enable) {
        const payload = {device_id: deviceId, command, enabled: enable};
        if (enable) payload.confirm_destructive = true;
        return api.request('/api/mcp/exposures', {
            method: 'PUT',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(payload)
        });
    },

    renderExposureRow(deviceId, command) {
        const enabled = command.state === 'enabled';
        const row = document.createElement('div');
        row.className = 'flex flex-col sm:flex-row sm:items-center justify-between gap-4 p-5';

        const identity = document.createElement('div');
        identity.className = 'min-w-0';
        const heading = document.createElement('div');
        heading.className = 'flex flex-wrap items-center gap-2';
        const label = document.createElement('h4');
        label.className = 'text-sm font-semibold text-gray-800 break-words';
        label.textContent = command.label || command.command;
        heading.appendChild(label);
        if (command.semantic_name) {
            const badge = document.createElement('span');
            badge.className = 'inline-flex rounded-full bg-blue-100 px-2 py-0.5 text-xs font-semibold text-blue-700';
            badge.textContent = command.semantic_name;
            heading.appendChild(badge);
        }
        if (command.feature_bound) {
            const fb = document.createElement('span');
            fb.className = 'inline-flex rounded-full bg-green-100 px-2 py-0.5 text-xs font-semibold text-green-700';
            fb.textContent = i18n.t('device_detail.mcp_feature_bound');
            heading.appendChild(fb);
        }
        if (command.destructive) {
            const badge = document.createElement('span');
            badge.className = 'inline-flex rounded-full bg-red-100 px-2 py-0.5 text-xs font-semibold text-red-700';
            badge.textContent = `⚠ ${i18n.t('device_detail.destructive')}`;
            heading.appendChild(badge);
        }
        const toolName = document.createElement('p');
        toolName.className = 'mt-1 text-xs font-mono text-gray-600 break-all';
        toolName.textContent = command.tool_name || command.command;
        const description = document.createElement('p');
        description.className = 'mt-1 text-xs text-gray-500';
        description.textContent = command.semantic_name
            ? i18n.t('device_detail.mcp_row_semantic_desc')
            : i18n.t('device_detail.mcp_row_desc');
        identity.append(heading, toolName, description);

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
        input.setAttribute('aria-label', `${command.label || command.command}: ${stateLabel.textContent}`);
        const track = document.createElement('span');
        track.className = 'settings-switch-track';
        input.onchange = () => this.onToggle(deviceId, command, input, stateLabel);
        switchLabel.append(input, track);
        control.append(stateLabel, switchLabel);
        row.append(identity, control);
        return row;
    },

    renderCapacity(deviceId, capacity) {
        const used = capacity.enabled || 0;
        const max = capacity.max_enabled || 32;
        this.enabledByDevice.set(deviceId, used);
        const element = document.getElementById('mcp-capacity-text');
        element.textContent = i18n.t('device_detail.mcp_capacity')
            .replace('{used}', used).replace('{max}', max);
        const ratio = max > 0 ? used / max : 1;
        element.className = `mcp-capacity text-xs font-mono ${ratio >= 1 ? 'text-red-600 font-semibold' : (ratio >= 0.8 ? 'text-amber-600 font-semibold' : 'text-gray-500')}`;
    },

    async loadDevice(deviceId) {
        const requestId = ++this.loadId;
        const rows = document.getElementById('mcp-tool-rows');
        const capacityText = document.getElementById('mcp-capacity-text');
        rows.innerHTML = `<div class="p-5 text-sm text-gray-400"><i class="ph ph-spinner animate-spin mr-2"></i>${i18n.t('device_detail.mcp_loading')}</div>`;
        capacityText.textContent = '';

        try {
            const data = await this.loadExposures(deviceId);
            if (requestId !== this.loadId || state.selectedDeviceDetail?.id !== deviceId) return;
            this.renderCapacity(deviceId, data.capacity || {});
            rows.replaceChildren();
            const commands = Array.isArray(data.commands) ? data.commands : [];
            if (!commands.length) {
                const empty = document.createElement('div');
                empty.className = 'p-5 text-sm text-gray-500';
                empty.textContent = i18n.t('device_detail.mcp_empty');
                rows.appendChild(empty);
                return;
            }
            commands.forEach(command => rows.appendChild(this.renderExposureRow(deviceId, command)));
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

    async onToggle(deviceId, command, input, stateLabel) {
        const enabled = command.state === 'enabled';
        if (!enabled && command.destructive &&
            !confirm(i18n.t('device_detail.mcp_destructive_confirm')
                .replace('{name}', command.label || command.command))) {
            input.checked = false;
            input.setAttribute('aria-checked', 'false');
            return;
        }

        input.disabled = true;
        try {
            await this.toggleExposure(deviceId, command.command, !enabled);
            command.state = enabled ? 'disabled' : 'enabled';
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
            if (state.selectedDeviceDetail?.id === deviceId) void this.loadDevice(deviceId);
        }
    }
};
