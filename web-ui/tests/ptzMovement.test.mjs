import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
const source = ts.transpileModule(await readFile('src/composables/ptzMovement.ts', 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 },
}).outputText
const { createPtzMovement } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'))
test('release waits for move acknowledgement then stops the original camera', async () => {
  let release
  const calls = [], states = []
  const movement = createPtzMovement(async (id, command) => {
    calls.push([id, command])
    if (command !== 'stop') await new Promise(resolve => { release = resolve })
  }, state => states.push(state), () => assert.fail('unexpected failure'))
  movement.start(10, 'left', 32)
  await Promise.resolve()
  movement.stop()
  assert.deepEqual(calls, [[10, 'left']])
  release(); await movement.settled()
  assert.deepEqual(calls, [[10, 'left'], [10, 'stop']])
  assert.equal(states.at(-1), false)
  movement.dispose()
})
test('a gesture cancelled before dispatch never starts moving', async () => {
  const calls = []
  const movement = createPtzMovement(async (...args) => calls.push(args), () => {}, () => {})
  movement.start(10, 'left', 32); movement.stop()
  await movement.settled()
  assert.equal(calls.length, 0)
  movement.dispose()
})
test('camera replacement and disposal preserve stop ordering', async () => {
  const calls = []
  const movement = createPtzMovement(async (id, command) => calls.push([id, command]), () => {}, () => {})
  movement.start(10, 'left', 32); await movement.settled()
  movement.start(12, 'right', 32); await movement.settled()
  movement.dispose(); await movement.settled()
  assert.deepEqual(calls, [[10, 'left'], [10, 'stop'], [12, 'right'], [12, 'stop']])
  movement.start(10, 'left', 32); await movement.settled()
  assert.equal(calls.length, 4)
})
test('failed moves still attempt a stop, without poisoning subsequent commands', async () => {
  const calls = [], failures = []
  const movement = createPtzMovement(async (id, command) => {
    calls.push(command)
    if (command === 'left') throw new Error('request failed after possible delivery')
  }, () => {}, command => failures.push(command))
  movement.start(10, 'left', 32)
  await movement.settled() // also drains the stop appended by the failure handler
  assert.deepEqual(calls, ['left', 'stop'])
  assert.deepEqual(failures, ['left'])
  movement.emergencyStop(10, 32); await movement.settled()
  assert.deepEqual(calls, ['left', 'stop', 'stop'])
  movement.dispose()
})
test('a failed stop is reported and does not block a subsequent emergency stop', async () => {
  const calls = [], failures = []
  let failStop = true
  const movement = createPtzMovement(async (id, command) => {
    calls.push([id, command])
    if (command === 'stop' && failStop) { failStop = false; throw new Error('offline') }
  }, () => {}, command => failures.push(command))
  movement.start(10, 'right', 32); await movement.settled()
  movement.stop(); await movement.settled()
  movement.emergencyStop(10, 32); await movement.settled()
  assert.deepEqual(failures, ['stop'])
  assert.deepEqual(calls, [[10, 'right'], [10, 'stop'], [10, 'stop']])
  movement.dispose()
})
