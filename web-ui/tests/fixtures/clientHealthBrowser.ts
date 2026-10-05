// Bundled against the actual reporter/API. Served only by the native loopback
// fixture with a disposable auth/history database, never a production server.
import { collectLocalClientHealth, startClientHealthReporting } from '../../src/composables/useClientHealth'
import { api, setCsrfToken } from '../../src/composables/useApi'

const camera = location.pathname.endsWith('/two') ? 9 : 8
let frames = 0
;(window as any)._witnessMseDiag = {
  [`${camera}_mse`]: { snapshot: () => ({
    currentState: { readyState: 3, playbackQuality: { totalVideoFrames: frames, droppedVideoFrames: 0 } },
    liveState: { selectedStream: 'sub', latencyMs: camera * 100, wsReadyState: 1 },
    stats: { restartCount: 0, stallCount: 0, errorCount: 0 },
    driftSamples: [{ t: new Date().toISOString(), currentTime: 11.4, bufferedEnd: 14,
      bufferedRangeCount: 2, playheadBuffered: false, nextBufferedStartSeconds: 13,
      playheadGapMs: 1600, headroomInCurrentRangeMs: 0, hasAudio: true,
      sourceBufferMode: 'sequence', timestampOffsetSeconds: -3,
      buffered: [[1, 2], [13, 14]], url: 'https://fixture-secret@camera' }],
  }) },
}
setCsrfToken('admin-csrf')
let allowed = true
const stop = startClientHealthReporting(() => allowed)
const identity = document.querySelector('#identity')!
const history = document.querySelector('#history')!
function renderIdentity() {
  const report = collectLocalClientHealth()
  identity.textContent = JSON.stringify({ browserId: report.browserId, sessionId: report.sessionId,
    visibility: report.visibilityState, player: report.players[0]?.id, reporting: allowed }, null, 2)
}
const timer = setInterval(() => { ++frames; renderIdentity() }, 1000)
renderIdentity()
document.querySelector('#stop')!.addEventListener('click', () => { allowed = false; stop(); renderIdentity() })
document.querySelector('#export')!.addEventListener('click', async () => {
  try { history.textContent = JSON.stringify(await api('/debug/client-health'), null, 2) }
  catch (error) { history.textContent = String(error) }
})
window.addEventListener('pagehide', () => { stop(); clearInterval(timer) }, { once: true })
