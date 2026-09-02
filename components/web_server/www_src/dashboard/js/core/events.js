// --- WebSocket Realtime Event Bus ---
// Singleton: one WS connection per tab. Buffers events during startup,
// replays after snapshot, handles resync/gap/reconnect.
const events = {
    _ws: null,
    _buffer: [],
    _lastSeq: 0,
    _live: false,
    _listeners: new Map(),
    _reconnectTimer: null,
    _reconnectDelay: 1000,
    _reconnectMax: 30000,
    _reconnectAttempts: 0,
    _degraded: false,
    _resyncPending: false,

    // Initialize: connect WebSocket and begin buffering
    init() {
        if (this._ws) return;
        this._connect();
    },

    _connect() {
        const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        const url = `${proto}//${location.host}/ws/events`;

        try {
            this._ws = new WebSocket(url);
        } catch (_) {
            this._scheduleReconnect();
            return;
        }

        this._ws.onopen = () => {
            this._degraded = false;
            this._reconnectAttempts = 0;
            this._reconnectDelay = 1000;
            this._emit('ws:connected');
        };

        this._ws.onmessage = (evt) => {
            try {
                const msg = JSON.parse(evt.data);
                this._onMessage(msg);
            } catch (_) {
                // Ignore malformed messages
            }
        };

        this._ws.onclose = () => {
            this._ws = null;
            this._live = false;
            this._degraded = true;
            this._emit('ws:disconnected');
            this._scheduleReconnect();
        };

        this._ws.onerror = () => {
            // onclose will fire after onerror
        };
    },

    _scheduleReconnect() {
        if (this._reconnectTimer) return;
        const jitter = Math.random() * 500;
        const delay = Math.min(this._reconnectDelay + jitter, this._reconnectMax);
        this._reconnectTimer = setTimeout(() => {
            this._reconnectTimer = null;
            this._reconnectAttempts++;
            this._connect();
        }, delay);
        this._reconnectDelay = Math.min(this._reconnectDelay * 2, this._reconnectMax);
    },

    // Enter live mode: called by frontend after snapshot is applied
    goLive(snapshotSeq) {
        this._lastSeq = snapshotSeq || 0;
        this._live = true;
        this._replayBuffered();
    },

    _replayBuffered() {
        if (!this._live || this._buffer.length === 0) return;

        const toReplay = this._buffer
            .filter(e => e.seq > this._lastSeq)
            .sort((a, b) => a.seq - b.seq);

        this._buffer = [];

        for (const event of toReplay) {
            this._applyEvent(event);
        }
    },

    _onMessage(msg) {
        if (msg.type === 'resync.required') {
            this._resyncPending = true;
            this._live = false;
            this._emit('resync:required');
            return;
        }

        // Gap detection: expected seq was N+1 but got something else
        if (this._live && this._lastSeq !== 0 && msg.seq !== this._lastSeq + 1) {
            this._live = false;
            this._resyncPending = true;
            this._emit('resync:required');
            return;
        }

        if (this._live) {
            this._lastSeq = msg.seq;
            this._applyEvent(msg);
        } else {
            // Buffer until goLive()
            this._buffer.push(msg);
            // Bound buffer to 100 events
            if (this._buffer.length > 100) {
                this._buffer = this._buffer.slice(-64);
                this._resyncPending = true;
                this._emit('resync:required');
            }
        }
    },

    _applyEvent(event) {
        const type = event.type;
        if (!type) return;

        // Update lastSeq for live events
        if (event.seq > this._lastSeq) this._lastSeq = event.seq;

        // Fan out to registered handlers
        const handlers = this._listeners.get(type);
        if (handlers) {
            for (const handler of handlers) {
                try { handler(event); } catch (_) {}
            }
        }

        // Also emit to wildcard listeners
        const wildcards = this._listeners.get('*');
        if (wildcards) {
            for (const handler of wildcards) {
                try { handler(event); } catch (_) {}
            }
        }
    },

    // Subscribe to event type (or '*' for all)
    on(type, handler) {
        if (!this._listeners.has(type)) {
            this._listeners.set(type, new Set());
        }
        this._listeners.get(type).add(handler);
    },

    // Unsubscribe
    off(type, handler) {
        const set = this._listeners.get(type);
        if (set) set.delete(handler);
    },

    _emit(type, detail) {
        const handlers = this._listeners.get(type);
        if (handlers) {
            for (const handler of handlers) {
                try { handler(detail); } catch (_) {}
            }
        }
    },

    // Status
    get connected() { return this._ws && this._ws.readyState === WebSocket.OPEN; },
    get live() { return this._live; },
    get degraded() { return this._degraded; },
    get lastSeq() { return this._lastSeq; },

    // Shutdown
    close() {
        if (this._reconnectTimer) {
            clearTimeout(this._reconnectTimer);
            this._reconnectTimer = null;
        }
        if (this._ws) {
            this._ws.close();
            this._ws = null;
        }
    }
};
