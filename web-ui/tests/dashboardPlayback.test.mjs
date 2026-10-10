import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
import * as Vue from 'vue'

const compile = source => ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const moduleUrl = source => 'data:text/javascript;base64,' + Buffer.from(compile(source)).toString('base64')
const utilitiesUrl = moduleUrl(await readFile('src/utils/dashboardPlayback.ts', 'utf8'))
const utilities = await import(utilitiesUrl)
const source = (await readFile('src/composables/useDashboardPlayback.ts', 'utf8'))
  .replace(/import \{[^}]+\} from 'vue'/, 'const { computed, reactive, ref, onUnmounted, watch } = globalThis.__dashboardPlaybackVue')
  .replace("import { useSettingsStore } from '../stores/settings'", 'const useSettingsStore = () => globalThis.__dashboardPlaybackSettings')
  .replace("'../utils/dashboardPlayback'", JSON.stringify(utilitiesUrl))

test('independent and synced replay, shared clock, inactivity holds and live-only defaults', async () => {
  const cleanup = [], timers = []
  const originalNow = Date.now, originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval
  let now = 1_800_000_000_000
  Date.now = () => now
  globalThis.setInterval = callback => { timers.push(callback); return timers.length }
  globalThis.clearInterval = () => {}
  globalThis.__dashboardPlaybackVue = { ...Vue, onUnmounted: callback => cleanup.push(callback) }
  const settings = globalThis.__dashboardPlaybackSettings = Vue.reactive({ dashboardReturnLiveSeconds: 180 })
  const scope = Vue.effectScope()
  try {
    const { useDashboardPlayback } = await import(moduleUrl(source))
    const ids = Vue.ref([8, 9]), held = Vue.ref(false)
    const p = scope.run(() => useDashboardPlayback(ids, held, () => {}))
    assert.equal(p.sync.value, false)
    assert.equal(p.anyReplay.value, false)
    p.seek(8, now / 1000 - 300)
    assert.equal(p.get(9).requestedAt, null)
    p.observed(8, now / 1000 - 299)
    p.setSync(true)
    assert.equal(p.get(9).requestedAt, p.get(8).requestedAt)
    now += 2500; timers[0]()
    assert.equal(p.get(8).viewedAt, p.get(9).viewedAt)
    const beforePause = p.get(8).viewedAt
    p.pause(8); now += 10_000; timers[0]()
    assert.equal(p.get(8).viewedAt, beforePause)
    p.setSync(false)
    assert.equal(p.get(8).viewedAt, p.get(9).viewedAt)
    p.seek(8, now / 1000 - 100)
    assert.notEqual(p.get(8).viewedAt, p.get(9).viewedAt)
    p.hold(8, true); now += 181_000; timers[0]()
    assert.notEqual(p.get(8).requestedAt, null, 'open menu holds the return timer')
    assert.equal(p.get(9).requestedAt, null, 'independent timer returns the other camera live')
    p.hold(8, false); timers[0]()
    assert.notEqual(p.get(8).requestedAt, null, 'closing a menu restarts inactivity window')
    now += 181_000; timers[0](); assert.equal(p.get(8).requestedAt, null)
    settings.dashboardReturnLiveSeconds = 0
    p.seek(8, now / 1000 - 300); now += 600_000; timers[0]()
    assert.notEqual(p.get(8).requestedAt, null)
    settings.dashboardReturnLiveSeconds = 180
    held.value = true; await Vue.nextTick(); now += 200_000; timers[0]()
    assert.notEqual(p.get(8).requestedAt, null, 'save/search dialogs hold inactivity')
    held.value = false; await Vue.nextTick()
    p.setSync(true); p.live(9)
    assert.equal(p.get(8).requestedAt, null); assert.equal(p.get(9).requestedAt, null)
    p.seek(8, now / 1000 - 60)
    ids.value = [8, 10]; await Vue.nextTick()
    assert.equal(p.get(10).requestedAt, p.get(8).requestedAt)
    assert.equal(p.positions[9], undefined)
    const fresh = scope.run(() => useDashboardPlayback(Vue.ref([8]), Vue.ref(false), () => {}))
    assert.equal(fresh.get(8).requestedAt, null); assert.equal(fresh.sync.value, false)
    assert.equal(utilities.isSavingAt(10, [{ from: 10, to: 20 }]), true)
    assert.equal(utilities.isSavingAt(20, [{ from: 10, to: 20 }]), false)
    assert.equal(utilities.inactivitySeconds('garbage'), 180)
  } finally {
    for (const close of cleanup) close()
    scope.stop(); Date.now = originalNow; globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear
  }
})
