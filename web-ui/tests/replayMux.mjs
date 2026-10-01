// Replay a private native mux capture through Witness's actual MSE player.
// Run from web-ui: node tests/replayMux.mjs ../build-vs2026/capture.mp4 [seconds] [firefox]
// Replays every moof/mdat, not the original network-arrival jitter or partial boundaries.
// Optional --inject-lag (Chromium, >=20s) verifies pre-restart drift evidence
// using an intentional backwards seek, not a claim of production reproduction.
import { readFile, mkdtemp, copyFile, rm } from 'node:fs/promises'
import { createServer } from 'node:http'
import { join, resolve, extname, sep } from 'node:path'
import { tmpdir } from 'node:os'
import { createRequire } from 'node:module'
import { spawn, spawnSync } from 'node:child_process'
import { chromium } from 'playwright'
const { wsServer: WebSocketServer } = createRequire(import.meta.url)('../node_modules/playwright-core/lib/utilsBundle.js')

const data = await readFile(process.argv[2])
const seconds = Number(process.argv[3] ?? 90)
if (!Number.isFinite(seconds) || seconds < 10 || seconds > 1800) throw new Error('Replay duration must be 10..1800 seconds')
function boxes(buffer, from = 0, to = buffer.length) {
  const result = []
  for (let p = from; p + 8 <= to;) {
    const size = buffer.readUInt32BE(p), type = buffer.toString('ascii', p + 4, p + 8)
    if (size < 8 || p + size > to) break
    result.push({ from: p, to: p + size, type }); p += size
  }
  return result
}
const top = boxes(data)
const first = top.find(b => b.type === 'moof')?.from
if (first === undefined) throw new Error('No fragments in mux capture')
const init = data.subarray(0, first)
const fragments = []
let lastVideoTime = 0
for (let i = 0; i < top.length; i++) {
  const moof = top[i]
  if (moof.type !== 'moof' || top[i + 1]?.type !== 'mdat') continue
  let at = null
  for (const traf of boxes(data, moof.from + 8, moof.to).filter(b => b.type === 'traf')) {
    const children = boxes(data, traf.from + 8, traf.to)
    const tfhd = children.find(b => b.type === 'tfhd'), tfdt = children.find(b => b.type === 'tfdt')
    if (tfhd && tfdt && data.readUInt32BE(tfhd.from + 12) === 1)
      at = data[tfdt.from + 8] === 1 ? Number(data.readBigUInt64BE(tfdt.from + 12)) / 90000 : data.readUInt32BE(tfdt.from + 12) / 90000
  }
  // The muxer can emit audio-only moofs. They are real media, not padding;
  // dropping one makes a fake audio gap that reliably stalls Firefox.
  if (at !== null) lastVideoTime = at
  fragments.push({ at: at ?? lastVideoTime, bytes: data.subarray(moof.from, top[i + 1].to) })
}
if (!fragments.length) throw new Error('No video fragments in mux capture')
if (fragments[fragments.length - 1].at - fragments[0].at < seconds + 2)
  throw new Error('Capture is too short for the requested replay; avoid mistaking fixture EOF for a playback stall')
