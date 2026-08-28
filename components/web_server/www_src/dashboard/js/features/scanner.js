// --- Scanner Logic ---
const scanner = {
    scanInterval: null,
    scanTimeout: null,
    
    async toggleScan() {
        if (state.isScanning) {
            this.stopScan();
        } else {
            this.startScan();
        }
    },
    
    async startScan() {
        state.isScanning = true;
        state.scannedDevices = []; // clear old results
        this.updateUI();
        
        try {
            await api.startScan();
            
            // Show initial loading state if list is empty
            document.getElementById('scan-initial').classList.add('hidden');
            const resultsContainer = document.getElementById('scan-items');
            resultsContainer.replaceChildren();
            
            document.getElementById('scan-loading').classList.remove('hidden');

            let scanCount = 0;
            this.scanInterval = setInterval(async () => {
                try {
                    const result = await api.getScanResults();
                    scanCount++;
                    if(result.devices.length > 0) {
                        document.getElementById('scan-loading').classList.add('hidden');
                        this.mergeResults(result.devices);
                    } else if (scanCount > 3 && state.scannedDevices.length === 0) {
                        document.getElementById('scan-loading').classList.add('hidden');
                        resultsContainer.innerHTML = `
                            <div class="absolute inset-0 flex flex-col items-center justify-center text-gray-500 p-8 text-center bg-gray-50/50">
                                <i class="ph ph-mask-sad text-4xl mb-3 opacity-50"></i>
                                <p class="text-sm">No BLE devices found nearby.</p>
                                <p class="text-xs mt-1">Make sure devices are powered on and in pairing mode.</p>
                            </div>
                        `;
                    }
                    if (!result.scanning) this.stopScan(true);
                } catch (error) {
                    this.stopScan(true);
                }
            }, 1000);
            
            // Firmware normally stops after six seconds; this is a UI fallback.
            this.scanTimeout = setTimeout(() => {
                if(state.isScanning) {
                    this.stopScan();
                }
            }, 8000);

        } catch(e) {
            ui.showToast(`Failed to start scanner: ${e.message}`, "error");
            this.stopScan(true);
        }
    },
    
    async stopScan(skipApiCall = false) {
        state.isScanning = false;
        if(this.scanInterval) {
            clearInterval(this.scanInterval);
            this.scanInterval = null;
        }
        if(this.scanTimeout) {
            clearTimeout(this.scanTimeout);
            this.scanTimeout = null;
        }
        this.updateUI();
        document.getElementById('scan-loading').classList.add('hidden');
        
        if(!skipApiCall) {
            try {
                await api.stopScan();
            } catch (error) {
                // Scan stop failed silently
            }
        }
    },

    mergeResults(newDevices) {
        const container = document.getElementById('scan-items');
        
        // Remove empty state message if present
        if(container.querySelector('.ph-mask-sad')) {
            container.innerHTML = '';
        }

        newDevices.forEach(device => {
            if (state.connectedDevices.some(saved => saved.mac === device.mac)) return;
            const existingIndex = state.scannedDevices.findIndex(d => d.mac === device.mac);
            if (existingIndex > -1) {
                // Update RSSI
                state.scannedDevices[existingIndex].rssi = device.rssi;
                const el = document.getElementById(`scanned-${device.mac.replace(/:/g, '')}`);
                if(el) {
                    const rssiEl = el.querySelector('.rssi-val');
                    if(rssiEl) rssiEl.innerText = `${device.rssi} dBm`;
                }
            } else {
                // Add new
                state.scannedDevices.push(device);
                this.renderDeviceItem(device, container);
            }
        });
    },

    renderDeviceItem(device, container) {
        const id = `scanned-${device.mac.replace(/:/g, '')}`;
        const div = document.createElement('div');
        div.id = id;
        div.className = "p-4 flex items-center justify-between hover:bg-brand-50/50 transition-colors cursor-pointer group border-b border-gray-100 last:border-0";
        
        // Calculate signal strength bars based on RSSI
        const signalQuality = device.rssi > -60 ? 3 : (device.rssi > -80 ? 2 : 1);
        const signalIcon = signalQuality === 3 ? 'ph-wifi-high text-green-500' : (signalQuality === 2 ? 'ph-wifi-medium text-yellow-500' : 'ph-wifi-low text-red-400');

        // Determine icon based on name heuristics
        let devIcon = 'ph-bluetooth';
        if(device.name.toLowerCase().includes('sensor') || device.name.toLowerCase().includes('th_')) devIcon = 'ph-thermometer';
        if(device.name.toLowerCase().includes('bulb') || device.name.toLowerCase().includes('light')) devIcon = 'ph-lightbulb';

        const safeName = escapeHtml(device.name);
        const safeMac = escapeHtml(device.mac);
        div.innerHTML = `
            <div class="flex items-center">
                <div class="w-10 h-10 bg-gray-100 text-gray-600 rounded-full flex items-center justify-center mr-4 group-hover:bg-brand-100 group-hover:text-brand-600 transition-colors">
                    <i class="ph ${devIcon} text-xl"></i>
                </div>
                <div>
                    <h4 class="font-medium text-gray-900 leading-tight">${safeName}</h4>
                    <div class="flex items-center mt-1 space-x-3 text-xs text-gray-500">
                        <span class="font-mono bg-gray-100 px-1.5 py-0.5 rounded text-[10px]">${safeMac}</span>
                        <span class="flex items-center" title="Signal Strength">
                            <i class="ph ${signalIcon} mr-1"></i>
                            <span class="rssi-val">${device.rssi} dBm</span>
                        </span>
                    </div>
                </div>
            </div>
            <button class="opacity-0 group-hover:opacity-100 transition-opacity px-4 py-1.5 bg-white border border-gray-200 text-brand-600 rounded shadow-sm hover:bg-brand-50 hover:border-brand-200 text-sm font-medium flex items-center">
                Select <i class="ph ph-arrow-right ml-1"></i>
            </button>
        `;
        
        div.onclick = () => ui.openModal(device);
        container.appendChild(div);
    },

    updateUI() {
        const btnText = document.getElementById('btn-scan-text');
        const btnIcon = document.querySelector('#btn-toggle-scan i');
        const btn = document.getElementById('btn-toggle-scan');
        const statusText = document.getElementById('scan-status-text');
        const radarRing = document.getElementById('scan-radar-ring');
        const radarFill = document.getElementById('scan-radar');

        if (state.isScanning) {
            btnText.innerText = "Stop Scan";
            btnIcon.className = "ph ph-stop-circle mr-2 text-lg text-red-100";
            btn.className = "w-full sm:w-auto px-6 py-2.5 bg-red-500 text-white rounded-lg hover:bg-red-600 transition-colors shadow-sm font-medium flex items-center justify-center";
            statusText.innerText = "Scanning nearby...";
            radarRing.classList.remove('hidden');
            radarFill.classList.remove('hidden');
            radarRing.classList.add('animate-radar');
        } else {
            btnText.innerText = "Start Scan";
            btnIcon.className = "ph ph-play-circle mr-2 text-lg";
            btn.className = "w-full sm:w-auto px-6 py-2.5 bg-brand-600 text-white rounded-lg hover:bg-brand-700 transition-colors shadow-sm font-medium flex items-center justify-center";
            statusText.innerText = "Ready to scan";
            radarRing.classList.add('hidden');
            radarFill.classList.add('hidden');
            radarRing.classList.remove('animate-radar');
        }
    }
};
