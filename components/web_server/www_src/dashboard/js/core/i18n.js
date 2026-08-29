// --- Internationalization ---
const i18n = {
    currentLang: localStorage.getItem('lang') || 'en',
    translations: {
        en: {
            'settings.title': 'Gateway Settings',
            'settings.subtitle': 'View system and network status.',
            'settings.system_info': 'System Information',
            'settings.firmware_version': 'Firmware Version',
            'settings.uptime': 'Uptime',
            'settings.free_heap': 'Free Heap Memory',
            'settings.network_status': 'Network Status',
            'settings.current_ssid': 'Current SSID',
            'settings.ip_address': 'IP Address',
            'settings.mac_address': 'MAC Address',
            'settings.signal_strength': 'Signal Strength',
            'settings.web_access': 'Web Access',
            'settings.mcp_access': 'MCP Access',
            'settings.mcp_access_desc': 'Manage the Bearer token used by AI agents to access the MCP endpoint.',
            'settings.mcp_no_token': 'No token configured. MCP endpoint is in dev mode (no auth).',
            'settings.mcp_token_set': 'Token set',
            'settings.mcp_generate': 'Generate Token',
            'settings.mcp_rotate': 'Rotate Token',
            'settings.mcp_revoke': 'Revoke',
            'settings.mcp_revoke_confirm': 'Revoke the MCP token? AI agents using this token will lose access.',
            'settings.mcp_new_token_hint': 'New token (copy it now, it won\'t be shown again):',
            'settings.mcp_token_copied': 'Token copied',
            'settings.mcp_token_generated': 'Token generated',
            'settings.mcp_token_revoked': 'Token revoked',
            'settings.danger_zone': 'Danger Zone',
            'settings.restart_desc': 'Rebooting the gateway will temporarily drop all active BLE connections.',
            'settings.restart': 'Restart Gateway',
            'settings.restart_confirm': 'Are you sure you want to restart the ESP32 Gateway?',
            'settings.username': 'Username',
            'settings.password': 'Password',
            'settings.current_password': 'Current Password',
            'settings.auth_enabled': 'Authentication enabled',
            'settings.auth_disabled': 'Authentication disabled',
            'settings.auth_not_configured': 'Not configured. Gateway is open to anyone on the network.',
            'settings.auth_enable': 'Enable Authentication',
            'settings.auth_disable': 'Disable',
            'settings.auth_enabled_toast': 'Authentication enabled',
            'settings.auth_disabled_toast': 'Authentication disabled',
            'settings.auth_password_required': 'Current password is required',
            'settings.auth_credentials_required': 'Username and password are required',
            'settings.auth_action_failed': 'Failed to update authentication',
            'settings.auth_load_failed': 'Failed to load auth status',
            'settings.disconnected': 'Disconnected',
            'settings.na': 'N/A',
        },
        vi: {
            'settings.title': 'Cài đặt Gateway',
            'settings.subtitle': 'Xem trạng thái hệ thống và mạng.',
            'settings.system_info': 'Thông tin hệ thống',
            'settings.firmware_version': 'Phiên bản Firmware',
            'settings.uptime': 'Thời gian chạy',
            'settings.free_heap': 'Bộ nhớ trống',
            'settings.network_status': 'Trạng thái mạng',
            'settings.current_ssid': 'SSID hiện tại',
            'settings.ip_address': 'Địa chỉ IP',
            'settings.mac_address': 'Địa chỉ MAC',
            'settings.signal_strength': 'Cường độ tín hiệu',
            'settings.web_access': 'Truy cập Web',
            'settings.mcp_access': 'Truy cập MCP',
            'settings.mcp_access_desc': 'Quản lý Bearer token mà AI sử dụng để truy cập endpoint MCP.',
            'settings.mcp_no_token': 'Chưa cấu hình token. MCP đang ở chế độ dev (không cần auth).',
            'settings.mcp_token_set': 'Token đã设好了',
            'settings.mcp_generate': 'Tạo Token',
            'settings.mcp_rotate': 'Làm mới Token',
            'settings.mcp_revoke': 'Thu hồi',
            'settings.mcp_revoke_confirm': 'Thu hồi MCP token? AI đang dùng token này sẽ mất quyền truy cập.',
            'settings.mcp_new_token_hint': 'Token mới (hãy copy ngay, sẽ không hiển thị lại):',
            'settings.mcp_token_copied': 'Đã copy token',
            'settings.mcp_token_generated': 'Đã tạo token',
            'settings.mcp_token_revoked': 'Đã thu hồi token',
            'settings.danger_zone': 'Vùng nguy hiểm',
            'settings.restart_desc': 'Khởi động lại gateway sẽ ngắt tạm thời tất cả kết nối BLE.',
            'settings.restart': 'Khởi động lại Gateway',
            'settings.restart_confirm': 'Bạn có chắc muốn khởi động lại ESP32 Gateway?',
            'settings.username': 'Tên đăng nhập',
            'settings.password': 'Mật khẩu',
            'settings.current_password': 'Mật khẩu hiện tại',
            'settings.auth_enabled': 'Đã bật xác thực',
            'settings.auth_disabled': 'Đã tắt xác thực',
            'settings.auth_not_configured': 'Chưa cấu hình. Gateway mở cho tất cả trên mạng.',
            'settings.auth_enable': 'Bật xác thực',
            'settings.auth_disable': 'Tắt',
            'settings.auth_enabled_toast': 'Đã bật xác thực',
            'settings.auth_disabled_toast': 'Đã tắt xác thực',
            'settings.auth_password_required': 'Cần nhập mật khẩu hiện tại',
            'settings.auth_credentials_required': 'Cần nhập tên đăng nhập và mật khẩu',
            'settings.auth_action_failed': 'Cập nhật xác thực thất bại',
            'settings.auth_load_failed': 'Không thể tải trạng thái xác thực',
            'settings.disconnected': 'Mất kết nối',
            'settings.na': 'N/A',
        }
    },
    t(key) {
        return this.translations[this.currentLang]?.[key] || this.translations.en[key] || key;
    },
    setLang(lang) {
        this.currentLang = lang;
        localStorage.setItem('lang', lang);
        this.applyTranslations();
    },
    applyTranslations() {
        document.querySelectorAll('[data-i18n]').forEach(el => {
            const key = el.getAttribute('data-i18n');
            el.textContent = this.t(key);
        });
        document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
            const key = el.getAttribute('data-i18n-placeholder');
            el.placeholder = this.t(key);
        });
    }
};
