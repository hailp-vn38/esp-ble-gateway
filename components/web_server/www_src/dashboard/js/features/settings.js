// --- Settings Logic ---
const settings = {
    authState: { enabled: false, configured: false, username: '' },
    mcpState: { configured: false, preview: '' },
    xiaozhiState: { enabled: false, endpoint_configured: false, state: 'disabled' },

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
            const data = await api.request('/api/settings');
            const system = data.system || {};
            const network = data.network || {};

            document.getElementById('set-fw-version').innerText =
                `${system.firmware || '-'} · IDF ${system.idf || '-'}`;
            document.getElementById('set-uptime').innerText =
                this.formatUptime(system.uptime_ms || 0);
            document.getElementById('set-heap').innerText =
                `${((system.free_heap || 0) / 1024).toFixed(1)} KB`;
            document.getElementById('set-ssid').innerText =
                network.ssid || i18n.t('settings.disconnected');
            document.getElementById('set-ip').innerText = network.ip || '0.0.0.0';
            document.getElementById('set-mac').innerText =
                network.mac || '00:00:00:00:00:00';
            document.getElementById('set-wifi-rssi').innerText =
                Number.isFinite(network.rssi) ? `${network.rssi} dBm` : i18n.t('settings.na');
            document.getElementById('sidebar-ip').innerText = network.ip || '0.0.0.0';
            document.getElementById('sidebar-status-text').innerText = 'Gateway Online';
            document.getElementById('sidebar-status-dot').className =
                'w-2 h-2 rounded-full bg-green-500 mr-2 animate-pulse';

            this.authState = data.auth || this.authState;
            this.mcpState = data.mcp || this.mcpState;
            this.xiaozhiState = data.xiaozhi || this.xiaozhiState;
            this.renderAuthStatus();
            this.renderMcpTokenStatus();
            this.renderXiaozhiStatus();
        } catch(e) {
            document.getElementById('sidebar-status-text').innerText = 'Gateway Offline';
            document.getElementById('sidebar-status-dot').className =
                'w-2 h-2 rounded-full bg-red-500 mr-2';
            ui.showToast('Could not load system info', 'error');
            document.getElementById('auth-status').innerHTML =
                `<p class="text-sm text-red-500">${i18n.t('settings.auth_load_failed')}</p>`;
            document.getElementById('mcp-token-status').innerHTML =
                '<p class="text-sm text-red-500">Failed to load token status</p>';
            document.getElementById('xiaozhi-state').textContent =
                i18n.t('settings.xiaozhi_load_failed');
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
    renderMcpTokenStatus() {
        const statusEl = document.getElementById('mcp-token-status');
        const actionsEl = document.getElementById('mcp-token-actions');

        if (this.mcpState.configured) {
            statusEl.innerHTML = `<p class="text-sm text-green-600 font-medium">${i18n.t('settings.mcp_token_set')} <span class="text-gray-400 font-mono">(${this.mcpState.preview || '...'})</span></p>`;
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
    },

    async generateMcpToken() {
        try {
            const result = await api.request('/api/mcp-token/generate', {method: 'POST'});
            const newTokenEl = document.getElementById('mcp-token-new');
            const tokenValueEl = document.getElementById('mcp-new-token-value');
            
            tokenValueEl.value = result.token;
            newTokenEl.classList.remove('hidden');

            this.mcpState = {
                configured: true,
                preview: `...${result.token.slice(-4)}`
            };
            this.renderMcpTokenStatus();
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
            this.mcpState = { configured: false, preview: '' };
            this.renderMcpTokenStatus();
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
    },

    // --- Xiaozhi Direct MCP Bridge ---
    renderXiaozhiStatus() {
        const state = this.xiaozhiState || {};
        const enabled = document.getElementById('xiaozhi-enabled');
        const stateEl = document.getElementById('xiaozhi-state');
        const endpointEl = document.getElementById('xiaozhi-endpoint-display');
        const errorRow = document.getElementById('xiaozhi-error-row');
        const errorEl = document.getElementById('xiaozhi-last-error');
        if (!enabled || !stateEl || !endpointEl) return;

        enabled.checked = Boolean(state.enabled);
        stateEl.textContent = i18n.t(`settings.xiaozhi_state_${state.state || 'disabled'}`);
        stateEl.className = state.state === 'connected'
            ? 'font-medium text-green-600'
            : (state.state === 'error' ? 'font-medium text-red-600' : 'font-medium text-amber-600');
        endpointEl.textContent = state.endpoint_display ||
            i18n.t('settings.xiaozhi_not_configured');
        endpointEl.title = state.endpoint_display || '';

        const errorParts = [];
        if (state.last_error) errorParts.push(`ESP ${state.last_error}`);
        if (state.last_http_status) errorParts.push(`HTTP ${state.last_http_status}`);
        if (state.last_ws_close_code) errorParts.push(`WS ${state.last_ws_close_code}`);
        errorRow.classList.toggle('hidden', errorParts.length === 0);
        errorEl.textContent = errorParts.join(' · ');
    },

    async saveXiaozhi() {
        const enabled = document.getElementById('xiaozhi-enabled').checked;
        const endpointInput = document.getElementById('xiaozhi-endpoint');
        const endpoint = endpointInput.value.trim();
        if (enabled && !endpoint && !this.xiaozhiState.endpoint_configured) {
            ui.showToast(i18n.t('settings.xiaozhi_endpoint_required'), 'error');
            return;
        }
        const payload = { enabled };
        if (endpoint) payload.endpoint = endpoint;
        const saveButton = document.getElementById('xiaozhi-save');
        saveButton.disabled = true;
        try {
            const result = await api.request('/api/settings/xiaozhi', {
                method: 'PUT',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(payload)
            });
            endpointInput.value = '';
            this.xiaozhiState = result.xiaozhi || this.xiaozhiState;
            this.renderXiaozhiStatus();
            ui.showToast(i18n.t('settings.xiaozhi_saved'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.xiaozhi_save_failed'), 'error');
        } finally {
            saveButton.disabled = false;
        }
    },

    async clearXiaozhi() {
        if (!confirm(i18n.t('settings.xiaozhi_clear_confirm'))) return;
        try {
            const result = await api.request('/api/settings/xiaozhi', {
                method: 'PUT',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ enabled: false, clear_endpoint: true })
            });
            document.getElementById('xiaozhi-endpoint').value = '';
            this.xiaozhiState = result.xiaozhi || {
                enabled: false, endpoint_configured: false, state: 'disabled'
            };
            this.renderXiaozhiStatus();
            ui.showToast(i18n.t('settings.xiaozhi_cleared'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.xiaozhi_save_failed'), 'error');
        }
    },

    renderAuthStatus() {
        const statusEl = document.getElementById('auth-status');
        const setupEl = document.getElementById('auth-setup');
        const enableSection = document.getElementById('auth-enable-section');
        const toggleBtn = document.getElementById('auth-toggle-btn');
        const toggleText = document.getElementById('auth-toggle-text');

        toggleBtn.disabled = false;

        if (this.authState.enabled) {
            statusEl.innerHTML = `<p class="text-sm text-green-600 font-medium">${i18n.t('settings.auth_enabled')} <span class="text-gray-400">(${this.authState.username || 'admin'})</span></p>`;
            setupEl.classList.add('hidden');
            enableSection.classList.remove('hidden');
            toggleText.textContent = i18n.t('settings.auth_disable');
            toggleBtn.className = 'w-full px-4 py-2.5 bg-red-50 text-red-600 border border-red-200 rounded-lg hover:bg-red-100 transition-colors font-medium text-sm flex items-center justify-center';
        } else if (this.authState.configured) {
            statusEl.innerHTML = `<p class="text-sm text-yellow-600">${i18n.t('settings.auth_disabled')} <span class="text-gray-400">(${this.authState.username || 'admin'})</span></p>`;
            setupEl.classList.add('hidden');
            enableSection.classList.remove('hidden');
            toggleText.textContent = i18n.t('settings.auth_enable');
            toggleBtn.className = 'w-full px-4 py-2.5 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors font-medium text-sm flex items-center justify-center';
        } else {
            statusEl.innerHTML = `<p class="text-sm text-gray-500">${i18n.t('settings.auth_not_configured')}</p>`;
            setupEl.classList.remove('hidden');
            enableSection.classList.add('hidden');
            toggleText.textContent = i18n.t('settings.auth_enable');
            toggleBtn.className = 'w-full px-4 py-2.5 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors font-medium text-sm flex items-center justify-center';
        }
    },

    async toggleAuth() {
        const toggleBtn = document.getElementById('auth-toggle-btn');
        toggleBtn.disabled = true;
        const wasEnabled = this.authState.enabled;

        try {
            if (this.authState.enabled) {
                // Disable auth
                const currentPassword = document.getElementById('auth-current-password').value;
                if (!currentPassword) {
                    ui.showToast(i18n.t('settings.auth_password_required'), 'error');
                    toggleBtn.disabled = false;
                    return;
                }
                await api.request('/api/auth/config', {
                    method: 'PUT',
                    body: JSON.stringify({ enabled: false, current_password: currentPassword })
                });
                ui.showToast(i18n.t('settings.auth_disabled_toast'), 'success');
            } else if (this.authState.configured) {
                // Re-enable auth
                const currentPassword = document.getElementById('auth-current-password').value;
                if (!currentPassword) {
                    ui.showToast(i18n.t('settings.auth_password_required'), 'error');
                    toggleBtn.disabled = false;
                    return;
                }
                await api.request('/api/auth/config', {
                    method: 'PUT',
                    body: JSON.stringify({ enabled: true, current_password: currentPassword })
                });
                ui.showToast(i18n.t('settings.auth_enabled_toast'), 'success');
            } else {
                // First-time setup
                const username = document.getElementById('auth-username').value.trim();
                const password = document.getElementById('auth-password').value;
                if (!username || !password) {
                    ui.showToast(i18n.t('settings.auth_credentials_required'), 'error');
                    toggleBtn.disabled = false;
                    return;
                }
                await api.request('/api/auth/config', {
                    method: 'PUT',
                    body: JSON.stringify({ enabled: true, username, new_password: password })
                });
                ui.showToast(i18n.t('settings.auth_enabled_toast'), 'success');
            }
            if (!wasEnabled) {
                window.location.assign('/login');
                return;
            }
            await this.load();
        } catch(e) {
            ui.showToast(i18n.t('settings.auth_action_failed'), 'error');
            toggleBtn.disabled = false;
        }
    }
};
