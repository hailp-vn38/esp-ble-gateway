// --- Navigation Controller ---
const nav = {
    getRoute() {
        const hash = window.location.hash.slice(1);
        if (PERSISTED_TABS.includes(hash)) return {tab: hash};
        if (hash.startsWith('device/')) {
            try {
                const deviceId = decodeURIComponent(hash.slice('device/'.length));
                if (deviceId) return {tab: 'device-detail', deviceId};
            } catch (_) {
                // Invalid route is handled by restoreRoute().
            }
        }
        return null;
    },
    getHash(tabId) {
        if (tabId === 'device-detail' && state.selectedDeviceDetail) {
            return `#device/${encodeURIComponent(state.selectedDeviceDetail.id)}`;
        }
        return PERSISTED_TABS.includes(tabId) ? `#${tabId}` : null;
    },
    replaceRoute(tabId) {
        const hash = this.getHash(tabId);
        if (hash) history.replaceState(null, '', hash);
    },
    restoreRoute() {
        const route = this.getRoute();

        if (!route) {
            this.switchTab(state.activeTab, false);
            this.replaceRoute(state.activeTab);
            return;
        }

        if (route.tab === 'device-detail') {
            const device = state.connectedDevices.find(dev => dev.id === route.deviceId);
            if (device) {
                devices.openDetailView(device, false);
                return;
            }

            ui.showToast("Device not found", "error");
            this.switchTab('devices', false);
            this.replaceRoute('devices');
            return;
        }

        this.switchTab(route.tab, false);
    },
    switchTab(tabId, updateRoute = true) {
        if (updateRoute) {
            const hash = this.getHash(tabId);
            if (hash && window.location.hash !== hash) window.location.hash = hash;
        }

        // Update state
        state.activeTab = tabId;
        saveTab(tabId);
        
        // Hide all views
        ['devices', 'scanner', 'device-detail', 'settings'].forEach(id => {
            const el = document.getElementById(`view-${id}`);
            if(el) el.classList.add('hidden');
        });
        
        // Reset nav button styling
        ['devices', 'scanner', 'settings'].forEach(id => {
            const btn = document.getElementById(`nav-${id}`);
            if(btn) {
                btn.className = 'w-full flex items-center px-4 py-3 text-sm font-medium rounded-lg transition-colors hover:bg-dark-surface hover:text-white text-gray-300';
            }
        });
        
        // Show target view
        const view = document.getElementById(`view-${tabId}`);
        if(view) view.classList.remove('hidden');
        
        // Style active nav button (if applicable)
        const activeBtn = document.getElementById(`nav-${tabId}`);
        if(activeBtn) {
             activeBtn.className = 'w-full flex items-center px-4 py-3 text-sm font-medium rounded-lg transition-colors bg-brand-600 text-white shadow-md shadow-brand-500/20';
        } else if (tabId === 'device-detail') {
            // If showing details, keep 'devices' tab highlighted
            const devicesBtn = document.getElementById('nav-devices');
            if(devicesBtn) devicesBtn.className = 'w-full flex items-center px-4 py-3 text-sm font-medium rounded-lg transition-colors bg-brand-600 text-white shadow-md shadow-brand-500/20';
        }

        // Close mobile sidebar on navigation
        if (window.innerWidth < 768) {
            this.closeSidebar();
        }

        // Trigger specific view logic
        if(tabId === 'devices') devices.renderGrid();
        if(tabId === 'settings') settings.loadMcpTokenStatus();
        if(tabId === 'scanner') {
            if(!state.isScanning && state.scannedDevices.length === 0) {
                // Optional: auto-start scan on view open
            }
        }
    },
    toggleSidebar() {
        const sidebar = document.getElementById('sidebar');
        const overlay = document.getElementById('mobile-overlay');
        if (sidebar.classList.contains('-translate-x-full')) {
            sidebar.classList.remove('-translate-x-full');
            overlay.classList.remove('hidden');
        } else {
            this.closeSidebar();
        }
    },
    closeSidebar() {
        const sidebar = document.getElementById('sidebar');
        const overlay = document.getElementById('mobile-overlay');
        sidebar.classList.add('-translate-x-full');
        overlay.classList.add('hidden');
    }
};
