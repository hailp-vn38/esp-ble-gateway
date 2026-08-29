// --- Settings Logic ---
const settings = {
    networkState: { connected: false },
    mcpState: { configured: false, preview: '' },
    xiaozhiState: {
        supported: true,
        enabled: false,
        runtime_enabled: false,
        restart_required: false,
        endpoint_configured: false,
        state: 'disabled'
    },
    xiaozhiEndpointEditing: false,
    xiaozhiBusy: false,
    xiaozhiBusyAction: '',

    formatUptime(milliseconds) {
        const seconds = Math.floor(milliseconds / 1000);
        const days = Math.floor(seconds / 86400);
        const hours = Math.floor((seconds % 86400) / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        return `${days ? `${days}d ` : ''}${hours}h ${minutes}m`;
    },

    formatMemory(bytes) {
        const value = Number(bytes) || 0;
        if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(1)} MB`;
        return `${(value / 1024).toFixed(1)} KB`;
    },

    async load() {
        const langSelector = document.getElementById('lang-selector');
        if (langSelector) langSelector.value = i18n.currentLang;
        i18n.applyTranslations();

        try {
            const data = await api.request('/api/settings');
            const system = data.system || {};
            this.networkState = data.network || { connected: false };

            document.getElementById('set-fw-version').textContent = system.firmware || '—';
            document.getElementById('set-idf-version').textContent = `IDF ${system.idf || '—'}`;
            document.getElementById('set-uptime').textContent =
                this.formatUptime(system.uptime_ms || 0);
            document.getElementById('set-heap').textContent =
                this.formatMemory(system.free_heap || 0);
            document.getElementById('set-ssid').textContent =
                this.networkState.ssid || i18n.t('settings.disconnected');
            document.getElementById('set-ip').textContent = this.networkState.ip || '0.0.0.0';
            document.getElementById('set-mac').textContent =
                this.networkState.mac || '00:00:00:00:00:00';
            document.getElementById('sidebar-ip').textContent = this.networkState.ip || '0.0.0.0';
            document.getElementById('sidebar-status-text').textContent = 'Gateway Online';
            document.getElementById('sidebar-status-dot').className =
                'w-2 h-2 rounded-full bg-green-500 mr-2 animate-pulse';

            this.mcpState = data.mcp || this.mcpState;
            this.xiaozhiState = data.xiaozhi || this.xiaozhiState;
            this.xiaozhiEndpointEditing = false;
            this.renderNetworkStatus();
            this.renderMcpTokenStatus();
            this.renderXiaozhiStatus();
        } catch (e) {
            document.getElementById('sidebar-status-text').textContent = 'Gateway Offline';
            document.getElementById('sidebar-status-dot').className =
                'w-2 h-2 rounded-full bg-red-500 mr-2';
            this.networkState = { connected: false };
            this.renderNetworkStatus();
            document.getElementById('mcp-auth-text').textContent =
                i18n.t('settings.load_failed');
            document.getElementById('mcp-auth-description').textContent = '';
            document.getElementById('xiaozhi-state-text').textContent =
                i18n.t('settings.xiaozhi_load_failed');
            ui.showToast(i18n.t('settings.load_failed'), 'error');
        }
    },

    renderNetworkStatus() {
        const network = this.networkState || {};
        const connected = Boolean(network.connected);
        const badge = document.getElementById('network-state-badge');
        const dot = document.getElementById('network-state-dot');
        const text = document.getElementById('network-state-text');
        const rssiEl = document.getElementById('set-wifi-rssi');
        const qualityEl = document.getElementById('set-wifi-quality');

        badge.className = connected
            ? 'inline-flex items-center gap-1.5 rounded-full bg-green-50 px-2.5 py-1 text-xs font-semibold text-green-700 whitespace-nowrap'
            : 'inline-flex items-center gap-1.5 rounded-full bg-gray-100 px-2.5 py-1 text-xs font-semibold text-gray-600 whitespace-nowrap';
        dot.className = connected
            ? 'w-1.5 h-1.5 rounded-full bg-green-500'
            : 'w-1.5 h-1.5 rounded-full bg-gray-400';
        text.textContent = i18n.t(connected
            ? 'settings.network_connected'
            : 'settings.network_disconnected');

        if (!Number.isFinite(network.rssi)) {
            rssiEl.textContent = i18n.t('settings.na');
            qualityEl.textContent = '';
            return;
        }
        const qualityKey = network.rssi >= -55
            ? 'settings.signal_good'
            : (network.rssi >= -67 ? 'settings.signal_fair' : 'settings.signal_weak');
        rssiEl.textContent = `${network.rssi} dBm`;
        qualityEl.textContent = ` · ${i18n.t(qualityKey)}`;
    },

    async restartGateway() {
        if (!confirm(i18n.t('settings.restart_confirm'))) return;
        try {
            await api.restart();
            this.triggerRestartUI();
        } catch (e) {
            ui.showToast(i18n.t('settings.restart_failed'), 'error');
        }
    },

    triggerRestartUI() {
        const overlay = document.getElementById('overlay-restarting');
        overlay.classList.remove('hidden');
        overlay.classList.add('flex');
        void overlay.offsetWidth;
        overlay.classList.remove('opacity-0');

        let count = 15;
        const counterEl = document.getElementById('restart-countdown');
        const interval = setInterval(() => {
            count--;
            counterEl.textContent = `Reconnecting in ${count}s...`;
            if (count <= 0) {
                clearInterval(interval);
                counterEl.textContent = 'Reloading page...';
                window.location.reload();
            }
        }, 1000);
    },

    // --- MCP Token Management ---
    renderMcpTokenStatus() {
        const configured = Boolean(this.mcpState.configured);
        const status = document.getElementById('mcp-auth-status');
        const dot = document.getElementById('mcp-auth-dot');
        const text = document.getElementById('mcp-auth-text');
        const description = document.getElementById('mcp-auth-description');
        const previewRow = document.getElementById('mcp-token-preview-row');
        const preview = document.getElementById('mcp-token-preview');
        const primary = document.getElementById('mcp-primary-action');
        const revoke = document.getElementById('mcp-revoke-action');

        status.className = configured
            ? 'inline-flex items-center gap-1.5 rounded-full bg-green-50 px-2.5 py-1 text-xs font-semibold text-green-700'
            : 'inline-flex items-center gap-1.5 rounded-full bg-amber-50 px-2.5 py-1 text-xs font-semibold text-amber-700';
        dot.className = configured
            ? 'w-1.5 h-1.5 rounded-full bg-green-500'
            : 'w-1.5 h-1.5 rounded-full bg-amber-500';
        text.textContent = i18n.t(configured
            ? 'settings.mcp_protected'
            : 'settings.mcp_unprotected');
        description.textContent = i18n.t(configured
            ? 'settings.mcp_protected_desc'
            : 'settings.mcp_unprotected_desc');
        previewRow.classList.toggle('hidden', !configured);
        preview.textContent = `••••••••${(this.mcpState.preview || '').replace(/^\.+/, '')}`;
        primary.textContent = i18n.t(configured
            ? 'settings.mcp_rotate'
            : 'settings.mcp_generate');
        revoke.classList.toggle('hidden', !configured);
    },

    async generateMcpToken() {
        const button = document.getElementById('mcp-primary-action');
        button.disabled = true;
        try {
            const result = await api.request('/api/mcp-token/generate', {method: 'POST'});
            document.getElementById('mcp-new-token-value').value = result.token;
            document.getElementById('mcp-token-new').classList.remove('hidden');
            this.mcpState = { configured: true, preview: `...${result.token.slice(-4)}` };
            this.renderMcpTokenStatus();
            ui.showToast(i18n.t('settings.mcp_token_generated'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.mcp_generate_failed'), 'error');
        } finally {
            button.disabled = false;
        }
    },

    async revokeMcpToken() {
        if (!confirm(i18n.t('settings.mcp_revoke_confirm'))) return;
        const button = document.getElementById('mcp-revoke-action');
        button.disabled = true;
        try {
            await api.request('/api/mcp-token', {method: 'DELETE'});
            const newToken = document.getElementById('mcp-new-token-value');
            newToken.value = '';
            document.getElementById('mcp-token-new').classList.add('hidden');
            this.mcpState = { configured: false, preview: '' };
            this.renderMcpTokenStatus();
            ui.showToast(i18n.t('settings.mcp_token_revoked'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.mcp_revoke_failed'), 'error');
        } finally {
            button.disabled = false;
        }
    },

    async copyMcpToken() {
        const input = document.getElementById('mcp-new-token-value');
        try {
            await navigator.clipboard.writeText(input.value);
            ui.showToast(i18n.t('settings.mcp_token_copied'), 'success');
        } catch (e) {
            input.select();
            document.execCommand('copy');
            ui.showToast(i18n.t('settings.mcp_token_copied'), 'success');
        }
    },

    // --- Xiaozhi Direct MCP Bridge ---
    setXiaozhiBusy(busy, action = '') {
        this.xiaozhiBusy = busy;
        this.xiaozhiBusyAction = busy ? action : '';
        this.renderXiaozhiStatus();
    },

    renderXiaozhiStatus() {
        const state = this.xiaozhiState || {};
        const rawState = state.supported === false
            ? 'unavailable'
            : (state.state === 'unsupported' ? 'unavailable' : (state.state || 'disabled'));
        const tone = {
            connected: {
                badge: 'bg-green-50 text-green-700',
                dot: 'bg-green-500'
            },
            connecting: {
                badge: 'bg-brand-50 text-brand-700',
                dot: 'bg-brand-500 animate-pulse'
            },
            handshaking: {
                badge: 'bg-brand-50 text-brand-700',
                dot: 'bg-brand-500 animate-pulse'
            },
            wait_network: {
                badge: 'bg-amber-50 text-amber-700',
                dot: 'bg-amber-500'
            },
            backoff: {
                badge: 'bg-amber-50 text-amber-700',
                dot: 'bg-amber-500'
            },
            error: {
                badge: 'bg-red-50 text-red-700',
                dot: 'bg-red-500'
            },
            disabled: {
                badge: 'bg-gray-100 text-gray-600',
                dot: 'bg-gray-400'
            },
            unavailable: {
                badge: 'bg-gray-100 text-gray-600',
                dot: 'bg-gray-400'
            }
        };
        const selectedTone = tone[rawState] || tone.disabled;
        const badge = document.getElementById('xiaozhi-state');
        badge.className = `inline-flex self-start items-center gap-1.5 rounded-full px-2.5 py-1 text-xs font-semibold whitespace-nowrap ${selectedTone.badge}`;
        document.getElementById('xiaozhi-state-dot').className =
            `w-1.5 h-1.5 rounded-full ${selectedTone.dot}`;
        document.getElementById('xiaozhi-state-text').textContent =
            i18n.t(`settings.xiaozhi_state_${rawState}`);

        const enabled = document.getElementById('xiaozhi-enabled');
        enabled.checked = Boolean(state.enabled);
        enabled.disabled = this.xiaozhiBusy || state.supported === false;

        const needsRestart = Boolean(state.restart_required);
        document.getElementById('xiaozhi-restart-warning')
            .classList.toggle('hidden', !needsRestart);

        const hasEndpoint = Boolean(state.endpoint_configured);
        const editing = hasEndpoint && this.xiaozhiEndpointEditing;
        const endpointView = document.getElementById('xiaozhi-endpoint-view');
        const endpointEditor = document.getElementById('xiaozhi-endpoint-editor');
        endpointView.classList.toggle('hidden', !hasEndpoint || editing);
        endpointEditor.classList.toggle('hidden', hasEndpoint && !editing);
        document.getElementById('xiaozhi-current-endpoint-row')
            .classList.toggle('hidden', !editing);
        document.getElementById('xiaozhi-endpoint-cancel')
            .classList.toggle('hidden', !hasEndpoint);

        const display = state.endpoint_display || i18n.t('settings.xiaozhi_not_configured');
        document.getElementById('xiaozhi-endpoint-display').textContent = display;
        document.getElementById('xiaozhi-current-endpoint').textContent = display;
        const inputLabel = document.getElementById('xiaozhi-endpoint-input-label');
        const editorHint = document.getElementById('xiaozhi-endpoint-editor-hint');
        const saveEndpoint = document.getElementById('xiaozhi-endpoint-save');
        inputLabel.textContent = i18n.t(editing
            ? 'settings.xiaozhi_new_endpoint'
            : 'settings.xiaozhi_endpoint');
        editorHint.textContent = i18n.t(editing
            ? 'settings.xiaozhi_endpoint_replace_hint'
            : 'settings.xiaozhi_endpoint_create_hint');
        saveEndpoint.textContent = i18n.t(editing
            ? 'settings.xiaozhi_save_changes'
            : 'settings.xiaozhi_save_endpoint');

        const endpointInput = document.getElementById('xiaozhi-endpoint-input');
        const changeEndpoint = document.getElementById('xiaozhi-change-endpoint');
        const cancelEndpoint = document.getElementById('xiaozhi-endpoint-cancel');
        endpointInput.disabled = this.xiaozhiBusy;
        saveEndpoint.disabled = this.xiaozhiBusy;
        changeEndpoint.disabled = this.xiaozhiBusy;
        cancelEndpoint.disabled = this.xiaozhiBusy;

        const protocolRow = document.getElementById('xiaozhi-protocol-row');
        protocolRow.classList.toggle('hidden', !state.protocol_version);
        document.getElementById('xiaozhi-protocol').textContent =
            state.protocol_version || '';

        const errorParts = [];
        if (state.last_error) errorParts.push(`ESP ${state.last_error}`);
        if (state.last_http_status) errorParts.push(`HTTP ${state.last_http_status}`);
        if (state.last_ws_close_code) errorParts.push(`WS ${state.last_ws_close_code}`);
        document.getElementById('xiaozhi-error-row')
            .classList.toggle('hidden', errorParts.length === 0);
        document.getElementById('xiaozhi-last-error').textContent = errorParts.join(' · ');

        const reconnect = document.getElementById('xiaozhi-reconnect');
        const reconnecting = rawState === 'connecting' || rawState === 'handshaking';
        const canReconnect = Boolean(state.runtime_enabled) &&
                             hasEndpoint &&
                             !needsRestart &&
                             !reconnecting &&
                             !this.xiaozhiBusy;
        reconnect.disabled = !canReconnect;
        document.getElementById('xiaozhi-reconnect-label').textContent =
            i18n.t(reconnecting || this.xiaozhiBusyAction === 'reconnect'
                ? 'settings.xiaozhi_reconnecting_action'
                : 'settings.xiaozhi_reconnect');

        const connectionMessageKey = rawState === 'connected'
            ? 'settings.xiaozhi_connection_active'
            : (reconnecting
                ? 'settings.xiaozhi_connection_connecting'
                : 'settings.xiaozhi_connection_inactive');
        document.getElementById('xiaozhi-connection-message').textContent =
            i18n.t(connectionMessageKey);

        const remove = document.getElementById('xiaozhi-remove');
        remove.disabled = this.xiaozhiBusy || (!hasEndpoint && !state.enabled);
    },

    async toggleXiaozhiEnabled() {
        const toggle = document.getElementById('xiaozhi-enabled');
        const desired = toggle.checked;
        if (desired && !this.xiaozhiState.endpoint_configured) {
            toggle.checked = false;
            ui.showToast(i18n.t('settings.xiaozhi_endpoint_required'), 'error');
            return;
        }

        this.setXiaozhiBusy(true, 'toggle');
        try {
            const result = await api.request('/api/settings/xiaozhi', {
                method: 'PUT',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ enabled: desired })
            });
            this.xiaozhiState = result.xiaozhi || this.xiaozhiState;
            ui.showToast(i18n.t(this.xiaozhiState.restart_required
                ? 'settings.xiaozhi_restart_required_toast'
                : 'settings.xiaozhi_toggle_saved'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.xiaozhi_save_failed'), 'error');
        } finally {
            this.setXiaozhiBusy(false);
        }
    },

    beginXiaozhiEndpointEdit() {
        this.xiaozhiEndpointEditing = true;
        const input = document.getElementById('xiaozhi-endpoint-input');
        input.value = '';
        this.renderXiaozhiStatus();
        input.focus();
    },

    cancelXiaozhiEndpointEdit() {
        this.xiaozhiEndpointEditing = false;
        document.getElementById('xiaozhi-endpoint-input').value = '';
        this.renderXiaozhiStatus();
    },

    async saveXiaozhiEndpoint() {
        const input = document.getElementById('xiaozhi-endpoint-input');
        const endpoint = input.value.trim();
        if (!endpoint || !endpoint.startsWith('wss://')) {
            ui.showToast(i18n.t('settings.xiaozhi_endpoint_required'), 'error');
            input.focus();
            return;
        }

        this.setXiaozhiBusy(true, 'endpoint');
        try {
            const result = await api.request('/api/settings/xiaozhi', {
                method: 'PUT',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ endpoint })
            });
            input.value = '';
            this.xiaozhiEndpointEditing = false;
            this.xiaozhiState = result.xiaozhi || this.xiaozhiState;
            ui.showToast(i18n.t(
                this.xiaozhiState.runtime_enabled && this.xiaozhiState.enabled
                    ? 'settings.xiaozhi_endpoint_reconnecting'
                    : 'settings.xiaozhi_endpoint_saved'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.xiaozhi_save_failed'), 'error');
        } finally {
            this.setXiaozhiBusy(false);
        }
    },

    async clearXiaozhi() {
        if (!confirm(i18n.t('settings.xiaozhi_clear_confirm'))) return;
        this.setXiaozhiBusy(true, 'clear');
        try {
            const result = await api.request('/api/settings/xiaozhi', {
                method: 'PUT',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ enabled: false, clear_endpoint: true })
            });
            document.getElementById('xiaozhi-endpoint-input').value = '';
            this.xiaozhiEndpointEditing = false;
            this.xiaozhiState = result.xiaozhi || {
                supported: true,
                enabled: false,
                runtime_enabled: false,
                restart_required: false,
                endpoint_configured: false,
                state: 'disabled'
            };
            ui.showToast(i18n.t('settings.xiaozhi_cleared'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.xiaozhi_save_failed'), 'error');
        } finally {
            this.setXiaozhiBusy(false);
        }
    },

    async reconnectXiaozhi() {
        this.setXiaozhiBusy(true, 'reconnect');
        try {
            const result = await api.request('/api/settings/xiaozhi/reconnect', {
                method: 'POST'
            });
            this.xiaozhiState.state = result.state || 'connecting';
            ui.showToast(i18n.t('settings.xiaozhi_reconnecting'), 'success');
        } catch (e) {
            ui.showToast(e.message || i18n.t('settings.xiaozhi_reconnect_failed'), 'error');
        } finally {
            this.setXiaozhiBusy(false);
        }
    }
};
