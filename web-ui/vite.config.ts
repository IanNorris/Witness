import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  base: '/witness2/',
  build: {
    outDir: '../WitnessServer/Web/witness2',
    emptyOutDir: true,
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
