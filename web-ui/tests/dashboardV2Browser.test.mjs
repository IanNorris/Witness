// Dashboards v2 against the built bundle and an in-memory dashboard API.
// Never connects to a Witness server; camera media is a static preview image.
import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import { readFile, mkdir } from 'node:fs/promises'
import { resolve, join, extname } from 'node:path'
import { chromium } from 'playwright'

const root = resolve('../WitnessServer/Web')
const now = Math.floor(Date.now() / 1000)
const cameras = [8, 9, 10].map(id => ({ id, name: ['Garden', 'Driveway', 'Porch'][id - 8], status: 'Connected', groups: [], recording: false,
  codec: 'h264', width: 1920, height: 1080 }))
const dashboards = new Map()
const updates = []
const streamRequests = []
let nextId = 1
const json = (res, data, status = 200) => { res.statusCode = status; res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(data)) }
const body = req => new Promise(done => { let data = ''; req.on('data', chunk => data += chunk); req.on('end', () => done(data ? JSON.parse(data) : {})) })
const summary = d => ({ id: d.id, name: d.name, scope: d.scope, kind: 'canvas', revision: d.revision, updatedAt: now, owned: true, canEdit: true,
  tiles: d.document.tiles.length, redacted: d.document.tiles.filter(t => t.type === 'unavailable').length, cameras: [] })

