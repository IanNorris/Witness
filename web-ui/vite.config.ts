import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { writeFileSync } from 'fs'
import { resolve } from 'path'
import { randomUUID } from 'crypto'

const buildHash = randomUUID().replace(/-/g, '').slice(0, 12)

export default defineConfig({
  plugins: [
    vue(),
    {
      name: 'write-build-hash',
      closeBundle() {
        writeFileSync(resolve(__dirname, '../WitnessServer/Web/build-hash.txt'), buildHash)
      },
    },
  ],
  define: {
    __BUILD_HASH__: JSON.stringify(buildHash),
  },
  base: '/',
  build: {
    outDir: '../WitnessServer/Web',
    emptyOutDir: true,
  },
  server: {
    proxy: {
      '/auth': { target: 'https://localhost:11236', secure: false },
      '/camera': { target: 'https://localhost:11236', secure: false },
      '/clip': { target: 'https://localhost:11236', secure: false },
      '/stream': { target: 'https://localhost:11236', secure: false },
      '/group': { target: 'https://localhost:11236', secure: false },
      '/debug': { target: 'https://localhost:11236', secure: false },
      '/setup': { target: 'https://localhost:11236', secure: false },
      '/dvr': { target: 'https://localhost:11236', secure: false },
      '/ws': { target: 'wss://localhost:11236', secure: false, ws: true },
    },
  },
})
