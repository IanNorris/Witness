// Offline check of bounded evidence produced by ptzDashboard --mux --evidence.
import { readFile, stat } from 'node:fs/promises'
import assert from 'node:assert/strict'

const file = process.argv[2], info = await stat(file)
assert(info.isFile() && info.size <= 128 * 1024 * 1024, 'Evidence file must be bounded')
const samples = (await readFile(file, 'utf8')).trim().split('\n').map(line => JSON.parse(line))
assert(samples.length >= 6 && samples.length <= 400, 'Need enough bounded samples across both transitions')
const ids = ['10_mse', '12_mse', '13_mse', '14_mse', '15_mse']
const starts = new Map(), times = new Map()
let promotedAt = -1, returnedAt = -1
for (const [index, sample] of samples.entries()) {
  assert.deepEqual(sample.report.players.map(p => p.id).sort(), [...ids].sort())
  for (const player of sample.report.players) {
    if (!starts.has(player.id)) starts.set(player.id, player.startTime)
    assert.equal(player.startTime, starts.get(player.id), `${player.id}: player recreated`)
    assert.equal(player.ready, 4); assert.equal(player.paused, false); assert.equal(player.error, null)
    for (const key of ['restartCount', 'stallCount', 'errorCount', 'integrityMismatchCount', 'decodeCorruptionCount'])
      assert.equal(player.stats[key], 0, `${player.id}: ${key}`)
    assert.equal(player.queue, 0)
    assert(Number.isFinite(player.time))
    if (times.has(player.id)) assert(player.time > times.get(player.id) + 2, `${player.id}: playback did not advance`)
    times.set(player.id, player.time)
    assert.deepEqual(sample.connections[player.id.split('_')[0]], { opened: 1, closed: 0 })
  }
  const visible = sample.report.tiles.filter(tile => tile.hidden === 'false').map(tile => tile.text)
  assert.equal(visible.length, 3)
  assert(visible.some(text => text.includes('Fixture PTZ 1')))
  if (visible.some(text => /Fixture PTZ 5.*Auto 1/.test(text)) && visible.some(text => /Fixture PTZ 4.*Auto 2/.test(text))) promotedAt = index
  if (promotedAt >= 0 && visible.some(text => /Fixture PTZ 2.*Auto 1/.test(text)) && visible.some(text => /Fixture PTZ 3.*Auto 2/.test(text))) returnedAt = index
}
assert(promotedAt >= 0 && returnedAt > promotedAt, 'Must observe ordered promotion followed by fallback')
console.log(JSON.stringify({ passed: true, samples: samples.length, players: ids.length,
  observedSeconds: (Date.parse(samples.at(-1).at) - Date.parse(samples[0].at)) / 1000,
  finalMediaTimes: Object.fromEntries(times), promotedAt, returnedAt }, null, 2))
