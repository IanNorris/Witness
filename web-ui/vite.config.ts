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
    emptyOutDir: false, // preserve setup/ directory
  },
  server: {
    proxy: {
      '/auth': 'https://localhost:443',
      '/camera': 'https://localhost:443',
      '/clip': 'https://localhost:443',
      '/stream': 'https://localhost:443',
      '/group': 'https://localhost:443',
      '/debug': 'https://localhost:443',
      '/setup': 'https://localhost:443',
    },
  },
})
