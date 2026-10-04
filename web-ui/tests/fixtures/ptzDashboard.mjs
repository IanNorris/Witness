// Synthetic, loopback-only UI fixture. Never forwards a request to Witness or a camera.
import { createServer } from 'node:http'
import { createHash } from 'node:crypto'
import { readFile, appendFile } from 'node:fs/promises'
import { extname, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createInterface } from 'node:readline'
import { createRequire } from 'node:module'
import { loadMuxReplay, streamMuxReplay } from './muxReplay.mjs'

export async function startPtzDashboard({ layout = false, automaticSlots = false, muxFile, seconds = 120, evidenceFile } = {}) {
  const replay = muxFile ? await loadMuxReplay(muxFile, seconds) : null
  // Resolve from repository rather than the caller's current directory.
  const webRoot = resolve(fileURLToPath(new URL('../../../WitnessServer/Web', import.meta.url)))
  const cameras = (layout ? [10, 12, 13, 14, 15] : [10, 12]).map((id, index) => ({ id, name: `Fixture PTZ ${index + 1}`,
    groups: [], status: 'Connected', recording: layout && index < 2, ptzEnabled: 1, codec: 'h264', width: 1920, height: 1080 }))
  const commands = []
  const sockets = new Set()
  const streamConnections = new Map()
  const reports = []
  const json = (res, value) => { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(value)) }
  const server = createServer(async (req, res) => {
    try {
      const path = new URL(req.url, 'http://localhost').pathname
      if (replay && path === '/test/evidence' && req.method === 'POST') {
        let body = ''
        for await (const chunk of req) {
          body += chunk
          if (body.length > 256 * 1024) { res.statusCode = 413; return res.end() }
        }
        if (reports.length >= 400) { res.statusCode = 429; return res.end() }
        reports.push(JSON.parse(body))
        const evidence = { at: new Date().toISOString(), report: reports.at(-1), connections: Object.fromEntries(streamConnections) }
        if (evidenceFile) await appendFile(evidenceFile, JSON.stringify(evidence) + '\n')
        console.log(JSON.stringify(evidence))
        return json(res, {})
      }
      if (path === '/auth/profile') return json(res, { csrf: 'fixture-only', username: 'fixture', admin: false })
      if (path === '/camera/enum') return json(res, cameras)
      if (path === '/group/enum') return json(res, { groups: [] })
      if (path.startsWith('/clip/')) return json(res, { clips: [], total: 0 })
      if (path === '/debug/client-health') { res.statusCode = 202; return json(res, {}) }
      if (path.match(/^\/ptz\/\d+\/zoom$/)) return json(res, { valid: false })
      if (path.match(/^\/ptz\/\d+\/presets$/)) return json(res, { presets: [
        { id: 1, name: 'Driveway', enabled: true }, { id: 2, name: 'Gate', enabled: true },
      ] })
      if (path.startsWith('/ptz/') || path.startsWith('/camera/record/')) {
        let body = ''
        for await (const chunk of req) body += chunk
        commands.push({ path, method: req.method, body: body ? JSON.parse(body) : null })
        return json(res, { success: true })
      }
      if (path.startsWith('/camera/preview/')) {
        res.setHeader('Content-Type', 'image/svg+xml')
        return res.end('<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360"><rect width="640" height="360" fill="#24354b"/><path d="M0 180H640M320 0V360" stroke="#4c6683"/><text x="24" y="45" fill="#dceafa" font-size="24">Synthetic camera — no physical movement</text></svg>')
      }
      if (path.startsWith('/assets/')) {
        const file = resolve(webRoot, '.' + path)
        if (!file.startsWith(webRoot + sep)) { res.statusCode = 403; return res.end() }
        res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.css': 'text/css' })[extname(file)] ?? 'application/octet-stream')
        return res.end(await readFile(file))
      }
      if (path === '/' || path === '/dvr') {
        res.setHeader('Content-Type', 'text/html')
        const sampleLayout = { version: 1, tiles: cameras.map((camera, i) => ({ cameraId: camera.id,
          x: (i % 3) * 4, y: Math.floor(i / 3) * 6, width: 4, height: 6 })),
          activityPlacement: 'rectangle', activityRegion: { x: 8, y: 6, width: 4, height: 6 },
          activityDirection: 'vertical', ...(automaticSlots ? {
            visibleCameraIds: [10, 12, 13], priorityCameraIds: [15, 14, 13, 12, 10], slotHoldSeconds: 6,
            automaticSlots: [{ anchorCameraId: 12, fallbackCameraId: 12 }, { anchorCameraId: 13, fallbackCameraId: 13 }],
          } : {}) }
        const initial = `<script>localStorage.setItem('streamingMode','"${replay ? 'mse' : 'jpeg'}"');localStorage.setItem('fullscreenMode','${layout ? 'false' : 'true'}');localStorage.setItem('witness-dashboard-activity-visible','${layout ? '1' : '0'}');${layout ? `localStorage.setItem('witness-dashboard-windowed-mode','layout');localStorage.setItem('witness-dashboard-layout-v1-all',${JSON.stringify(JSON.stringify(sampleLayout))});` : ''}
          ${replay ? `setInterval(() => {
            const players = Object.entries(window._witnessMseDiag || {}).map(([id, diag]) => {
              const d = diag.snapshot(); return { id, startTime: d.startTime, stats: d.stats,
                time: d.currentState?.currentTime, ready: d.currentState?.readyState, paused: d.currentState?.paused,
                error: d.currentState?.error, queue: d.liveState?.appendQueueLength };
            });
            const tiles = [...document.querySelectorAll('.camera-grid-item')].map(tile => ({
              text: tile.textContent?.trim().slice(0, 100), hidden: tile.getAttribute('aria-hidden') }));
            fetch('/test/evidence', {method:'POST', body:JSON.stringify({players, tiles})}).catch(()=>{});
          }, 5000);` : ''}</script>`
        return res.end((await readFile(resolve(webRoot, 'index.html'), 'utf8')).replace('<head>', '<head>' + initial))
      }
      res.statusCode = 404; res.end()
    } catch { res.statusCode = 500; res.end() }
  })
  // A quiet event connection avoids testing behind the real connection-loss
  // modal. No event or camera protocol is forwarded to a production service.
  let wsServer
  if (replay) {
    const { wsServer: WebSocketServer } = createRequire(import.meta.url)('../../node_modules/playwright-core/lib/utilsBundle.js')
    wsServer = new WebSocketServer({ server })
    wsServer.on('connection', (socket, req) => {
      if (req.url === '/ws/events') { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); return }
      const match = req.url.match(/^\/ws\/stream\/(?:sub\/)?(\d+)(?:\?|$)/)
      if (!match || !cameras.some(camera => camera.id === Number(match[1]))) { socket.close(); return }
      const id = Number(match[1]), evidence = streamConnections.get(id) ?? { opened: 0, closed: 0 }
      evidence.opened++; streamConnections.set(id, evidence)
      socket.on('close', () => evidence.closed++)
      streamMuxReplay({ send: data => socket.readyState === 1 && socket.send(data), onClose: cb => socket.on('close', cb) }, replay)
    })
  } else server.on('upgrade', (req, socket) => {
    const key = req.headers['sec-websocket-key']
    if (req.url !== '/ws/events' || typeof key !== 'string') { socket.destroy(); return }
    const accept = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64')
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n')
    sockets.add(socket)
    socket.on('error', () => socket.destroy())
    socket.on('close', () => sockets.delete(socket))
  })
  await new Promise(done => server.listen(0, '127.0.0.1', done))
  return { server, cameras, commands, origin: `http://127.0.0.1:${server.address().port}`,
    setMotion(cameraId, active) {
      const camera = cameras.find(camera => camera.id === cameraId)
      if (!camera || typeof active !== 'boolean') throw new Error('Invalid synthetic motion')
      camera.motionActive = active
      const payload = Buffer.from(JSON.stringify({ event: 'camera:motion', data: { cameraID: cameraId, active } }))
      if (payload.length > 125) throw new Error('Fixture message too large')
      for (const socket of sockets) {
        if (replay) socket.send(payload.toString())
        else socket.write(Buffer.concat([Buffer.from([0x81, payload.length]), payload]))
      }
    },
    async close() {
      if (wsServer) { for (const socket of wsServer.clients) socket.terminate(); await new Promise(done => wsServer.close(done)) }
      else for (const socket of sockets) socket.destroy()
      server.closeAllConnections(); await new Promise(done => server.close(done))
    } }
}

if (process.argv.includes('--serve')) {
  const muxIndex = process.argv.indexOf('--mux')
  const evidenceIndex = process.argv.indexOf('--evidence')
  const fixture = await startPtzDashboard({ layout: process.argv.includes('--layout'), automaticSlots: process.argv.includes('--auto-slots'),
    muxFile: muxIndex >= 0 ? process.argv[muxIndex + 1] : undefined,
    evidenceFile: evidenceIndex >= 0 ? process.argv[evidenceIndex + 1] : undefined })
  console.log('PTZ fixture: ' + fixture.origin)
  const control = createInterface({ input: process.stdin })
  control.on('line', line => {
    try { const command = JSON.parse(line); fixture.setMotion(command.cameraId, command.active); console.log('Synthetic motion updated') }
    catch { console.log('Invalid fixture command') }
  })
  process.on('SIGINT', async () => { await fixture.close(); process.exit(0) })
  setTimeout(async () => { await fixture.close(); control.close(); process.exit(0) }, 240000)
}
