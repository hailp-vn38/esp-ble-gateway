// Login page logic
(function() {
    const form = document.getElementById('login-form');
    const errorMsg = document.getElementById('error-message');
    const btnLogin = document.getElementById('btn-login');
    const btnText = document.getElementById('btn-login-text');
    const btnSpinner = document.getElementById('btn-login-spinner');

    function showError(message) {
        errorMsg.textContent = message;
        errorMsg.classList.remove('hidden');
    }

    function hideError() {
        errorMsg.classList.add('hidden');
    }

    function setLoading(loading) {
        btnLogin.disabled = loading;
        btnText.textContent = loading ? 'Signing in...' : 'Sign In';
        btnSpinner.classList.toggle('hidden', !loading);
    }

    form.addEventListener('submit', async (e) => {
        e.preventDefault();
        hideError();

        const username = document.getElementById('username').value.trim();
        const password = document.getElementById('password').value;

        if (!username || !password) {
            showError('Please enter both username and password');
            return;
        }

        setLoading(true);

        try {
            const response = await fetch('/api/auth/login', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                credentials: 'same-origin',
                body: JSON.stringify({ username, password })
            });

            const data = await response.json();

            if (response.ok && data.success) {
                window.location.href = '/';
                return;
            }

            if (response.status === 429) {
                showError('Too many login attempts. Please try again later.');
                return;
            }

            if (response.status === 409) {
                showError('Authentication is disabled on this gateway.');
                return;
            }

            showError('Invalid username or password');
        } catch (err) {
            showError('Could not connect to gateway');
        } finally {
            setLoading(false);
        }
    });
})();
