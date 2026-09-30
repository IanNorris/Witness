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
// Deliberately synthetic scene: visual QA without real camera imagery or PII.
const scene = `<svg xmlns="http://www.w3.org/2000/svg" width="960" height="540" viewBox="0 0 960 540"><defs><linearGradient id="sky" x2="0" y2="1"><stop stop-color="#8aaeb6"/><stop offset="1" stop-color="#d7dac2"/></linearGradient></defs><path fill="url(#sky)" d="M0 0h960v540H0z"/><path fill="#506f61" d="M0 220 140 160 290 220 420 140 640 220 810 130 960 210v330H0z"/><path fill="#90a17d" d="M0 340h960v200H0z"/><path fill="#b4b1a3" d="m310 540 170-270h70l220 270z"/><path fill="#c4bdac" d="M610 215h240v170H610z"/><path fill="#4f6464" d="m580 220 150-100 150 100z"/><path fill="#374f54" d="M660 275h55v110h-55zM750 255h65v65h-65z"/><circle fill="#38594c" cx="150" cy="240" r="85"/><path stroke="#6c6251" stroke-width="20" d="M150 285v150"/><text x="24" y="42" fill="#fff" font-family="sans-serif" font-size="20">VISUAL TEST SCENE</text></svg>`
const clips = Array.from({ length: 6 }, (_, i) => ({
  clipUID: i + 1, cameraID: i % 2 ? 9 : 8, timestamp: Math.floor(Date.now() / 1000) - 180 - i * 110,
  duration: 18 + i * 7, tags: i % 2 ? 'Vehicle' : 'Person', reviewed: i > 2 ? 1 : 0,
  saved: i === 2 ? 1 : 0, recordMode: 1, lighting: 1, detectionVersion: 14,
  audioEvents: [{ group: i % 2 ? 'vehicle' : 'speech', startTime: Math.floor(Date.now() / 1000) - 175 - i * 110, endTime: Math.floor(Date.now() / 1000) - 165 - i * 110, peakScore: .87 }],
}))
const json = (res, value) => { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(value)) }
const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname
  if (path === '/auth/profile') return json(res, { csrf: 'fixture', username: 'reviewer', admin: false })
  if (path === '/camera/enum') return json(res, cameras)
  if (path === '/group/enum') return json(res, { groups: [] })
  if (path.startsWith('/clip/thumb/') || path.startsWith('/camera/preview/')) { res.setHeader('Content-Type', 'image/svg+xml'); return res.end(scene) }
  if (path.startsWith('/clip/enum/') || path.startsWith('/clip/recent/')) return json(res, { clips, count: clips.length })
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
    const errors = []
    page.on('pageerror', e => errors.push(e.message))
    for (const width of [1440, 768, 390, 320]) {
      await page.setViewportSize({ width, height: 1000 })
      for (const route of ['dvr', 'dashboard', 'clips']) {
        await page.goto(`${origin}/${route === 'dashboard' ? '' : route}?at=${start + 900}`)
        await page.locator('.app-wrapper').waitFor({ state: 'visible' })
        await page.getByRole('heading', { name: 'Connection Lost', exact: true }).waitFor({ state: 'hidden' })
        if (route === 'dvr') {
          await page.getByRole('heading', { name: 'DVR', exact: true }).waitFor()
          assert.equal(await page.locator('video').count(), 0, 'Opening DVR must not start streams')
          await page.screenshot({ path: `test-screenshots/dvr-empty-${width}.png` })
          await page.getByRole('button', { name: 'Garden overview', exact: true }).click()
          await page.locator('.dvr-track').waitFor()
        }
        if (route === 'dashboard') await page.locator('.strip-thumb').first().waitFor()
        if (route === 'clips') { await page.locator('.clip-card').first().waitFor(); assert.equal(await page.locator('.clip-card').count(), clips.length) }
        await page.screenshot({ path: `test-screenshots/${route}-${width}.png` })
        if (route === 'dashboard' && width === 1440) {
          await page.locator('.strip-thumb[role="button"]').first().focus()
          await page.keyboard.press('Enter')
          await page.locator('.clip-modal').waitFor()
          await page.keyboard.press('Escape')
          await page.locator('.clip-modal').waitFor({ state: 'hidden' })
          await page.getByRole('button', { name: 'Fullscreen', exact: true }).click()
          await page.locator('.camera-grid.fullscreen').waitFor()
          assert.equal(await page.getByRole('heading', { name: 'Your cameras', exact: true }).count(), 0)
          await page.screenshot({ path: 'test-screenshots/dashboard-fullscreen.png' })
          await page.getByRole('button', { name: 'Layout', exact: true }).click()
          await page.getByRole('button', { name: 'right', exact: true }).click()
          await page.locator('.activity-strip.vertical').waitFor()
          await page.screenshot({ path: 'test-screenshots/dashboard-right-activity.png', animations: 'disabled' })
          await page.getByRole('button', { name: 'Cancel', exact: true }).click()
          await page.keyboard.press('Escape')
          await page.getByRole('heading', { name: 'Your cameras', exact: true }).waitFor()
        }
        if (route === 'clips' && width === 1440) {
          await page.locator('.clip-thumb').first().focus()
          await page.keyboard.press('Space')
          await page.locator('.clip-modal').waitFor()
          await page.keyboard.press('Escape')
          await page.locator('.clip-modal').waitFor({ state: 'hidden' })
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
