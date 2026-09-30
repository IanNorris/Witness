// Integration test against the built UI, with a synthetic recording and API.
// No running Witness instance or real camera/database is used.
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { createServer } from 'node:http'
import { mkdtemp, readFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, extname, resolve } from 'node:path'
import { chromium } from 'playwright'

const root = resolve('../WitnessServer/Web')
const scratch = await mkdtemp(join(tmpdir(), 'witness-dvr-test-'))
const videoPath = join(scratch, 'fixture.mp4')
const generated = spawnSync('ffmpeg', ['-v', 'error', '-f', 'lavfi', '-i', 'color=c=blue:s=160x90:r=10',
  '-t', '120', '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', videoPath])
assert.equal(generated.status, 0, generated.stderr?.toString())
const video = await readFile(videoPath)
const start = 1700000000
const cameras = [{ id: 8, name: 'Fixture camera', groups: [], status: 'Connected' }]
const json = (res, value) => { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(value)) }
const server = createServer(async (req, res) => {
  try {
    const path = new URL(req.url, 'http://localhost').pathname
    if (path === '/auth/profile') return json(res, { csrf: 'fixture', username: 'tester', admin: false })
    if (path === '/camera/enum') return json(res, cameras)
    if (path === '/group/enum') return json(res, { groups: [] })
    if (path.startsWith('/dvr/coverage/')) return json(res, { ranges: [{ from: start, to: start + 120 }] })
    if (path.startsWith('/dvr/events/')) return json(res, { clips: [
      { id: 1, from: start + 10, to: start + 13 }, { id: 2, from: start + 40, to: start + 43 },
    ], audio: [], clipsTruncated: false })
    if (path.startsWith('/dvr/segments/')) return json(res, { segments: [{ id: 1, from: start, to: start + 120, duration: 120 }] })
    if (path === '/dvr/segment/1') {
      res.setHeader('Content-Type', 'video/mp4')
      res.setHeader('Accept-Ranges', 'bytes')
      const range = /bytes=(\d+)-(\d*)/.exec(req.headers.range ?? '')
      const from = range ? Number(range[1]) : 0
      const to = range?.[2] ? Math.min(Number(range[2]), video.length - 1) : video.length - 1
      if (range) { res.statusCode = 206; res.setHeader('Content-Range', `bytes ${from}-${to}/${video.length}`) }
      res.setHeader('Content-Length', to - from + 1)
      return res.end(video.subarray(from, to + 1))
    }
    if (path.startsWith('/dvr/')) return json(res, { frames: [] })
    const file = path.startsWith('/assets/') ? join(root, path.slice(1)) : join(root, 'index.html')
    res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html' })[extname(file)] ?? 'application/octet-stream')
    res.end(await readFile(file))
  } catch { res.statusCode = 404; res.end() }
})
await new Promise(done => server.listen(0, '127.0.0.1', done))
let browser
try {
  browser = await chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] })
  const page = await browser.newPage()
  await page.routeWebSocket('**/ws/events', () => {})
  const errors = []
  page.on('pageerror', error => errors.push(error.message))
  const origin = `http://127.0.0.1:${server.address().port}`
  await page.goto(`${origin}/dvr?camera=8&at=${start}`)
  await page.getByRole('link', { name: 'Dashboard', exact: true }).waitFor()
  const toggle = page.getByRole('checkbox', { name: 'Highlights reel' })
  await page.waitForFunction(() => !document.querySelector('.dvr-highlights-toggle input')?.disabled)
  const colours = await page.locator('.dvr-panel').first().evaluate(el => ({
    panel: getComputedStyle(el).backgroundColor,
    track: getComputedStyle(document.querySelector('.dvr-track')).backgroundColor,
    coverage: getComputedStyle(document.querySelector('.dvr-range.dvr-coverage')).backgroundColor,
  }))
  assert.notEqual(colours.panel, 'rgb(255, 255, 255)')
  assert.notEqual(colours.track, colours.coverage)
  await page.locator('.dvr-speed').selectOption('4')
  await toggle.check()
  await page.waitForFunction(() => [...document.querySelectorAll('video')].some(v => !v.paused && v.currentTime >= 8 && v.currentTime < 20))
  await page.getByRole('button', { name: 'Pause selected', exact: true }).click()
  await page.waitForFunction(() => [...document.querySelectorAll('video')].every(v => v.paused))
  const pausedAt = await page.locator('.dvr-player-grid video').evaluateAll(videos => videos.map(v => v.currentTime))
  await new Promise(done => setTimeout(done, 300))
  assert.deepEqual(await page.locator('.dvr-player-grid video').evaluateAll(videos => videos.map(v => v.currentTime)), pausedAt)
  await page.getByRole('button', { name: 'Play selected', exact: true }).click()
  await page.waitForFunction(() => [...document.querySelectorAll('video')].some(v => !v.paused && v.currentTime >= 38 && v.currentTime < 47), null, { timeout: 15000 })
  await page.getByRole('status').filter({ hasText: 'End of highlights' }).waitFor({ timeout: 15000 })
  await page.waitForFunction(() => [...document.querySelectorAll('video')].every(v => v.paused))
  assert.deepEqual(errors, [])
  console.log('PASS: DVR sidebar, timeline contrast, real-video highlights seek, quiet-period skip and end-of-reel pause')
  // Manual clock navigation within an event retains the reel and pause state.
  await toggle.check()
  const clock = page.getByLabel('Go to local time')
  const atEvent = await page.evaluate(ts => {
    const date = new Date(ts * 1000)
    const pad = n => String(n).padStart(2, '0')
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`
  }, start + 41)
  await clock.fill(atEvent)
  await clock.dispatchEvent('change')
  await page.waitForFunction(() => [...document.querySelectorAll('.dvr-player-grid video')].every(v => v.paused)
    && [...document.querySelectorAll('.dvr-player-grid video')].some(v => Math.abs(v.currentTime - 41) < 1))
  assert.equal(await toggle.isChecked(), true)
  // Click actual quiet recording coverage, rather than another padded highlight.
  const track = await page.locator('.dvr-track').first().boundingBox()
  assert.ok(track)
  await page.mouse.click(track.x + track.width * (1800 + 25) / 3600, track.y + track.height / 2)
  await page.waitForFunction(() => !document.querySelector('.dvr-highlights-toggle input').checked)
  await page.waitForFunction(() => [...document.querySelectorAll('.dvr-player-grid video')].every(v => v.paused)
    && [...document.querySelectorAll('.dvr-player-grid video')].some(v => v.currentTime > 20 && v.currentTime < 32))
  await page.locator('.dvr-speed').selectOption('1')
  await page.getByRole('button', { name: 'Play selected', exact: true }).click()
  await new Promise(done => setTimeout(done, 600))
  const quietTime = await page.locator('.dvr-player-grid video:visible').first().evaluate(v => v.currentTime)
  assert.ok(quietTime > 20 && quietTime < 33, `Manual quiet-period playback was pulled into a highlight: ${quietTime}`)
  // An actively playing manual jump also exits the reel, without pausing.
  await toggle.check()
  await page.waitForFunction(() => [...document.querySelectorAll('.dvr-player-grid video')].some(v => !v.paused && v.currentTime >= 38 && v.currentTime < 47))
  await page.getByRole('button', { name: '+30s', exact: true }).click()
  await page.waitForFunction(() => !document.querySelector('.dvr-highlights-toggle input').checked
    && [...document.querySelectorAll('.dvr-player-grid video')].some(v => !v.paused && v.currentTime > 60 && v.currentTime < 80))
  assert.deepEqual(errors, [])
  console.log('PASS: manual event seek preserves highlights; quiet-period click/jump exits and preserves paused/playing state')
  await page.goto(`${origin}/stream/8`)
  await page.waitForURL('**/live/8')
  await page.reload()
  assert.equal(new URL(page.url()).pathname, '/live/8')
  console.log('PASS: legacy UI navigation canonicalizes to /live/8 and reload stays on the viewer')
} finally {
  await browser?.close()
  await new Promise(done => server.close(done))
  await rm(scratch, { recursive: true, force: true })
}
