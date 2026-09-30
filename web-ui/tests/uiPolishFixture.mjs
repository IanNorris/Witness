// Isolated, loopback-only visual fixture. No production auth/database/cameras.
// node tests/uiPolishFixture.mjs [--verify] (run from web-ui)
import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import { readFile, mkdir } from 'node:fs/promises'
import { join, resolve, extname } from 'node:path'
import { chromium } from 'playwright'
import { createRequire } from 'node:module'
const { wsServer: WebSocketServer } = createRequire(import.meta.url)('../node_modules/playwright-core/lib/utilsBundle.js')
const root = resolve('../WitnessServer/Web')
const start = Math.floor(Date.now() / 3600000) * 3600
const cameras = [
  { id: 8, name: 'Garden overview', status: 'Connected', groups: [], width: 1920, height: 1080, codec: 'h264' },
  { id: 9, name: 'Front entrance', status: 'Disconnected', groups: [], width: 1920, height: 1080, codec: 'h264' },
]
const json = (res, value) => { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(value)) }
const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname
  if (path === '/auth/profile') return json(res, { csrf: 'fixture', username: 'reviewer', admin: false })
  if (path === '/camera/enum') return json(res, cameras)
  if (path === '/group/enum') return json(res, { groups: [] })
  if (path.startsWith('/dvr/coverage/')) return json(res, { ranges: [{ from: start, to: start + 1800 }] })
  if (path.startsWith('/dvr/events/')) return json(res, { clips: [
    { id: 1, from: start + 120, to: start + 190 }, { id: 2, from: start + 600, to: start + 760 },
  ], audio: [{ from: start + 610, to: start + 680, group: 'Speech', score: .85 }], clipsTruncated: false })
  if (path.startsWith('/dvr/segments/')) return json(res, { segments: [] })
  if (path.startsWith('/dvr/')) return json(res, { frames: [] })
  if (!path.startsWith('/assets/') && path !== '/' && !['/dvr', '/clips'].includes(path))
    return json(res, { clips: [], results: [], count: 0, totalCount: 0 })
  try {
    const file = path.startsWith('/assets/') ? join(root, path.slice(1)) : join(root, 'index.html')
    res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html' })[extname(file)] ?? 'application/octet-stream')
    res.end(await readFile(file))
  } catch { res.statusCode = 404; res.end() }
})
const sockets = new WebSocketServer({ server })
await new Promise(done => server.listen(0, '127.0.0.1', done))
const origin = `http://127.0.0.1:${server.address().port}`
console.log('VISUAL FIXTURE', origin, 'DVR', `${origin}/dvr?at=${start + 900}`)
if (process.argv.includes('--verify')) {
  const browser = await chromium.launch({ headless: true })
  try {
    await mkdir('test-screenshots', { recursive: true })
    const page = await browser.newPage()
    await page.routeWebSocket('**/ws/**', () => {})
    const errors = []
    page.on('pageerror', e => errors.push(e.message))
    for (const width of [1440, 768, 390, 320]) {
      await page.setViewportSize({ width, height: 1000 })
      for (const route of ['dvr', 'dashboard', 'clips']) {
        await page.goto(`${origin}/${route === 'dashboard' ? '' : route}?at=${start + 900}`)
        await page.locator('.app-wrapper').waitFor({ state: 'visible' })
        if (route === 'dvr') {
          await page.getByRole('heading', { name: 'DVR', exact: true }).waitFor()
          assert.equal(await page.locator('video').count(), 0, 'Opening DVR must not start streams')
          await page.screenshot({ path: `test-screenshots/dvr-empty-${width}.png` })
          await page.getByRole('button', { name: 'Garden overview', exact: true }).click()
          await page.locator('.dvr-track').waitFor()
        }
        await page.screenshot({ path: `test-screenshots/${route}-${width}.png` })
        if (route === 'dashboard' && width === 1440) {
          await page.getByRole('button', { name: 'Fullscreen', exact: true }).click()
          await page.locator('.camera-grid.fullscreen').waitFor()
          assert.equal(await page.getByRole('heading', { name: 'Your cameras', exact: true }).count(), 0)
          await page.keyboard.press('Escape')
          await page.getByRole('heading', { name: 'Your cameras', exact: true }).waitFor()
        }
        const overflow = await page.evaluate(() => {
          const main = document.querySelector('.app-content')
          return main.scrollWidth > main.clientWidth + 1
        })
        assert.equal(overflow, false, `Horizontal overflow on ${route} at ${width}px`)
      }
    }
    assert.deepEqual(errors, [])
    console.log('PASS: DVR/dashboard/clips at four widths, opt-in streams, fullscreen/Escape, no page errors or horizontal overflow')
  } finally { await browser.close(); for (const socket of sockets.clients) socket.terminate(); sockets.close(); await new Promise(done => server.close(done)) }
} else {
  const stop = () => { for (const socket of sockets.clients) socket.terminate(); sockets.close(); server.close(() => process.exit(0)) }
  process.on('SIGINT', stop)
  process.on('SIGTERM', stop)
  setTimeout(stop, 20 * 60 * 1000)
}