const root = resolve('../WitnessServer/Web')
const firefoxMode = process.argv[4] === 'firefox'
const injectLag = process.argv.includes('--inject-lag')
if (injectLag && (firefoxMode || seconds < 20)) throw new Error('--inject-lag requires Chromium and >=20 seconds')
const received = []
const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname
  if (path === '/test/report') {
    let text = ''; for await (const chunk of req) text += chunk
    const snapshot = JSON.parse(text); received.push(snapshot)
    console.log(JSON.stringify({ elapsed: received.length * 5, stats: snapshot.stats, state: snapshot.currentState,
      events: received.length === 1 ? snapshot.importantEvents : undefined }))
    return res.end('ok')
  }
  let value
  if (path === '/auth/profile') value = { csrf: 'test', username: 'tester', admin: false }
  else if (path === '/camera/enum') value = [{ id: 13, name: 'Mux replay', groups: [], status: 'Connected', codec: 'h264', width: 896, height: 512, hasSubStream: true, subCodec: 'h264' }]
  else if (path === '/group/enum') value = { groups: [] }
  else if (path.startsWith('/camera/') || req.method === 'POST') value = {}
  if (value !== undefined) { res.setHeader('Content-Type', 'application/json'); return res.end(JSON.stringify(value)) }
  try {
    const file = path.startsWith('/assets/') ? join(root, path.slice(1)) : join(root, 'index.html')
    res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html' })[extname(file)] ?? 'application/octet-stream')
    let body = await readFile(file)
    if (firefoxMode && extname(file) === '.html') body = body.toString().replace('</head>', `<script>
      setInterval(() => { const d = Object.values(window._witnessMseDiag || {})[0]; if(d) fetch('/test/report',
        {method:'POST',body:JSON.stringify(d.snapshot())}).catch(()=>{}); }, 5000);
    </script></head>`)
    res.end(body)
  } catch { res.statusCode = 404; res.end() }
})
await new Promise(done => server.listen(0, '127.0.0.1', done))
let browser
let firefoxProcess, firefoxProfile
function verifyPlayback(snapshot) {
  if (injectLag) {
    const restart = snapshot.lastDriftRestart
    if (!restart || restart.drift?.lagMs <= 5000 || !restart.drift?.hasAudio ||
        restart.drift?.futureSafeKeyframes !== 0 || !restart.driftSamples?.length ||
        !snapshot.driftSamples?.length || snapshot.stats.stallCount || snapshot.stats.errorCount)
      throw new Error('Controlled drift did not retain expected healthy-playback evidence')
    const history = restart.driftSamples
    if (!history.some(sample => sample.playbackAdvanceMs > 0 && sample.lagChangeMs < 0) ||
        !snapshot.driftSamples.some(sample => sample.generation !== restart.drift.generation))
      throw new Error('Drift history did not capture catch-up progress and post-restart generation')
    console.log('PASS: controlled drift captures lag, progress, muxed audio, no safe seek targets, pre-restart history and recovery generation')
    return
  }
  if (snapshot.stats.restartCount || snapshot.stats.stallCount || snapshot.stats.errorCount ||
      snapshot.currentState.error || snapshot.currentState.paused ||
      snapshot.currentState.currentTime < seconds - 8)
    throw new Error('Replay did not maintain continuous playback; see FINAL diagnostics')
}
const timers = new Set()
function stream(socket) {
    let closed = false
    socket.onClose(() => { closed = true })
    const later = (fn, ms) => { const timer = setTimeout(() => { timers.delete(timer); if (!closed) fn() }, ms); timers.add(timer) }
    later(() => {
      socket.send(JSON.stringify({ type: 'streamSelection', stream: 'sub', codec: 'h264', width: 896, height: 512 }))
      socket.send(JSON.stringify({ type: 'initSegment', generation: 0, audioCodec: 'mp4a.40.2', bytes: init.length }))
      socket.send(init)
      for (let i = 0; i < fragments.length; i++) {
        const fragment = fragments[i]
        later(() => {
          socket.send(JSON.stringify({ type: 'partial', generation: 0, segmentIndex: Math.floor(fragment.at / 2), partIndex: i,
            independent: i === 0, keyframeSeekSafe: false, duration: fragments[i + 1]?.at - fragment.at || .15, bytes: fragment.bytes.length }))
          socket.send(fragment.bytes)
        }, Math.max(0, fragment.at - fragments[0].at) * 1000)
      }
    }, 200)
}
const sockets = new WebSocketServer({ server })
sockets.on('connection', (socket, req) => {
  if (!req.url.startsWith('/ws/stream/')) return
  stream({ send: data => socket.readyState === 1 && socket.send(data), onClose: cb => socket.on('close', cb) })
})
try {
  const origin = `http://127.0.0.1:${server.address().port}/live/13`
  if (firefoxMode) {
    firefoxProfile = await mkdtemp(join(tmpdir(), 'witness-firefox-mux-'))
    await copyFile('tests/firefox-mux-user.js', join(firefoxProfile, 'user.js'))
    firefoxProcess = spawn('C:/Program Files/Mozilla Firefox/firefox.exe', ['-headless', '-no-remote', '-profile', firefoxProfile, origin],
      { windowsHide: true, stdio: 'ignore' })
    await new Promise(done => setTimeout(done, seconds * 1000 + 5000))
    if (!received.length) throw new Error('Firefox supplied no MSE diagnostic reports')
    const last = received[received.length - 1]
    console.log('FINAL', JSON.stringify({ file: process.argv[2], browser: 'firefox', stats: last.stats, state: last.currentState,
      events: last.importantEvents?.filter(e => /restart|error|catchUp|initialSeek/i.test(e.type)) }))
    verifyPlayback(last)
  } else {
  browser = await chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] })
  const page = await browser.newPage()
  const samples = []
  await page.goto(origin)
  if (injectLag) await page.evaluate(() => {
    setTimeout(() => {
      const video = document.querySelector('video')
      video.currentTime = Math.max(video.buffered.start(0) + .1, video.currentTime - 4.2)
    }, 10000)
  })
  for (let elapsed = 0; elapsed < seconds; elapsed += 5) {
    await new Promise(done => setTimeout(done, 5000))
    const snapshot = await page.evaluate(() => {
      const diag = Object.values(window._witnessMseDiag ?? {})[0]
      return diag?.snapshot()
    })
    if (!snapshot) throw new Error('MSE diagnostics unavailable')
    samples.push(snapshot)
    console.log(JSON.stringify({ elapsed: elapsed + 5, stats: snapshot.stats, state: snapshot.currentState,
      events: elapsed === 0 ? snapshot.importantEvents : undefined }))
  }
  const last = samples[samples.length - 1]
  console.log('FINAL', JSON.stringify({ file: process.argv[2], stats: last.stats, state: last.currentState,
    events: last.importantEvents?.filter(e => /restart|error|catchUp|initialSeek/i.test(e.type)) }))
  verifyPlayback(last)
  }
} finally {
  for (const timer of timers) clearTimeout(timer)
  await browser?.close()
  if (firefoxProcess?.pid) spawnSync('taskkill', ['/PID', String(firefoxProcess.pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' })
  for (const socket of sockets.clients) socket.terminate()
  sockets.close()
  await new Promise(done => server.close(done))
  if (firefoxProfile) {
    if (!resolve(firefoxProfile).startsWith(resolve(tmpdir()) + sep)) throw new Error('Unexpected temporary profile location')
    await rm(firefoxProfile, { recursive: true, force: true, maxRetries: 10, retryDelay: 300 })
  }
}
