// --- Settings Logic ---
const settings = {
    formatUptime(milliseconds) {
        const seconds = Math.floor(milliseconds / 1000);
        const days = Math.floor(seconds / 86400);
        const hours = Math.floor((seconds % 86400) / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        return `${days ? `${days}d ` : ''}${hours}h ${minutes}m`;
    },
    async load() {
        // Set language selector
        const langSelector = document.getElementById('lang-selector');
        if (langSelector) langSelector.value = i18n.currentLang;
        i18n.applyTranslations();

        try {
            const status = await api.getStatus();
            document.getElementById('set-fw-version').innerText =
                `${status.firmware_version} · IDF ${status.idf_version}`;
            document.getElementById('set-uptime').innerText = this.formatUptime(status.uptime_ms);
            document.getElementById('set-heap').innerText = `${(status.free_heap / 1024).toFixed(1)} KB`;
            document.getElementById('set-ssid').innerText = status.wifi_ssid || i18n.t('settings.disconnected');
            document.getElementById('set-ip').innerText = status.ip;
            document.getElementById('set-mac').innerText = status.wifi_mac;
            document.getElementById('set-wifi-rssi').innerText =
                Number.isFinite(status.wifi_rssi) ? `${status.wifi_rssi} dBm` : i18n.t('settings.na');
            document.getElementById('sidebar-ip').innerText = status.ip;
            document.getElementById('sidebar-status-text').innerText = 'Gateway Online';
            document.getElementById('sidebar-status-dot').className =
                'w-2 h-2 rounded-full bg-green-500 mr-2 animate-pulse';
        } catch(e) {
            document.getElementById('sidebar-status-text').innerText = 'Gateway Offline';
            document.getElementById('sidebar-status-dot').className =
                'w-2 h-2 rounded-full bg-red-500 mr-2';
            ui.showToast('Could not load system info', 'error');
        }
    },

    async restartGateway() {
        if(!confirm(i18n.t('settings.restart_confirm'))) return;
        
        try {
            await api.restart();
            this.triggerRestartUI();
        } catch(e) {
            ui.showToast("Failed to send restart command", "error");
        }
    },

    triggerRestartUI() {
        const overlay = document.getElementById('overlay-restarting');
        overlay.classList.remove('hidden');
        overlay.classList.add('flex');
        
        // Trigger reflow
        void overlay.offsetWidth;
        overlay.classList.remove('opacity-0');

        let count = 15;
        const counterEl = document.getElementById('restart-countdown');
        
        const interval = setInterval(() => {
            count--;
            counterEl.innerText = `Reconnecting in ${count}s...`;
            if (count <= 0) {
                clearInterval(interval);
                counterEl.innerText = "Reloading page...";
                window.location.reload();
            }
        }, 1000);
    },

    // --- MCP Token Management ---
    async loadMcpTokenStatus() {
        try {
            const result = await api.request('/api/mcp-token');
            const statusEl = document.getElementById('mcp-token-status');
            const actionsEl = document.getElementById('mcp-token-actions');
            
            if (result.has_token) {
                statusEl.innerHTML = `<p class="text-sm text-green-600 font-medium">${i18n.t('settings.mcp_token_set')} <span class="text-gray-400 font-mono">(${result.token_preview || '****'})</span></p>`;
                actionsEl.innerHTML = `
                    <button onclick="settings.generateMcpToken()" data-i18n="settings.mcp_rotate" class="px-3 py-1.5 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-xs font-medium">Rotate Token</button>
                    <button onclick="settings.revokeMcpToken()" data-i18n="settings.mcp_revoke" class="px-3 py-1.5 bg-red-50 text-red-600 rounded-lg hover:bg-red-100 transition-colors text-xs font-medium">Revoke</button>
                `;
            } else {
                statusEl.innerHTML = `<p class="text-sm text-gray-500">${i18n.t('settings.mcp_no_token')}</p>`;
                actionsEl.innerHTML = `
                    <button onclick="settings.generateMcpToken()" data-i18n="settings.mcp_generate" class="px-3 py-1.5 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-xs font-medium">Generate Token</button>
                `;
            }
        } catch(e) {
            document.getElementById('mcp-token-status').innerHTML =
                `<p class="text-sm text-red-500">Failed to load token status</p>`;
        }
    },

    async generateMcpToken() {
        try {
            const result = await api.request('/api/mcp-token/generate', {method: 'POST'});
            const newTokenEl = document.getElementById('mcp-token-new');
            const tokenValueEl = document.getElementById('mcp-new-token-value');
            
            tokenValueEl.value = result.token;
            newTokenEl.classList.remove('hidden');
            
            this.loadMcpTokenStatus();
            ui.showToast(i18n.t('settings.mcp_token_generated'), 'success');
        } catch(e) {
            ui.showToast('Failed to generate token', 'error');
        }
    },

    async revokeMcpToken() {
        if (!confirm(i18n.t('settings.mcp_revoke_confirm'))) return;
        
        try {
            await api.request('/api/mcp-token', {method: 'DELETE'});
            document.getElementById('mcp-token-new').classList.add('hidden');
            this.loadMcpTokenStatus();
            ui.showToast(i18n.t('settings.mcp_token_revoked'), 'success');
        } catch(e) {
            ui.showToast('Failed to revoke token', 'error');
        }
    },

    copyMcpToken() {
        const input = document.getElementById('mcp-new-token-value');
        input.select();
        navigator.clipboard.writeText(input.value);
        ui.showToast(i18n.t('settings.mcp_token_copied'), 'success');
    }
};
