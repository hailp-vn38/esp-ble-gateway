// --- UI Utilities ---
const ui = {
    openModal(deviceInfo) {
        state.selectedDeviceForAdd = deviceInfo;
        
        // Populate modal data
        document.getElementById('modal-dev-name').innerText = deviceInfo.name || 'Unknown Device';
        document.getElementById('modal-dev-mac').innerText = deviceInfo.mac;
        document.getElementById('modal-dev-rssi').innerText = deviceInfo.rssi + ' dBm';
        
        // Pre-fill input
        const input = document.getElementById('input-custom-name');
        input.value = deviceInfo.name !== 'Unknown Device' ? deviceInfo.name : '';
        
        const modal = document.getElementById('modal-add-device');
        const backdrop = document.getElementById('modal-backdrop');
        const content = document.getElementById('modal-content');
        
        modal.classList.remove('hidden');
        modal.classList.add('flex');
        
        // Trigger reflow
        void modal.offsetWidth;
        
        backdrop.classList.remove('opacity-0');
        content.classList.remove('scale-95', 'opacity-0');
        
        setTimeout(() => input.focus(), 200);
    },
    closeModal() {
        const modal = document.getElementById('modal-add-device');
        const backdrop = document.getElementById('modal-backdrop');
        const content = document.getElementById('modal-content');
        
        backdrop.classList.add('opacity-0');
        content.classList.add('scale-95', 'opacity-0');
        
        setTimeout(() => {
            modal.classList.add('hidden');
            modal.classList.remove('flex');
            state.selectedDeviceForAdd = null;
        }, 200); // match transition duration
    },
    openEditModal() {
        if(!state.selectedDeviceDetail) return;
        
        // Populate modal data
        document.getElementById('input-edit-name').value = state.selectedDeviceDetail.customName;
        document.getElementById('input-edit-type').value = state.selectedDeviceDetail.type;
        
        const modal = document.getElementById('modal-edit-device');
        const backdrop = document.getElementById('modal-edit-backdrop');
        const content = document.getElementById('modal-edit-content');
        
        modal.classList.remove('hidden');
        modal.classList.add('flex');
        
        // Trigger reflow
        void modal.offsetWidth;
        
        backdrop.classList.remove('opacity-0');
        content.classList.remove('scale-95', 'opacity-0');
        
        setTimeout(() => document.getElementById('input-edit-name').focus(), 200);
    },
    closeEditModal() {
        const modal = document.getElementById('modal-edit-device');
        const backdrop = document.getElementById('modal-edit-backdrop');
        const content = document.getElementById('modal-edit-content');
        
        backdrop.classList.add('opacity-0');
        content.classList.add('scale-95', 'opacity-0');
        
        setTimeout(() => {
            modal.classList.add('hidden');
            modal.classList.remove('flex');
        }, 200);
    },
    showToast(message, type = 'info') {
        const container = document.getElementById('toast-container');
        const toast = document.createElement('div');
        
        // Icon mapping based on Phosphor icons
        const icons = {
            success: '<i class="ph ph-check-circle text-green-500 text-xl mr-3"></i>',
            error: '<i class="ph ph-warning-circle text-red-500 text-xl mr-3"></i>',
            info: '<i class="ph ph-info text-blue-500 text-xl mr-3"></i>'
        };
        
        toast.className = `flex items-center p-4 bg-white rounded-lg shadow-lg border border-gray-100 transform transition-all duration-300 translate-y-10 opacity-0 min-w-[300px] pointer-events-auto`;
        toast.innerHTML = icons[type] || icons.info;
        const text = document.createElement('p');
        text.className = 'text-sm font-medium text-gray-800';
        text.textContent = message;
        toast.appendChild(text);
        
        container.appendChild(toast);
        
        // Animate in
        requestAnimationFrame(() => {
            toast.classList.remove('translate-y-10', 'opacity-0');
        });
        
        // Remove after 3s
        setTimeout(() => {
            toast.classList.add('opacity-0', 'translate-x-10');
            setTimeout(() => toast.remove(), 300);
        }, 3000);
    }
};
