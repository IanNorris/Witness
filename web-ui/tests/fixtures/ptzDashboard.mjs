// Synthetic, loopback-only UI fixture. Never forwards a request to Witness or a camera.
import { createServer } from 'node:http'
import { createHash } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { extname, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'

export async function startPtzDashboard({ layout = false } = {}) {
  // Resolve from repository rather than the caller's current directory.
  const webRoot = resolve(fileURLToPath(new URL('../../../WitnessServer/Web', import.meta.url)))
  const cameras = (layout ? [10, 12, 13, 14, 15] : [10, 12]).map((id, index) => ({ id, name: `Fixture PTZ ${index + 1}`,
    groups: [], status: 'Connected', recording: layout && index < 2, ptzEnabled: 1, codec: 'h264', width: 1920, height: 1080 }))
  const commands = []
  const sockets = new Set()
  const json = (res, value) => { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(value)) }
  const server = createServer(async (req, res) => {
    try {
      const path = new URL(req.url, 'http://localhost').pathname
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
          activityDirection: 'vertical' }
        const initial = `<script>localStorage.setItem('streamingMode','"jpeg"');localStorage.setItem('fullscreenMode','${layout ? 'false' : 'true'}');localStorage.setItem('witness-dashboard-activity-visible','${layout ? '1' : '0'}');${layout ? `localStorage.setItem('witness-dashboard-windowed-mode','layout');localStorage.setItem('witness-dashboard-layout-v1-all',${JSON.stringify(JSON.stringify(sampleLayout))});` : ''}</script>`
        return res.end((await readFile(resolve(webRoot, 'index.html'), 'utf8')).replace('<head>', '<head>' + initial))
      }
      res.statusCode = 404; res.end()
    } catch { res.statusCode = 500; res.end() }
  })
  // A quiet event connection avoids testing behind the real connection-loss
  // modal. No event or camera protocol is forwarded to a production service.
  server.on('upgrade', (req, socket) => {
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
    async close() { for (const socket of sockets) socket.destroy(); server.closeAllConnections(); await new Promise(done => server.close(done)) } }
}

if (process.argv.includes('--serve')) {
  const fixture = await startPtzDashboard({ layout: process.argv.includes('--layout') })
  console.log('PTZ fixture: ' + fixture.origin)
  process.on('SIGINT', async () => { await fixture.close(); process.exit(0) })
}
