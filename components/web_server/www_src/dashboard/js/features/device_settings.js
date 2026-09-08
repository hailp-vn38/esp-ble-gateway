// --- Device Settings UI Module ---
const deviceSettings = {
    _baseline: null,       // { device_id, config_revision, settings: [...] }
    _dirty: new Map(),     // setting_id -> { original, current }
    _operationId: null,
    _operationDeviceId: null,
    _savePending: false,
    _pendingWsEvent: null,
    _staleRevision: false,

    /* ── Public lifecycle ──────────────────────────────────────────── */

    async load(deviceId) {
        this._clear();
        this._renderEmpty();

        try {
            const data = await api.request(
                `/api/devices/settings?device_id=${encodeURIComponent(deviceId)}`
            );
            this._baseline = data;
            this._dirty.clear();
            this._staleRevision = false;
            this._render(data);
        } catch (err) {
            const msg = String(err.message || err);
            if (msg.includes('settings_unsupported') ||
                msg.includes('settings_not_ready') ||
                msg.includes('settings_discovering')) {
                this._renderNotAvailable(msg.includes('settings_discovering')
                    ? 'discovering' : 'unsupported');
            } else {
                this._renderError(msg);
            }
        }
    },

    unload() {
        this._clear();
    },

    /* ── WS event handling ─────────────────────────────────────────── */

    onSettingsState(ev) {
        if (!state.selectedDeviceDetail || ev.deviceId !== state.selectedDeviceDetail.id) return;
        /* settings.changed, not settings.state, is the snapshot refresh
         * signal.  In particular, READY may precede the terminal event of a
         * post-reboot transaction; loading here would discard operationId. */
        if ((ev.state === 'discovering' || ev.state === 'reading') &&
            !this._operationId && !this._savePending) {
            this._renderNotAvailable('discovering');
        }
    },

    onSettingsChanged(ev) {
        if (!state.selectedDeviceDetail) return;
        if (ev.deviceId !== state.selectedDeviceDetail.id) return;

        // If we have a baseline and revision changed, mark stale
        if (this._baseline && ev.configRevision !== undefined) {
            if (ev.configRevision !== this._baseline.config_revision) {
                this._staleRevision = true;
                this._updateStaleBanner();
            }
        }

        // REST remains the source of truth; this event asks us to refresh it.
        if (!this._operationId && !this._savePending) void this.load(ev.deviceId);
    },

    onConnectionChanged(connected) {
        const banner = document.getElementById('ds-offline-banner');
        if (banner) {
            banner.classList.toggle('hidden', connected);
        }
    },

    /* ── Dirty model ───────────────────────────────────────────────── */

    _markDirty(settingId, currentValue) {
        if (!this._baseline) return;
        const setting = this._baseline.settings.find(s => s.id === settingId);
        if (!setting || setting.readonly || setting.type === 'secret') return;

        const original = setting.value;
        if (JSON.stringify(currentValue) === JSON.stringify(original)) {
            this._dirty.delete(settingId);
        } else {
            this._dirty.set(settingId, { original, current: currentValue });
        }
        this._updateSaveButton();
    },

    _hasChanges() {
        return this._dirty.size > 0;
    },

    /* ── Save lifecycle ────────────────────────────────────────────── */

    async save() {
        if (!this._baseline || !this._hasChanges()) return;
        const device = state.selectedDeviceDetail;
        if (!device) return;

        const changes = [];
        for (const [id, entry] of this._dirty) {
            changes.push({ id, value: entry.current });
        }

        const btn = document.getElementById('ds-save-btn');
        this._setButtonLoading(btn, true);
        this._savePending = true;
        this._operationDeviceId = device.id;
        this._pendingWsEvent = null;
        this._renderOperationState('queued');

        try {
            const result = await api.request('/api/devices/settings', {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    device_id: device.id,
                    expected_revision: this._baseline.config_revision,
                    changes
                })
            });

            this._operationId = String(result.operation_id);
            this._savePending = false;
            this._dirty.clear();
            this._updateSaveButton();
            this._renderOperationState('queued');
            ui.showToast(i18n.t('device_settings.save_queued'), 'success');

            // Transaction progress and completion arrive through /ws/events.
            if (this._pendingWsEvent) {
                const earlyEvent = this._pendingWsEvent;
                this._pendingWsEvent = null;
                this.onTransaction(earlyEvent);
            }
        } catch (err) {
            this._operationId = null;
            this._operationDeviceId = null;
            this._savePending = false;
            this._pendingWsEvent = null;
            const msg = String(err.message || err);
            if (msg.includes('active_transaction')) {
                ui.showToast(i18n.t('device_settings.save_busy'), 'error');
            } else if (msg.includes('validation_failed')) {
                ui.showToast(i18n.t('device_settings.save_validation'), 'error');
            } else {
                ui.showToast(`${i18n.t('device_settings.save_failed')}: ${msg}`, 'error');
            }
        } finally {
            this._setButtonLoading(btn, false);
        }
    },

    onTransaction(ev) {
        const device = state.selectedDeviceDetail;
        if (!device || ev.deviceId !== device.id) return;

        // A fast device can publish before PUT's JSON response arrives.
        if (this._savePending && !this._operationId &&
            ev.deviceId === this._operationDeviceId) {
            this._pendingWsEvent = ev;
            return;
        }
        if (!this._operationId ||
            String(ev.operationId) !== String(this._operationId)) return;

        const name = typeof ev.state === 'string' ? ev.state : 'unknown';
        this._renderOperationState(name, ev.current, ev.total);
        if (['succeeded', 'failed', 'conflict', 'cancelled', 'outcome_unknown'].includes(name)) {
            this._operationId = null;
            this._operationDeviceId = null;
            // A following settings.changed event refreshes the REST snapshot.
        }
    },

    /* ── Rendering ─────────────────────────────────────────────────── */

    _render(data) {
        const card = document.getElementById('device-settings-card');
        if (!card) return;

        const container = document.getElementById('ds-settings-container');
        const banner = document.getElementById('ds-stale-banner');
        const offlineBanner = document.getElementById('ds-offline-banner');
        if (!container) return;

        container.replaceChildren();
        banner?.classList.add('hidden');

        // Offline banner
        const device = state.selectedDeviceDetail;
        if (offlineBanner) {
            offlineBanner.classList.toggle('hidden', device?.status === 'online');
        }

        // Group settings
        const groups = new Map();
        const ungrouped = [];

        for (const setting of data.settings) {
            const group = setting.group || '';
            if (group) {
                if (!groups.has(group)) groups.set(group, []);
                groups.get(group).push(setting);
            } else {
                ungrouped.push(setting);
            }
        }

        // Render ungrouped first, then groups
        for (const setting of ungrouped) {
            container.appendChild(this._renderSetting(setting));
        }

        for (const [groupName, items] of groups) {
            const section = document.createElement('div');
            section.className = 'space-y-3';
            if (ungrouped.length > 0 || [...groups.keys()].indexOf(groupName) > 0) {
                section.classList.add('pt-3', 'border-t', 'border-gray-100');
            }

            const header = document.createElement('h4');
            header.className = 'text-xs font-semibold uppercase tracking-wide text-gray-500';
            header.textContent = groupName;
            section.appendChild(header);

            for (const setting of items) {
                section.appendChild(this._renderSetting(setting));
            }
            container.appendChild(section);
        }

        // Show card, update meta
        card.classList.remove('hidden');
        const meta = document.getElementById('ds-meta');
        if (meta) {
            meta.textContent = i18n.t('device_settings.revision')
                .replace('{n}', data.config_revision);
        }

        this._updateSaveButton();
    },

    _renderSetting(setting) {
        const row = document.createElement('div');
        row.className = 'flex flex-col sm:flex-row sm:items-center justify-between gap-2 py-2';
        row.dataset.settingId = setting.id;

        // Label
        const labelWrap = document.createElement('div');
        labelWrap.className = 'min-w-0 flex-1';
        const label = document.createElement('label');
        label.className = 'text-sm font-medium text-gray-800 block';
        label.setAttribute('for', `ds-input-${setting.id}`);
        label.textContent = setting.title || setting.id;
        labelWrap.appendChild(label);

        if (setting.unit) {
            const unit = document.createElement('span');
            unit.className = 'text-xs text-gray-500 ml-1';
            unit.textContent = setting.unit;
            labelWrap.appendChild(unit);
        }

        if (setting.readonly) {
            const badge = document.createElement('span');
            badge.className = 'text-xs text-gray-400 ml-1';
            badge.textContent = i18n.t('device_settings.readonly');
            labelWrap.appendChild(badge);
        }

        row.appendChild(labelWrap);

        // Control
        const control = document.createElement('div');
        control.className = 'flex-shrink-0 sm:w-48';

        switch (setting.type) {
            case 'boolean':
                control.appendChild(this._renderBool(setting));
                break;
            case 'integer':
                control.appendChild(this._renderInteger(setting));
                break;
            case 'number':
                control.appendChild(this._renderNumber(setting));
                break;
            case 'string':
                control.appendChild(this._renderString(setting));
                break;
            case 'enum':
                control.appendChild(this._renderEnum(setting));
                break;
            case 'secret':
                control.appendChild(this._renderSecret(setting));
                break;
            default:
                control.appendChild(this._renderFallback(setting));
        }

        row.appendChild(control);
        return row;
    },

    /* ── Type renderers ────────────────────────────────────────────── */

    _renderBool(setting) {
        const wrap = document.createElement('label');
        wrap.className = 'settings-switch';
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.id = `ds-input-${setting.id}`;
        input.checked = setting.value === true;
        input.disabled = setting.readonly;
        input.setAttribute('role', 'switch');
        input.onchange = () => {
            this._markDirty(setting.id, input.checked);
        };
        const track = document.createElement('span');
        track.className = 'settings-switch-track';
        wrap.append(input, track);
        return wrap;
    },

    _renderInteger(setting) {
        const input = document.createElement('input');
        input.type = 'number';
        input.id = `ds-input-${setting.id}`;
        input.value = setting.value ?? '';
        input.min = setting.minimum ?? '';
        input.max = setting.maximum ?? '';
        input.step = setting.step ?? 1;
        input.className = 'w-full px-3 py-1.5 border border-gray-300 rounded-lg text-sm';
        input.disabled = setting.readonly;
        input.onchange = () => {
            const val = parseInt(input.value, 10);
            if (!isNaN(val)) this._markDirty(setting.id, val);
        };
        return input;
    },

    _renderNumber(setting) {
        const input = document.createElement('input');
        input.type = 'number';
        input.id = `ds-input-${setting.id}`;
        input.value = setting.value ?? '';
        input.min = setting.minimum ?? '';
        input.max = setting.maximum ?? '';
        input.step = setting.step ?? 'any';
        input.className = 'w-full px-3 py-1.5 border border-gray-300 rounded-lg text-sm';
        input.disabled = setting.readonly;
        input.onchange = () => {
            const val = parseFloat(input.value);
            if (!isNaN(val)) this._markDirty(setting.id, val);
        };
        return input;
    },

    _renderString(setting) {
        const input = document.createElement('input');
        input.type = 'text';
        input.id = `ds-input-${setting.id}`;
        input.value = setting.value ?? '';
        input.className = 'w-full px-3 py-1.5 border border-gray-300 rounded-lg text-sm';
        input.disabled = setting.readonly;
        input.oninput = () => {
            this._markDirty(setting.id, input.value);
        };
        return input;
    },

    _renderEnum(setting) {
        const select = document.createElement('select');
        select.id = `ds-input-${setting.id}`;
        select.className = 'w-full px-3 py-1.5 border border-gray-300 rounded-lg text-sm bg-white';
        select.disabled = setting.readonly;

        if (setting.options && Array.isArray(setting.options)) {
            for (const opt of setting.options) {
                const optEl = document.createElement('option');
                if (typeof opt === 'object') {
                    optEl.value = opt.value;
                    optEl.textContent = opt.label || opt.value;
                } else {
                    optEl.value = opt;
                    optEl.textContent = opt;
                }
                select.appendChild(optEl);
            }
        }

        select.value = setting.value ?? '';
        select.onchange = () => {
            this._markDirty(setting.id, select.value);
        };
        return select;
    },

    _renderSecret(setting) {
        const wrap = document.createElement('div');
        wrap.className = 'space-y-2';

        const configured = setting.configured === true;

        if (configured) {
            // Show masked + KEEP/SET/CLEAR controls
            const masked = document.createElement('div');
            masked.className = 'flex items-center gap-2';
            const dots = document.createElement('span');
            dots.className = 'text-sm text-gray-500 font-mono tracking-wider';
            dots.textContent = '\u2022\u2022\u2022\u2022\u2022\u2022';
            masked.appendChild(dots);
            wrap.appendChild(masked);

            const actions = document.createElement('div');
            actions.className = 'flex gap-1';

            const replaceBtn = document.createElement('button');
            replaceBtn.type = 'button';
            replaceBtn.className = 'text-xs px-2 py-1 rounded bg-gray-100 text-gray-600 hover:bg-gray-200';
            replaceBtn.textContent = i18n.t('device_settings.secret_replace');
            replaceBtn.onclick = () => this._showSecretReplace(setting, wrap);

            const clearBtn = document.createElement('button');
            clearBtn.type = 'button';
            clearBtn.className = 'text-xs px-2 py-1 rounded bg-red-50 text-red-600 hover:bg-red-100';
            clearBtn.textContent = i18n.t('device_settings.secret_clear');
            clearBtn.onclick = () => this._markDirty(setting.id, { action: 'clear' });

            actions.append(replaceBtn, clearBtn);
            wrap.appendChild(actions);
        } else {
            // New secret — show input
            const input = document.createElement('input');
            input.type = 'password';
            input.id = `ds-input-${setting.id}`;
            input.placeholder = i18n.t('device_settings.secret_new_placeholder');
            input.className = 'w-full px-3 py-1.5 border border-gray-300 rounded-lg text-sm';
            input.oninput = () => {
                if (input.value) {
                    this._markDirty(setting.id, { action: 'set', value: input.value });
                } else {
                    this._dirty.delete(setting.id);
                    this._updateSaveButton();
                }
            };
            wrap.appendChild(input);
        }

        return wrap;
    },

    _showSecretReplace(setting, container) {
        // Replace masked view with input
        container.replaceChildren();
        const input = document.createElement('input');
        input.type = 'password';
        input.id = `ds-input-${setting.id}`;
        input.placeholder = i18n.t('device_settings.secret_new_placeholder');
        input.className = 'w-full px-3 py-1.5 border border-gray-300 rounded-lg text-sm';
        input.oninput = () => {
            if (input.value) {
                this._markDirty(setting.id, { action: 'set', value: input.value });
            } else {
                this._dirty.delete(setting.id);
                this._updateSaveButton();
            }
        };

        const cancelBtn = document.createElement('button');
        cancelBtn.type = 'button';
        cancelBtn.className = 'text-xs px-2 py-1 rounded bg-gray-100 text-gray-600 hover:bg-gray-200 mt-1';
        cancelBtn.textContent = i18n.t('device_settings.cancel');
        cancelBtn.onclick = () => this.load(state.selectedDeviceDetail?.id);

        container.append(input, cancelBtn);
        input.focus();
    },

    _renderFallback(setting) {
        const span = document.createElement('span');
        span.className = 'text-sm text-gray-500';
        span.textContent = setting.value !== undefined ? String(setting.value) : '\u2014';
        return span;
    },

    /* ── UI helpers ────────────────────────────────────────────────── */

    _renderEmpty() {
        const container = document.getElementById('ds-settings-container');
        if (container) container.replaceChildren();
        const card = document.getElementById('device-settings-card');
        if (card) card.classList.add('hidden');
        this._operationId = null;
        this._operationDeviceId = null;
        this._savePending = false;
        this._pendingWsEvent = null;
    },

    _renderNotAvailable(reason) {
        const card = document.getElementById('device-settings-card');
        if (!card) return;
        card.classList.remove('hidden');
        const container = document.getElementById('ds-settings-container');
        if (!container) return;
        container.replaceChildren();

        const msg = document.createElement('p');
        msg.className = 'text-sm text-gray-500';
        msg.textContent = reason === 'discovering'
            ? i18n.t('device_settings.discovering')
            : i18n.t('device_settings.unsupported');
        container.appendChild(msg);
    },

    _renderError(message) {
        const container = document.getElementById('ds-settings-container');
        if (!container) return;
        container.replaceChildren();

        const msg = document.createElement('p');
        msg.className = 'text-sm text-red-600';
        msg.textContent = `${i18n.t('device_settings.load_failed')}: ${message}`;
        container.appendChild(msg);
    },

    _renderOperationState(state) {
        const el = document.getElementById('ds-operation-status');
        if (!el) return;

        const stateMap = {
            queued:          ['text-blue-600', 'device_settings.op_queued'],
            updating:        ['text-blue-600', 'device_settings.op_updating'],
            validating:      ['text-blue-600', 'device_settings.op_updating'],
            starting:        ['text-blue-600', 'device_settings.op_updating'],
            applying:        ['text-blue-600', 'device_settings.op_updating'],
            committing:      ['text-blue-600', 'device_settings.op_updating'],
            confirming:      ['text-blue-600', 'device_settings.op_updating'],
            waiting_reboot:  ['text-amber-600', 'device_settings.op_rebooting'],
            reconnecting:    ['text-amber-600', 'device_settings.op_rebooting'],
            verifying:       ['text-blue-600', 'device_settings.op_verifying'],
            succeeded:       ['text-green-600', 'device_settings.op_succeeded'],
            failed:          ['text-red-600', 'device_settings.op_failed'],
            conflict:        ['text-amber-600', 'device_settings.op_conflict'],
            unknown:         ['text-gray-600', 'device_settings.op_unknown']
        };

        const [colorClass, i18nKey] = stateMap[state] || stateMap.unknown;
        el.className = `text-xs font-semibold ${colorClass}`;
        el.textContent = i18n.t(i18nKey);
        el.classList.remove('hidden');
    },

    _updateStaleBanner() {
        const banner = document.getElementById('ds-stale-banner');
        if (banner) {
            banner.classList.toggle('hidden', !this._staleRevision);
        }
    },

    _updateSaveButton() {
        const btn = document.getElementById('ds-save-btn');
        if (!btn) return;
        btn.disabled = !this._hasChanges() || this._staleRevision;
    },

    _setButtonLoading(btn, loading) {
        if (!btn) return;
        if (loading) {
            btn.dataset.origHtml = btn.innerHTML;
            btn.innerHTML = `<i class="ph ph-spinner animate-spin mr-1.5"></i> ${i18n.t('device_settings.saving')}`;
            btn.disabled = true;
        } else {
            btn.innerHTML = btn.dataset.origHtml || i18n.t('device_settings.save');
            btn.disabled = !this._hasChanges();
        }
    },

    _clear() {
        this._baseline = null;
        this._dirty.clear();
        this._staleRevision = false;
    }
};
