module.exports = {
    content: [
        './components/web_server/www_src/**/*.html',
        './components/web_server/www_src/**/*.js'
    ],
    theme: {
        extend: {
            fontFamily: {
                sans: ['system-ui', 'sans-serif'],
                mono: ['ui-monospace', 'SFMono-Regular', 'Menlo', 'Monaco', 'Consolas', 'monospace']
            },
            colors: {
                brand: {
                    50: '#f0f9ff',
                    100: '#e0f2fe',
                    300: '#7dd3fc',
                    500: '#0ea5e9',
                    600: '#0284c7',
                    700: '#0369a1',
                    900: '#0c4a6e'
                },
                dark: {
                    bg: '#0f172a',
                    surface: '#1e293b',
                    border: '#334155'
                }
            }
        }
    },
    plugins: []
};
