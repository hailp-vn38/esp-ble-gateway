// --- MCP Tools Logic ---
const mcpTools = {
    loadId: 0,

    async loadExposures(deviceId) {
        return api.request(`/api/mcp/exposures?device_id=${encodeURIComponent(deviceId)}`);
    },

    async toggleExposure(deviceId, command, enable) {
        const payload = {device_id: deviceId, command: command, enabled: enable};
        if (enable) payload.confirm_destructive = true;
        const headers = {'Content-Type': 'application/json'};
        return api.request('/api/mcp/exposures', {method: 'PUT', headers, body: JSON.stringify(payload)});
    },

    renderChip(deviceId, command) {
        const selected = command.state === 'enabled';
        const chip = document.createElement('button');
        chip.type = 'button';
        chip.setAttribute('aria-pressed', String(selected));
        chip.title = command.tool_name || command.command;
        chip.className = selected
            ? 'mcp-chip mcp-chip-selected'
            : 'mcp-chip mcp-chip-unselected';

        const icon = document.createElement('i');
        icon.className = `ph ${selected ? 'ph-check-circle' : 'ph-plus'} mr-1.5`;
        const label = document.createElement('span');
        label.textContent = command.label || command.command;
        chip.append(icon, label);
        chip.onclick = () => this.onToggle(deviceId, command, chip);
        return chip;
    },

    async loadDevice(deviceId) {
        const requestId = ++this.loadId;
        const chips = document.getElementById('mcp-tool-chips');
        const capacityText = document.getElementById('mcp-capacity-text');
        chips.innerHTML = '<span class="text-xs text-gray-400"><i class="ph ph-spinner animate-spin mr-1"></i>Loading MCP tools…</span>';
        capacityText.textContent = '';

        try {
            const data = await this.loadExposures(deviceId);
            if (requestId !== this.loadId ||
                state.selectedDeviceDetail?.id !== deviceId) return;

            const cap = data.capacity || {};
            const used = cap.enabled || 0;
            const max = cap.max_enabled || 32;
            capacityText.textContent = `${used} / ${max} tool slots`;
            chips.replaceChildren();

            const commands = Array.isArray(data.commands) ? data.commands : [];
            if (commands.length === 0) {
                chips.innerHTML = '<span class="text-xs text-gray-400">No commands available. Refresh device capabilities first.</span>';
                return;
            }
            commands.forEach(command =>
                chips.appendChild(this.renderChip(deviceId, command)));
        } catch (error) {
            if (requestId !== this.loadId ||
                state.selectedDeviceDetail?.id !== deviceId) return;
            chips.replaceChildren();
            const message = document.createElement('span');
            message.className = 'mcp-tool-error text-xs';
            message.textContent = `${error.message}. Set the MCP admin token in Gateway Settings.`;
            chips.appendChild(message);
        }
    },

    async onToggle(deviceId, command, chip) {
        const selected = command.state === 'enabled';
        if (!selected && command.destructive &&
            !confirm(`Expose destructive command ${command.command} to MCP clients?`)) {
            return;
        }

        chip.disabled = true;
        try {
            await this.toggleExposure(deviceId, command.command, !selected);
            ui.showToast(
                `${command.label || command.command} ${selected ? 'removed from' : 'added to'} MCP tools`,
                'success');
        } catch (error) {
            ui.showToast(error.message, 'error');
        } finally {
            if (state.selectedDeviceDetail?.id === deviceId) {
                this.loadDevice(deviceId);
            }
        }
    }
};
