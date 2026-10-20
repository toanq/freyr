import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// The CMS API lives in the Synora server under /cms (same mapping as nginx.conf).
const synoraServer = process.env.SYNORA_SERVER_URL ?? 'http://127.0.0.1:5080'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    host: true,
    allowedHosts: ['synora-landing.carcise.dev'],
    proxy: {
      '/api': {
        target: synoraServer,
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api/, '/cms'),
      },
      '/uploads': {
        target: synoraServer,
        changeOrigin: true,
        rewrite: (path) => `/cms${path}`,
      },
    },
  },
  preview: {
    host: true,
    allowedHosts: ['synora-landing.carcise.dev'],
  },
})
