// --- Configuration & State ---
const TAB_STORAGE_KEY = 'esp32-gateway.active-tab';
const PERSISTED_TABS = ['devices', 'scanner', 'settings'];

const loadSavedTab = () => {
    try {
        const savedTab = localStorage.getItem(TAB_STORAGE_KEY);
        return PERSISTED_TABS.includes(savedTab) ? savedTab : 'devices';
    } catch (_) {
        return 'devices';
    }
};

const saveTab = tabId => {
    if (!PERSISTED_TABS.includes(tabId)) return;
    try {
        localStorage.setItem(TAB_STORAGE_KEY, tabId);
    } catch (_) {
        // Continue navigation when storage is unavailable.
    }
};

const state = {
    activeTab: loadSavedTab(),
    devicesLoaded: false,
    isScanning: false,
    scannedDevices: [],
    connectedDevices: [],
    selectedDeviceForAdd: null,
    selectedDeviceDetail: null,
    pendingOpenDeviceId: null,
    featureStateByDevice: new Map(),
    schemaRevisionByDevice: new Map()
};

const escapeHtml = value => String(value).replace(/[&<>'"]/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;'
})[character]);
