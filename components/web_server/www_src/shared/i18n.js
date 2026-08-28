// --- i18n Module ---
const I18N = {
    en: {
        "nav.devices": "My Devices",
        "nav.add_device": "Add Device",
        "nav.settings": "Gateway Settings",
        "settings.web_access": "Web Access",
        "settings.system_info": "System Information",
        "settings.network_status": "Network Status",
        "settings.danger_zone": "Danger Zone",
        "settings.restart": "Restart Gateway"
    },
    vi: {
        "nav.devices": "Thiết bị",
        "nav.add_device": "Thêm thiết bị",
        "nav.settings": "Cài đặt Gateway",
        "settings.web_access": "Truy cập Web",
        "settings.system_info": "Thông tin hệ thống",
        "settings.network_status": "Trạng thái mạng",
        "settings.danger_zone": "Vùng nguy hiểm",
        "settings.restart": "Khởi động lại Gateway"
    }
};

const i18n = {
    _language: 'auto',

    init() {
        try {
            this._language = localStorage.getItem('esp32-gateway.language') || 'auto';
        } catch (_) {
            this._language = 'auto';
        }
    },

    _resolveLanguage() {
        if (this._language !== 'auto') return this._language;
        const browserLang = navigator.language || navigator.userLanguage || 'en';
        if (browserLang.startsWith('vi')) return 'vi';
        return 'en';
    },

    setLanguage(lang) {
        this._language = lang;
        try {
            localStorage.setItem('esp32-gateway.language', lang);
        } catch (_) {
            // Storage unavailable
        }
        this.apply(document);
    },

    t(key) {
        const lang = this._resolveLanguage();
        return (I18N[lang] && I18N[lang][key]) || (I18N.en && I18N.en[key]) || key;
    },

    apply(root) {
        root.querySelectorAll('[data-i18n]').forEach(el => {
            const key = el.getAttribute('data-i18n');
            const translated = this.t(key);
            if (translated !== key) {
                el.textContent = translated;
            }
        });
    }
};

i18n.init();
