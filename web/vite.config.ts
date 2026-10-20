import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
// The console lives under /app/ on the shared domain (landing site owns /).
export default defineConfig({
  base: '/app/',
  plugins: [react()],
})