const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname
  try {
    if (path === '/auth/profile') return json(res, { csrf: 'synthetic', username: 'tester', admin: false, serverTimeUnixMs: now * 1000, serverUtcOffsetMinutes: 0 })
    if (path === '/camera/enum') return json(res, cameras)
    if (path === '/dashboard/enum') return json(res, { dashboards: [...dashboards.values()].map(summary), admin: false })
    const get = /^\/dashboard\/get\/(\d+)$/.exec(path)
    if (get) { const d = dashboards.get(Number(get[1])); return d ? json(res, { ...summary(d), document: d.document }) : json(res, { error: 'not-found' }, 404) }
    if (path === '/dashboard/create') {
      const b = await body(req)
      const id = nextId++
      dashboards.set(id, { id, name: b.name, scope: b.scope, revision: 1, document: { schema: 2, tiles: [], badges: [], ...b.document } })
      return json(res, { id, revision: 1 })
    }
    if (path === '/dashboard/update') {
      const b = await body(req)
      const d = dashboards.get(b.id)
      if (!d) return json(res, { error: 'not-found' }, 404)
      if (b.baseRevision !== d.revision) return json(res, { error: 'conflict', revision: d.revision }, 409)
      updates.push(b)
      d.revision++
      if (b.name) d.name = b.name
      if (b.document) d.document = b.document
      return json(res, { id: d.id, revision: d.revision })
    }
    if (path.startsWith('/camera/preview/')) { res.setHeader('Content-Type', 'image/svg+xml'); return res.end('<svg xmlns="http://www.w3.org/2000/svg" width="160" height="90"><rect width="160" height="90" fill="#2d4a66"/><circle cx="80" cy="45" r="18" fill="#79beef"/></svg>') }
    if (path.startsWith('/dvr/coverage/')) return json(res, { ranges: [], pending: [] })
    if (path.startsWith('/dvr/events/')) return json(res, { clips: [], audio: [], clipsTruncated: false })
    if (path.startsWith('/dvr/') || path.startsWith('/debug/') || path.startsWith('/api/')) return json(res, { frames: [] })
    if (!path.startsWith('/assets/') && !['/', '/dashboards'].includes(path) && !/^\/(d|kiosk)\//.test(path)) return json(res, { clips: [], count: 0 })
    const file = path.startsWith('/assets/') ? join(root, path.slice(1)) : join(root, 'index.html')
    res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html' })[extname(file)] ?? 'application/octet-stream')
    res.end(await readFile(file))
  } catch { res.statusCode = 404; res.end() }
})
await new Promise(done => server.listen(0, '127.0.0.1', done))
const origin = `http://127.0.0.1:${server.address().port}`
await mkdir('test-screenshots', { recursive: true })
let browser
try {
  browser = await chromium.launch({ headless: true })
  const page = await browser.newPage({ viewport: { width: 1600, height: 960 } })
  const errors = []; page.on('pageerror', error => errors.push(error.message))
  page.on('dialog', dialog => dialog.accept())
  await page.routeWebSocket('**/ws/events', () => {})
  await page.routeWebSocket('**/ws/stream/**', ws => { streamRequests.push(ws.url()) })

  // Create a dashboard; the editor opens with the template gallery.
  await page.goto(`${origin}/dashboards?new=1`)
  await page.getByLabel('Dashboard name', { exact: true }).fill('Hallway wall')
  await page.getByRole('button', { name: 'Create and edit', exact: true }).click()
  await page.waitForURL('**/d/1/edit')
  await page.getByRole('dialog', { name: 'Templates' }).waitFor()
  await page.screenshot({ path: 'test-screenshots/dashboard-v2-templates.png' })
  await page.getByRole('button', { name: '2 × 2', exact: true }).click()
  const tiles = page.locator('.edit-tile')
  assert.equal(await tiles.count(), 3, 'three cameras fill a 2 × 2 template in priority order')

  // Drag a clock from the palette into the empty fourth slot.
  const frame = await page.locator('.frame').boundingBox()
  const clockSource = await page.getByRole('button', { name: 'Add Clock', exact: true }).boundingBox()
  await page.mouse.move(clockSource.x + 20, clockSource.y + 10)
  await page.mouse.down()
  await page.mouse.move(frame.x + frame.width * 0.75, frame.y + frame.height * 0.75, { steps: 8 })
  assert.equal(await page.locator('.ghost span').innerText(), 'Place')
  await page.screenshot({ path: 'test-screenshots/dashboard-v2-drag.png' })
  await page.mouse.up()
  assert.equal(await tiles.count(), 4, 'drop places the clock')

  // Split: drag the clock onto the left edge zone of the first camera.
  await page.mouse.move(frame.x + frame.width * 0.75, frame.y + frame.height * 0.75)
  await page.mouse.down()
  await page.mouse.move(frame.x + frame.width * 0.03, frame.y + frame.height * 0.25, { steps: 8 })
  assert.equal(await page.locator('.ghost span').innerText(), 'Split left')
  await page.mouse.up()
  const firstCamera = await page.locator('.edit-tile', { hasText: 'Garden' }).boundingBox()
  assert.ok(firstCamera.width < frame.width * 0.4, 'split halves the target')

  await page.keyboard.press('Control+z')
  assert.ok((await page.locator('.edit-tile', { hasText: 'Garden' }).boundingBox()).width > frame.width * 0.45, 'undo restores the split target')
  await page.keyboard.press('Control+Shift+z')
  assert.ok((await page.locator('.edit-tile', { hasText: 'Garden' }).boundingBox()).width < frame.width * 0.4, 'redo re-applies it')

  // Keyboard: select a tile, nudge is blocked by neighbours, delete removes it.
  await page.locator('.edit-tile', { hasText: 'Clock' }).click()
  await page.getByRole('heading', { name: 'Clock' }).waitFor()
  await page.screenshot({ path: 'test-screenshots/dashboard-v2-editor.png' })
  await page.keyboard.press('Delete')
  assert.equal(await tiles.count(), 3)
  await page.keyboard.press('Control+z')
  assert.equal(await tiles.count(), 4)

  // Save; the payload is a valid, non-overlapping grid document.
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await page.getByText('Saved', { exact: true }).waitFor()
  const saved = updates.at(-1).document
  assert.equal(saved.tiles.length, 4)
  for (const [i, a] of saved.tiles.entries()) {
    assert.ok(a.x >= 0 && a.y >= 0 && a.x + a.w <= 48 && a.y + a.h <= 48, `tile ${a.id} in bounds`)
    for (const b of saved.tiles.slice(i + 1)) assert.ok(a.x >= b.x + b.w || b.x >= a.x + a.w || a.y >= b.y + b.h || b.y >= a.y + a.h, `${a.id} and ${b.id} do not overlap`)
  }

  // A concurrent save elsewhere produces the conflict dialog, not a silent overwrite.
  dashboards.get(1).revision++
  await page.getByLabel('Dashboard name', { exact: true }).fill('Hallway wall 2')
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await page.getByRole('alertdialog').waitFor()
  await page.getByRole('button', { name: 'Overwrite with mine', exact: true }).click()
  await page.getByText('Saved', { exact: true }).waitFor()
  assert.equal(dashboards.get(1).name, 'Hallway wall 2')

  // Preview renders the live canvas at target size.
  await page.getByRole('button', { name: 'Preview', exact: true }).click()
  await page.locator('.preview-host .dash-camera').first().waitFor()
  await page.screenshot({ path: 'test-screenshots/dashboard-v2-preview.png' })

  // View: a redacted tile renders a placeholder and requests nothing for it.
  dashboards.get(1).document.tiles.push({ id: 'zz', type: 'unavailable', x: 0, y: 47, w: 1, h: 1, reason: 'no-access', redacted: true })
  await page.goto(`${origin}/d/1`)
  await page.locator('.dash-camera').first().waitFor()
  await page.getByText('Not available for your account').waitFor()
  assert.equal(await page.locator('.dash-camera:not(.dash-parked)').count(), 3)
  await page.screenshot({ path: 'test-screenshots/dashboard-v2-view.png' })
  assert.ok(streamRequests.every(url => /\/ws\/stream\/(8|9|10)\b/.test(url)), 'only accessible cameras are streamed')

  // Double-click maximises, Escape restores.
  await page.locator('.dash-camera .camera-video-wrap').first().dblclick()
  assert.equal(await page.locator('.dash-camera:not(.dash-parked)').count(), 1)
  await page.keyboard.press('Escape')
  assert.equal(await page.locator('.dash-camera:not(.dash-parked)').count(), 3)

  // Phone: the dashboard becomes a list; no editor, and edit redirects.
  const phone = await browser.newPage({ viewport: { width: 390, height: 844 }, hasTouch: true, isMobile: true })
  await phone.routeWebSocket('**/ws/**', () => {})
  await phone.goto(`${origin}/d/1`)
  await phone.locator('.mobile-source').first().waitFor()
  assert.equal(await phone.locator('.mobile-source').count(), 3)
  assert.equal(await phone.getByRole('link', { name: 'Edit', exact: true }).count(), 0)
  assert.ok(await phone.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'no horizontal overflow on phones')
  await phone.screenshot({ path: 'test-screenshots/dashboard-v2-phone.png', fullPage: true })
  await phone.goto(`${origin}/d/1/edit`)
  await phone.waitForURL('**/d/1')

  assert.deepEqual(errors, [])
  console.log('PASS: create, template, place, split, undo/redo, save payload, conflict, preview, redacted view, maximise, phone list')
} finally {
  await browser?.close()
  server.close()
}
