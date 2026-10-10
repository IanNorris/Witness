import vue from '../../web-ui/node_modules/@vitejs/plugin-vue/dist/index.mjs'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
const root = fileURLToPath(new URL('../../', import.meta.url))
const media = readFileSync(new URL('../../artifacts/recent-review-fixtures-20261010/h264-aac.mp4', import.meta.url))
const thumbnail = readFileSync(new URL('../../artifacts/recent-replay-thumbnail.png', import.meta.url))
export default {
  root,
  cacheDir: 'artifacts/recent-replay-vite-cache',
  optimizeDeps: { entries: ['Tools/Tests/recentReplayFixture.html'] },
  define: { __BUILD_HASH__: JSON.stringify('synthetic-replay-fixture') },
  resolve: { dedupe: ['vue', 'pinia'], alias: {
    vue: fileURLToPath(new URL('../../web-ui/node_modules/vue/dist/vue.esm-bundler.js', import.meta.url)),
    pinia: fileURLToPath(new URL('../../web-ui/node_modules/pinia/dist/pinia.mjs', import.meta.url)),
    'vue-router': fileURLToPath(new URL('../../web-ui/node_modules/vue-router/dist/vue-router.mjs', import.meta.url)),
  } },
  plugins: [vue(), {name: 'synthetic-replay-only', configureServer(server) {
    server.middlewares.use((req, res, next) => {
      const path = new URL(req.url, 'http://localhost').pathname
      const requestedAnchor = /^\/dvr\/(?:coverage|segments)\/\d+\/\d+\/(\d+)$/.exec(path)
      const anchor = requestedAnchor ? Number(requestedAnchor[1]) : Math.floor(Date.now() / 1000)
      const segments = [1800, 600, 305].map((offset, index) => ({id: index + 1, from: anchor - offset, to: anchor - offset + 8, duration: 8}))
      const json = value => {res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(value))}
      if (path.startsWith('/clip/enum/')) {
        const parts = path.split('/'), url = new URL(req.url, 'http://localhost')
        const count = Number(parts[4]), offset = Number(parts[7]), camera = Number(parts[3])
        let clips = Array.from({length: 30}, (_, i) => ({clipUID: 1000 + i, cameraID: i < 15 ? 8 : 9,
          timestamp: anchor - i * 15, duration: 8, tags: 'person', saved: 0, recordMode: 1, lighting: 1, reviewed: 0}))
        if (camera !== -1) clips = clips.filter(c => c.cameraID === camera)
        if (url.searchParams.has('tags') && !url.searchParams.get('tags').split(',').includes('person')) clips = []
        return json({count: clips.length, clips: clips.slice(offset, offset + count)})
      }
      if (path === '/clip/tags') return json({tags: [{id: 1, name: 'person', display: 'Person', icon: '', sortOrder: 0, hidden: 0, clipCount: 30}]})
      if (path.startsWith('/clip/thumb/')) {res.setHeader('Content-Type', 'image/png'); return res.end(thumbnail)}
      if (path.startsWith('/clip/video/')) {res.setHeader('Content-Type', 'video/mp4'); res.setHeader('Content-Length', media.length); return res.end(media)}
      if (path.startsWith('/dvr/coverage/')) return json({ranges: segments.map(({from, to}) => ({from, to})), pending: [{from: anchor - 65, to: anchor, targetSeconds: 60}]})
      if (path.startsWith('/dvr/segments/')) return json({segments})
      if (path.startsWith('/dvr/segment/')) {
        let start = 0, end = media.length - 1
        const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? '')
        if (range) { start = Number(range[1]); if (range[2]) end = Math.min(end, Number(range[2])); res.statusCode = 206 }
        if (start >= media.length || end < start) {res.statusCode = 416; return res.end()}
        res.setHeader('Content-Type', 'video/mp4'); res.setHeader('Accept-Ranges', 'bytes')
        if (range) res.setHeader('Content-Range', `bytes ${start}-${end}/${media.length}`)
        res.setHeader('Content-Length', end - start + 1); return res.end(media.subarray(start, end + 1))
      }
      if (path.startsWith('/camera/preview/')) {
        res.setHeader('Content-Type', 'image/svg+xml')
        return res.end('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 360"><rect width="640" height="360" fill="#203c55"/><text x="60" y="180" fill="white" font-size="28">Synthetic live camera</text></svg>')
      }
      // Never proxy fixture API traffic to the user's running server.
      if (/^\/(camera|clip|auth|api|debug|stream|dvr)\//.test(path)) return json([])
      next()
    })
  } }],
  server: {host: '127.0.0.1', port: 62519, strictPort: true, watch: {ignored: ['**/build*/**', '**/ThirdParty/**', '**/.git/**']}},
}
