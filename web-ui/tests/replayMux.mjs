// Replay a private native mux capture through Witness's actual MSE player.
// Run from web-ui: node tests/replayMux.mjs ../build-vs2026/capture.mp4 [seconds] [firefox]
// Replays every moof/mdat, not the original network-arrival jitter or partial boundaries.
// Optional --inject-lag (Chromium, >=20s) verifies pre-restart drift evidence
// using an intentional backwards seek, not a claim of production reproduction.
// --serve runs only the loopback fixture for computer-use browser inspection.
// No browser process is launched in that mode. Optional --camera N labels the
// replay with its actual source camera; the configured server is never contacted.
import { readFile, mkdtemp, copyFile, rm } from 'node:fs/promises'
import { createServer } from 'node:http'
import { join, resolve, extname, sep } from 'node:path'
import { tmpdir } from 'node:os'
import { createRequire } from 'node:module'
import { spawn, spawnSync } from 'node:child_process'
import { chromium } from 'playwright'
import { loadMuxReplay, streamMuxReplay } from './fixtures/muxReplay.mjs'
const { wsServer: WebSocketServer } = createRequire(import.meta.url)('../node_modules/playwright-core/lib/utilsBundle.js')

const seconds = Number(process.argv[3] ?? 90)
const replay = await loadMuxReplay(process.argv[2], seconds)
const { fragments, hasAudio } = replay
const root = resolve('../WitnessServer/Web')
const firefoxMode = process.argv[4] === 'firefox'
const serveMode = process.argv.includes('--serve')
const cameraOption = process.argv.indexOf('--camera')
const cameraId = cameraOption < 0 ? 13 : Number(process.argv[cameraOption + 1])
if (!Number.isSafeInteger(cameraId) || cameraId <= 0) throw new Error('Invalid replay camera ID')
const injectLag = process.argv.includes('--inject-lag')
const blockAudio = process.argv.includes('--block-audio')
if (serveMode && (firefoxMode || injectLag || blockAudio)) throw new Error('--serve uses external browser control, without automated injection modes')
if (blockAudio && (firefoxMode || injectLag)) throw new Error('--block-audio requires Chromium without --inject-lag')
if (injectLag && (firefoxMode || seconds < 20)) throw new Error('--inject-lag requires Chromium and >=20 seconds')
const received = []
let finishServing
const servingStopped = new Promise(done => { finishServing = done })
const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname
  if (path === '/test/report') {
    let text = ''; for await (const chunk of req) {
      text += chunk
      if (text.length > 256 * 1024) { res.statusCode = 413; return res.end() }
    }
    let snapshot
    try { snapshot = JSON.parse(text) } catch { res.statusCode = 400; return res.end() }
    if (!snapshot || typeof snapshot !== 'object' || !snapshot.stats || !snapshot.currentState) {
      res.statusCode = 400; return res.end()
    }
    if (received.length >= 400) { res.statusCode = 429; return res.end() }
    received.push(snapshot)
    const observedElapsed = (Date.now() - Date.parse(snapshot.startTime)) / 1000
    console.log(JSON.stringify({ elapsed: Number.isFinite(observedElapsed) ? observedElapsed : null,
      stats: snapshot.stats, state: snapshot.currentState,
      events: received.length === 1 ? snapshot.importantEvents : undefined }))
    return res.end('ok')
  }
  if (serveMode && path === '/test/stop' && req.method === 'POST') {
    res.end('ok'); finishServing(); return
  }
  let value
  if (path === '/auth/profile') value = { csrf: 'test', username: 'tester', admin: false }
  else if (path === '/camera/enum') value = [{ id: cameraId, name: 'Private mux replay', groups: [], status: 'Connected', codec: 'h264', width: 896, height: 512, hasSubStream: true, subCodec: 'h264' }]
  else if (path === '/group/enum') value = { groups: [] }
  else if (path.startsWith('/camera/') || req.method === 'POST') value = {}
  if (value !== undefined) { res.setHeader('Content-Type', 'application/json'); return res.end(JSON.stringify(value)) }
  try {
    const file = path.startsWith('/assets/') ? join(root, path.slice(1)) : join(root, 'index.html')
    if (!resolve(file).startsWith(root + sep)) { res.statusCode = 403; return res.end() }
    res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html' })[extname(file)] ?? 'application/octet-stream')
    let body = await readFile(file)
    if (firefoxMode && extname(file) === '.html') body = body.toString().replace('</head>', `<script>
      setInterval(() => { const d = Object.values(window._witnessMseDiag || {})[0]; if(d) fetch('/test/report',
        {method:'POST',body:JSON.stringify(d.snapshot())}).catch(()=>{}); }, 5000);
    </script></head>`)
    if (serveMode && extname(file) === '.html') body = body.toString().replace('</head>', `<script>
      const replayStarted = performance.now();
      let replayLastReport = 0;
      setInterval(() => {
        const d = Object.values(window._witnessMseDiag || {})[0];
        if (!d) return;
        let panel = document.getElementById('replay-evidence');
        if (!panel) {
          panel = document.createElement('pre'); panel.id = 'replay-evidence';
          panel.style.cssText = 'position:fixed;right:12px;top:12px;z-index:99999;padding:12px;background:#111e;color:#eee;font:12px monospace;max-width:460px;max-height:80vh;overflow:auto;pointer-events:none';
          document.body.appendChild(panel);
        }
        const snapshot = d.snapshot();
        const state = snapshot.currentState;
        if (performance.now() - replayLastReport >= 5000) {
          replayLastReport = performance.now();
          fetch('/test/report', {method:'POST',body:JSON.stringify(snapshot)}).catch(()=>{});
        }
        panel.textContent = JSON.stringify({ fixture: 'Private mux replay, no camera connection',
          elapsed: (performance.now()-replayStarted)/1000, stats: snapshot.stats,
          state: { currentTime: state?.currentTime, paused: state?.paused,
            readyState: state?.readyState, error: state?.error, quality: state?.playbackQuality },
          pipeline: { queue: snapshot.liveState?.appendQueueLength,
            queueBytes: snapshot.liveState?.appendQueueBytes, lastFragAge: snapshot.liveState?.lastFragAge,
            latencyMs: snapshot.liveState?.latencyMs, selectedStream: snapshot.liveState?.selectedStream },
        }, null, 2);
      }, 1000);
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
    const cancel = streamMuxReplay(socket, replay)
    timers.add(cancel)
    socket.onClose(() => timers.delete(cancel))
}
const sockets = new WebSocketServer({ server })
sockets.on('connection', (socket, req) => {
  if (!req.url.startsWith('/ws/stream/')) return
  stream({ send: data => socket.readyState === 1 && socket.send(data), onClose: cb => socket.on('close', cb) })
})
try {
  const origin = `http://127.0.0.1:${server.address().port}/live/${cameraId}`
  if (serveMode) {
    console.log('replayOrigin=' + origin)
    console.log('replayVideoFragments=' + fragments.length + ' hasAudio=' + hasAudio)
    // Browser-control startup/observation round trips are outside replay time.
    // Allow setup headroom, but still bound the listener and support early Stop.
    const deadline = setTimeout(finishServing, (seconds + 90) * 1000)
    await servingStopped
    clearTimeout(deadline)
    const last = received.at(-1)
    if (last) console.log('FINAL', JSON.stringify({ file: process.argv[2], stats: last.stats,
      state: last.currentState, observedReports: received.length }))
  } else if (firefoxMode) {
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
  if (blockAudio) await page.addInitScript(() => {
    localStorage.setItem('witness-live-audio-13', '1')
    const original = HTMLMediaElement.prototype.play
    HTMLMediaElement.prototype.play = function () {
      if (!this.muted) return Promise.reject(new DOMException('Injected autoplay refusal', 'NotAllowedError'))
      return original.call(this)
    }
  })
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
  if (blockAudio) {
    const events = last.importantEvents ?? []
    if (!events.some(e => e.type === 'audioBlocked') || !events.some(e => e.type === 'mutedPlaybackResumed'))
      throw new Error('Autoplay refusal was not diagnosed and recovered')
    await page.getByRole('status').filter({ hasText: 'browser blocked live audio' }).waitFor()
    await page.getByRole('button', { name: 'Dismiss audio warning' }).click()
    await page.locator('button[title="Play live audio"]').click()
    await new Promise(done => setTimeout(done, 1000))
    if (await page.getByRole('button', { name: 'Dismiss audio warning' }).count()) throw new Error('Warning repeated on explicit audio retry')
    const recovered = await page.evaluate(() => ({ paused: document.querySelector('video').paused, muted: document.querySelector('video').muted,
      saved: localStorage.getItem('witness-live-audio-13') }))
    if (recovered.paused || !recovered.muted || recovered.saved !== '1') throw new Error('Retry did not preserve muted playback and saved preference')
    console.log('PASS: injected autoplay refusal resumes muted playback, updates audio control, preserves preference and warns once')
  }
  }
} finally {
  for (const cancel of timers) cancel()
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
