/* ── feature_ui.js — Semantic feature presentation engine ────────────── *
 *  Responsible for:                                                     *
 *    • presentation registry & resolution                                *
 *    • feature grouping                                                 *
 *    • icon mapping                                                     *
 *    • card renderer dispatch                                           *
 *    • numeric scaling & formatting                                     *
 *    • incremental card updates                                         *
 *    • control pending state                                            *
 *    • connection / offline visual state                                *
 *                                                                       *
 *  NOT responsible for:                                                 *
 *    fetch API, BLE commands, WebSocket subscription, device store,     *
 *    schema refresh, MCP                                                *
 * ────────────────────────────────────────────────────────────────────── */

const featureUi = {

    /* ── Card identity map ──────────────────────────────────────────── */

    cardByFeatureKey: new Map(),

    featureKey(feature) {
        return `${feature.feature_id}:${feature.property_id}`;
    },

    /* ── Value kind resolver ────────────────────────────────────────── */

    valueKind(feature) {
        if (feature.value_type === 1) return 'bool';
        if (feature.value_type === 2) return 'int';
        return feature.semantic?.value_type || 'none';
    },

    /* ── Numeric helpers ────────────────────────────────────────────── */

    scaleOf(feature) {
        return 10 ** Math.max(0, Number(feature?.decimals) || 0);
    },

    rawToDisplay(feature, raw) {
        return Number(raw) / this.scaleOf(feature);
    },

    displayToRaw(feature, value) {
        return Math.round(Number(value) * this.scaleOf(feature));
    },

    formatNumeric(feature, raw) {
        const decimals = Math.max(0, Number(feature.decimals) || 0);
        const value = this.rawToDisplay(feature, raw);
        return `${value.toFixed(decimals)}${feature.unit ? ` ${feature.unit}` : ''}`;
    },

    /* ── Presentation registry ──────────────────────────────────────── */

    groupOrder: ['controls', 'environment', 'settings', 'other'],

    groupLabels: {
        controls: 'feature_group.controls',
        environment: 'feature_group.environment',
        settings: 'feature_group.settings',
        other: 'feature_group.other'
    },

    presentations: {
        'relay:on_off':        { group: 'controls',    renderer: 'toggle',         icon: 'power' },
        'outlet:on_off':       { group: 'controls',    renderer: 'toggle',         icon: 'plugs' },
        'light:on_off':        { group: 'controls',    renderer: 'toggle',         icon: 'lightbulb' },
        'light:level':         { group: 'controls',    renderer: 'range',          icon: 'lightbulb' },
        'fan:percent_setting': { group: 'controls',    renderer: 'range',          icon: 'devices' },

        'temperature:temperature': { group: 'environment', renderer: 'metric',     icon: 'thermometer' },
        'humidity:humidity':   { group: 'environment', renderer: 'metric',         icon: 'info' },
        'contact:contact':     { group: 'environment', renderer: 'contact',        icon: 'info' },

        'value:value':         { group: 'settings',    renderer: 'generic-number', icon: 'info' }
    },

    /* ── Resolution ─────────────────────────────────────────────────── */

    presentationKey(feature) {
        const name = feature.semantic?.name || 'unknown';
        const property = feature.semantic?.property || 'unknown';
        return `${name}:${property}`;
    },

    resolvePresentation(feature) {
        const key = this.presentationKey(feature);

        if (this.presentations[key]) {
            const base = { ...this.presentations[key] };

            // Generic VALUE read-only → other/monitoring, not settings
            if (key === 'value:value' &&
                feature.control?.writable !== true) {
                base.group = 'other';
                base.renderer = 'metric';
            }
            return base;
        }

        // Fallback for unknown semantics
        const kind = this.valueKind(feature);
        const writable = feature.control?.writable === true;

        if (kind === 'bool' && writable) {
            return { group: 'controls', renderer: 'toggle', icon: 'power' };
        }
        if (kind === 'int' && writable) {
            return { group: 'settings', renderer: 'generic-number', icon: 'info' };
        }
        if (kind === 'int') {
            return { group: 'other', renderer: 'metric', icon: 'info' };
        }

        return { group: 'other', renderer: 'fallback', icon: 'info' };
    },

    /* ── Grouping ───────────────────────────────────────────────────── */

    groupFeatures(features) {
        const groups = {
            controls: [],
            environment: [],
            settings: [],
            other: []
        };

        for (const feature of features) {
            const presentation = this.resolvePresentation(feature);
            groups[presentation.group].push(feature);
        }

        return groups;
    },

    /* ── Public render API ──────────────────────────────────────────── */

    renderFeatures({ container, features, device, onToggle, onNumericSet }) {
        container.replaceChildren();
        this.cardByFeatureKey.clear();

        if (!features || !features.length) {
            this.renderEmpty(container, device);
            return;
        }

        const groups = this.groupFeatures(features);

        for (const groupKey of this.groupOrder) {
            const groupFeatures = groups[groupKey];
            if (!groupFeatures.length) continue;
            const section = this._renderGroup(groupKey, groupFeatures, device, {
                onToggle,
                onNumericSet
            });
            container.appendChild(section);
        }
    },

    renderLoading(container) {
        container.replaceChildren();
        this.cardByFeatureKey.clear();

        const el = document.createElement('div');
        el.className = 'flex items-center justify-center py-8 text-sm text-gray-500';
        el.innerHTML = `<i class="ph ph-spinner animate-spin mr-2"></i>${i18n.t('device_detail.loading_features')}`;
        container.appendChild(el);
    },

    renderEmpty(container, device) {
        const el = document.createElement('div');
        el.className = 'rounded-lg border border-dashed border-gray-300 p-6 text-center';
        el.innerHTML = `<i class="ph ph-plugs text-2xl text-gray-400"></i>` +
            `<h4 class="mt-2 text-sm font-semibold text-gray-800">${i18n.t('device_detail.no_features')}</h4>` +
            `<p class="mt-1 text-xs text-gray-500">${i18n.t('device_detail.no_features_desc')}</p>`;

        if (device) {
            const retry = document.createElement('button');
            retry.className = 'mt-4 px-4 py-2 bg-brand-50 text-brand-700 rounded-lg hover:bg-brand-100 text-sm font-medium';
            retry.textContent = i18n.t('device_detail.refresh_schema');
            retry.onclick = () => devices.refreshSchema(device);
            el.appendChild(retry);
        }

        container.appendChild(el);
    },

    renderError(container, error, device) {
        container.replaceChildren();
        this.cardByFeatureKey.clear();

        const el = document.createElement('div');
        el.className = 'rounded-lg border border-red-200 bg-red-50 p-5';
        el.innerHTML = `<h4 class="text-sm font-semibold text-red-800">${i18n.t('device_detail.schema_load_error')}</h4>` +
            `<p class="text-xs text-red-700 mt-1 break-words">${escapeHtml(error.message || String(error))}</p>`;

        if (device) {
            const retry = document.createElement('button');
            retry.className = 'mt-3 px-3 py-2 bg-white border border-red-200 text-red-700 rounded-lg hover:bg-red-100 text-sm font-medium';
            retry.textContent = i18n.t('device_detail.retry');
            retry.onclick = () => devices.loadDetail(device);
            el.appendChild(retry);
        }

        container.appendChild(el);
    },

    /* ── Incremental update ─────────────────────────────────────────── */

    updateFeatureState(feature, device) {
        const key = this.featureKey(feature);
        const card = this.cardByFeatureKey.get(key);
        if (!card) return false;

        const presentation = this.resolvePresentation(feature);
        const stateSlot = card.querySelector('[data-role="feature-state"]');
        const controlSlot = card.querySelector('[data-role="feature-control"]');

        switch (presentation.renderer) {
        case 'toggle':
            this._updateToggle(controlSlot, stateSlot, feature);
            break;
        case 'range':
            this._updateRange(controlSlot, stateSlot, feature);
            break;
        case 'metric':
            this._updateMetric(stateSlot, feature);
            break;
        case 'contact':
            this._updateContact(stateSlot, feature);
            break;
        case 'generic-number':
            this._updateGenericNumber(controlSlot, stateSlot, feature);
            break;
        }
        return true;
    },

    updateConnectionState(features, device) {
        const isOffline = device.status !== 'online';

        for (const feature of features) {
            const key = this.featureKey(feature);
            const card = this.cardByFeatureKey.get(key);
            if (!card) continue;

            const controlSlot = card.querySelector('[data-role="feature-control"]');
            const stateSlot = card.querySelector('[data-role="feature-state"]');

            // Disable all interactive controls when offline
            const interactiveEls = controlSlot?.querySelectorAll('input, button');
            if (interactiveEls) {
                interactiveEls.forEach(el => { el.disabled = isOffline; });
            }

            // For metric/contact cards, append offline indicator when offline
            if (isOffline && stateSlot) {
                const existing = stateSlot.querySelector('.feature-offline-badge');
                if (!existing) {
                    const badge = document.createElement('div');
                    badge.className = 'feature-offline-badge text-xs text-gray-400 mt-1';
                    badge.textContent = i18n.t('feature_state.offline');
                    stateSlot.appendChild(badge);
                }
            } else if (!isOffline && stateSlot) {
                // Remove offline badge when reconnected
                const badge = stateSlot.querySelector('.feature-offline-badge');
                if (badge) badge.remove();
            }
        }
    },

    setPending(feature, pending) {
        const key = this.featureKey(feature);
        const card = this.cardByFeatureKey.get(key);
        if (!card) return;

        const controlSlot = card.querySelector('[data-role="feature-control"]');
        if (!controlSlot) return;

        const interactiveEls = controlSlot.querySelectorAll('input, button');
        interactiveEls.forEach(el => { el.disabled = pending; });
    },

    /* ── Private: group rendering ───────────────────────────────────── */

    _renderGroup(groupKey, features, device, callbacks) {
        const section = document.createElement('section');
        section.className = 'mb-6 last:mb-0';

        // Group header
        const header = document.createElement('div');
        header.className = 'flex items-center gap-2 mb-3';

        const label = document.createElement('h4');
        label.className = 'text-xs font-semibold text-gray-500 uppercase tracking-wide';
        label.textContent = i18n.t(this.groupLabels[groupKey] || groupKey);
        header.appendChild(label);

        const count = document.createElement('span');
        count.className = 'text-xs text-gray-400';
        count.textContent = features.length;
        header.appendChild(count);

        section.appendChild(header);

        // Grid
        const grid = document.createElement('div');
        grid.className = 'grid grid-cols-1 md:grid-cols-2 gap-3';

        for (const feature of features) {
            const card = this._renderCard(feature, device, callbacks);
            grid.appendChild(card);
            this.cardByFeatureKey.set(this.featureKey(feature), card);
        }

        section.appendChild(grid);
        return section;
    },

    /* ── Private: card shell ────────────────────────────────────────── */

    _renderCard(feature, device, callbacks) {
        const presentation = this.resolvePresentation(feature);

        const card = document.createElement('article');
        card.dataset.featureKey = this.featureKey(feature);
        card.className = 'rounded-xl border border-gray-200 bg-white p-4 shadow-sm';

        // Header row
        const headerRow = document.createElement('div');
        headerRow.className = 'flex items-start justify-between gap-3';

        // Left: icon + title
        const left = document.createElement('div');
        left.className = 'flex items-center gap-3';

        const iconWrap = document.createElement('div');
        iconWrap.className = 'w-8 h-8 rounded-lg bg-gray-100 flex items-center justify-center flex-shrink-0';
        iconWrap.innerHTML = `<i class="ph ph-${presentation.icon} text-gray-600"></i>`;
        left.appendChild(iconWrap);

        const textWrap = document.createElement('div');
        const titleEl = document.createElement('h5');
        titleEl.className = 'text-sm font-semibold text-gray-800';
        titleEl.textContent = feature.title || feature.semantic?.name || feature.feature_id;
        titleEl.title = feature.feature_id;
        textWrap.appendChild(titleEl);
        left.appendChild(textWrap);

        headerRow.appendChild(left);

        // Right: state slot
        const stateSlot = document.createElement('div');
        stateSlot.dataset.role = 'feature-state';
        stateSlot.className = 'flex-shrink-0';
        headerRow.appendChild(stateSlot);

        card.appendChild(headerRow);

        // Control slot
        const controlSlot = document.createElement('div');
        controlSlot.dataset.role = 'feature-control';
        controlSlot.className = 'mt-3';
        card.appendChild(controlSlot);

        // Dispatch to renderer
        this._dispatchRenderer(presentation, feature, device, stateSlot, controlSlot, callbacks);

        return card;
    },

    /* ── Private: renderer dispatch ─────────────────────────────────── */

    _dispatchRenderer(presentation, feature, device, stateSlot, controlSlot, callbacks) {
        switch (presentation.renderer) {
        case 'toggle':
            this._renderToggle(feature, device, controlSlot, stateSlot, callbacks);
            break;
        case 'range':
            this._renderRange(feature, device, controlSlot, stateSlot, callbacks);
            break;
        case 'metric':
            this._renderMetric(feature, device, stateSlot);
            break;
        case 'contact':
            this._renderContact(feature, device, stateSlot);
            break;
        case 'generic-number':
            this._renderGenericNumber(feature, device, controlSlot, stateSlot, callbacks);
            break;
        default:
            this._renderFallback(feature, device, controlSlot);
            break;
        }
    },

    /* ── Private: toggle renderer ───────────────────────────────────── */

    _renderToggle(feature, device, controlSlot, stateSlot, callbacks) {
        const isOn = feature.state?.valid && feature.state.value_bool;
        const disabled = device.status !== 'online';
        const writable = feature.control?.writable && feature.control.write_command;

        // State badge
        const stateBadge = document.createElement('span');
        stateBadge.className = 'text-xs font-medium ' +
            (isOn ? 'text-green-600' : 'text-gray-500');
        stateBadge.textContent = i18n.t(isOn ? 'feature_state.on' : 'feature_state.off');
        stateSlot.appendChild(stateBadge);

        if (!writable) return;

        // Accessible switch using .settings-switch from shell.html
        const label = document.createElement('label');
        label.className = 'settings-switch';
        label.title = feature.feature_id;

        const input = document.createElement('input');
        input.type = 'checkbox';
        input.role = 'switch';
        input.checked = !!isOn;
        input.disabled = disabled;
        input.setAttribute('aria-checked', String(!!isOn));

        const track = document.createElement('span');
        track.className = 'settings-switch-track';

        label.append(input, track);
        controlSlot.appendChild(label);

        input.addEventListener('change', () => {
            if (callbacks?.onToggle) callbacks.onToggle(feature);
        });

        // Store reference for incremental update
        controlSlot._toggleInput = input;
        controlSlot._stateBadge = stateBadge;
    },

    /* ── Private: range renderer ────────────────────────────────────── */

    _renderRange(feature, device, controlSlot, stateSlot, callbacks) {
        const writable = feature.control?.writable && feature.control.write_command;
        const min = this.rawToDisplay(feature, feature.control.minimum);
        const max = this.rawToDisplay(feature, feature.control.maximum);
        const step = this.rawToDisplay(feature, feature.control.step);
        const current = feature.state?.valid
            ? this.rawToDisplay(feature, feature.state.value_int) : min;
        const disabled = device.status !== 'online';

        // Value label
        const valueLabel = document.createElement('div');
        valueLabel.className = 'flex items-baseline justify-between mb-2';
        valueLabel.innerHTML =
            `<span class="text-lg font-semibold text-gray-800">${this.formatNumeric(feature, feature.state?.valid ? feature.state.value_int : 0)}</span>` +
            `<span class="text-xs text-gray-400 font-mono">${min} – ${max}</span>`;
        stateSlot.appendChild(valueLabel);

        if (!writable) return;

        // Slider
        const sliderRow = document.createElement('div');
        sliderRow.className = 'flex items-center gap-3';

        const range = document.createElement('input');
        range.type = 'range';
        range.min = min;
        range.max = max;
        range.step = step;
        range.value = current;
        range.disabled = disabled;
        range.className = 'flex-1 accent-brand-600';
        range.setAttribute('aria-label', feature.title || feature.feature_id);

        sliderRow.appendChild(range);
        controlSlot.appendChild(sliderRow);

        // Slider interaction guard — prevent WS updates from jumping the slider while dragging
        range.addEventListener('pointerdown', () => {
            range.dataset.userEditing = 'true';
        });
        range.addEventListener('pointerup', () => {
            delete range.dataset.userEditing;
        });
        range.addEventListener('change', () => {
            delete range.dataset.userEditing;
        });

        // Preview on input, send on change
        range.addEventListener('input', () => {
            valueLabel.querySelector('span').textContent =
                this.formatNumeric(feature, this.displayToRaw(feature, Number(range.value)));
        });

        range.addEventListener('change', async () => {
            if (callbacks?.onNumericSet) {
                await callbacks.onNumericSet(feature, Number(range.value));
            }
        });

        // Store for incremental update
        controlSlot._range = range;
        controlSlot._valueLabel = valueLabel;
    },

    /* ── Private: metric renderer ───────────────────────────────────── */

    _renderMetric(feature, device, stateSlot) {
        const valueEl = document.createElement('div');
        valueEl.className = 'text-center py-2';

        if (feature.state?.valid) {
            const kind = this.valueKind(feature);
            let display;
            if (kind === 'bool') {
                display = feature.state.value_bool
                    ? i18n.t('feature_state.on')
                    : i18n.t('feature_state.off');
            } else if (kind === 'int' && Number.isFinite(feature.state.value_int)) {
                display = this.formatNumeric(feature, feature.state.value_int);
            } else {
                display = '—';
            }
            valueEl.innerHTML =
                `<div class="text-2xl font-bold text-gray-800">${escapeHtml(display)}</div>` +
                `<div class="text-xs text-gray-400 mt-1">${i18n.t('feature_state.updated_now')}</div>`;
        } else {
            valueEl.innerHTML =
                `<div class="text-2xl font-bold text-gray-300">—</div>` +
                `<div class="text-xs text-gray-400 mt-1">${i18n.t('feature_state.unknown')}</div>`;
        }

        stateSlot.appendChild(valueEl);

        // Store for incremental update
        stateSlot._valueEl = valueEl;
    },

    /* ── Private: contact renderer ──────────────────────────────────── */

    _renderContact(feature, device, stateSlot) {
        const isOpen = feature.state?.valid && feature.state.value_bool === true;
        const label = isOpen
            ? i18n.t('feature_state.open')
            : i18n.t('feature_state.closed');
        const colorClass = isOpen ? 'text-amber-600' : 'text-green-600';

        const indicator = document.createElement('div');
        indicator.className = 'flex items-center gap-2';
        indicator.innerHTML =
            `<span class="inline-block w-2.5 h-2.5 rounded-full ${isOpen ? 'bg-amber-500' : 'bg-green-500'}"></span>` +
            `<span class="text-sm font-semibold ${colorClass}">${label}</span>`;
        stateSlot.appendChild(indicator);

        // Store for incremental update
        stateSlot._indicator = indicator;
    },

    /* ── Private: generic number renderer ───────────────────────────── */

    _renderGenericNumber(feature, device, controlSlot, stateSlot, callbacks) {
        const min = this.rawToDisplay(feature, feature.control.minimum);
        const max = this.rawToDisplay(feature, feature.control.maximum);
        const step = this.rawToDisplay(feature, feature.control.step);
        const current = feature.state?.valid
            ? this.rawToDisplay(feature, feature.state.value_int) : min;
        const disabled = device.status !== 'online';
        const writable = feature.control?.writable && feature.control.write_command;

        // Current value display
        const valueLabel = document.createElement('div');
        valueLabel.className = 'text-lg font-semibold text-gray-800 mb-2';
        valueLabel.textContent = this.formatNumeric(feature, feature.state?.valid ? feature.state.value_int : 0);
        stateSlot.appendChild(valueLabel);

        if (!writable) return;

        // Slider
        const range = document.createElement('input');
        range.type = 'range';
        range.min = min;
        range.max = max;
        range.step = step;
        range.value = current;
        range.disabled = disabled;
        range.className = 'w-full accent-brand-600 mb-2';
        range.setAttribute('aria-label', feature.title || feature.feature_id);

        // Numeric input + Apply button row
        const inputRow = document.createElement('div');
        inputRow.className = 'flex items-center gap-2';

        const numberInput = document.createElement('input');
        numberInput.type = 'number';
        numberInput.min = min;
        numberInput.max = max;
        numberInput.step = step;
        numberInput.value = current;
        numberInput.disabled = disabled;
        numberInput.className = 'w-24 px-2 py-1 border border-gray-300 rounded text-sm font-mono';
        numberInput.setAttribute('aria-label', feature.title || feature.feature_id);

        const unitSpan = document.createElement('span');
        unitSpan.className = 'text-xs text-gray-500';
        unitSpan.textContent = feature.unit || '';

        const applyBtn = document.createElement('button');
        applyBtn.type = 'button';
        applyBtn.disabled = disabled;
        applyBtn.className = 'ml-auto px-3 py-1.5 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors text-sm font-medium disabled:opacity-50 disabled:cursor-not-allowed';
        applyBtn.textContent = i18n.t('feature_control.apply');

        inputRow.append(numberInput, unitSpan, applyBtn);
        controlSlot.append(range, inputRow);

        // Slider interaction guard
        range.addEventListener('pointerdown', () => {
            range.dataset.userEditing = 'true';
        });
        range.addEventListener('pointerup', () => {
            delete range.dataset.userEditing;
        });
        range.addEventListener('change', () => {
            delete range.dataset.userEditing;
        });

        // Sync slider ↔ number
        range.addEventListener('input', () => {
            numberInput.value = range.value;
            valueLabel.textContent = this.formatNumeric(feature, this.displayToRaw(feature, Number(range.value)));
        });
        numberInput.addEventListener('input', () => {
            range.value = numberInput.value;
            valueLabel.textContent = this.formatNumeric(feature, this.displayToRaw(feature, Number(numberInput.value)));
        });

        // Validate on raw integer before sending
        applyBtn.addEventListener('click', async () => {
            const displayVal = Number(numberInput.value);
            const raw = this.displayToRaw(feature, displayVal);

            if (!Number.isFinite(raw)) return;
            if (raw < feature.control.minimum || raw > feature.control.maximum) return;
            if ((raw - feature.control.minimum) % feature.control.step !== 0) return;

            applyBtn.disabled = true;
            try {
                if (callbacks?.onNumericSet) {
                    await callbacks.onNumericSet(feature, displayVal);
                }
            } finally {
                applyBtn.disabled = false;
            }
        });

        // Store for incremental update
        controlSlot._range = range;
        controlSlot._numberInput = numberInput;
        controlSlot._valueLabel = valueLabel;
    },

    /* ── Private: incremental update per renderer ───────────────────── */

    _updateToggle(controlSlot, stateSlot, feature) {
        const input = controlSlot?._toggleInput;
        const badge = stateSlot?._stateBadge;
        if (!input || !badge) return;

        const isOn = feature.state?.valid && feature.state.value_bool;
        input.checked = !!isOn;
        input.setAttribute('aria-checked', String(!!isOn));
        badge.textContent = i18n.t(isOn ? 'feature_state.on' : 'feature_state.off');
        badge.className = 'text-xs font-medium ' + (isOn ? 'text-green-600' : 'text-gray-500');
    },

    _updateRange(controlSlot, stateSlot, feature) {
        const range = controlSlot?._range;
        const valueLabel = stateSlot?._valueLabel;
        if (!range || !valueLabel) return;

        if (feature.state?.valid && Number.isFinite(feature.state.value_int)) {
            const displayVal = this.rawToDisplay(feature, feature.state.value_int);
            if (range.dataset.userEditing !== 'true') {
                range.value = displayVal;
            }
            valueLabel.querySelector('span').textContent =
                this.formatNumeric(feature, feature.state.value_int);
        }
    },

    _updateMetric(stateSlot, feature) {
        const valueEl = stateSlot?._valueEl;
        if (!valueEl) return;

        if (feature.state?.valid) {
            const kind = this.valueKind(feature);
            let display;
            if (kind === 'bool') {
                display = feature.state.value_bool
                    ? i18n.t('feature_state.on')
                    : i18n.t('feature_state.off');
            } else if (kind === 'int' && Number.isFinite(feature.state.value_int)) {
                display = this.formatNumeric(feature, feature.state.value_int);
            } else {
                display = '—';
            }
            valueEl.innerHTML =
                `<div class="text-2xl font-bold text-gray-800">${escapeHtml(display)}</div>` +
                `<div class="text-xs text-gray-400 mt-1">${i18n.t('feature_state.updated_now')}</div>`;
        }
    },

    _updateContact(stateSlot, feature) {
        const indicator = stateSlot?._indicator;
        if (!indicator) return;

        const isOpen = feature.state?.valid && feature.state.value_bool === true;
        const label = isOpen
            ? i18n.t('feature_state.open')
            : i18n.t('feature_state.closed');
        const colorClass = isOpen ? 'text-amber-600' : 'text-green-600';

        indicator.innerHTML =
            `<span class="inline-block w-2.5 h-2.5 rounded-full ${isOpen ? 'bg-amber-500' : 'bg-green-500'}"></span>` +
            `<span class="text-sm font-semibold ${colorClass}">${label}</span>`;
    },

    _updateGenericNumber(controlSlot, stateSlot, feature) {
        const range = controlSlot?._range;
        const numberInput = controlSlot?._numberInput;
        const valueLabel = stateSlot?._valueLabel;
        if (!valueLabel) return;

        if (feature.state?.valid && Number.isFinite(feature.state.value_int)) {
            const displayVal = this.rawToDisplay(feature, feature.state.value_int);
            valueLabel.textContent = this.formatNumeric(feature, feature.state.value_int);
            if (range && range.dataset.userEditing !== 'true') {
                range.value = displayVal;
            }
            if (numberInput && numberInput !== document.activeElement) {
                numberInput.value = displayVal;
            }
        }
    },

    /* ── Private: fallback renderer ─────────────────────────────────── */

    _renderFallback(feature, device, controlSlot) {
        const el = document.createElement('span');
        el.className = 'text-xs text-gray-400';
        el.textContent = feature.feature_id;
        controlSlot.appendChild(el);
    }
};
