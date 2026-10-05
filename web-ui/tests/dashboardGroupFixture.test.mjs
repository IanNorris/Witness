import test from 'node:test'
import assert from 'node:assert/strict'
import { startPtzDashboard } from './fixtures/ptzDashboard.mjs'

for (const [name, options, count] of [
  ['default groups remain empty', {}, 0],
  ['many groups exercise the compact picker', { manyGroups: true }, 6],
  ['few long groups exercise wrapping buttons', { fewLongGroups: true }, 2],
]) {
  test(name, async () => {
    const fixture = await startPtzDashboard({ layout: true, ...options })
    try {
      assert.match(fixture.origin, /^http:\/\/127\.0\.0\.1:\d+$/)
      const result = await (await fetch(`${fixture.origin}/group/enum`)).json()
      assert.equal(result.count, count)
      assert.equal(result.groups.length, count)
      assert.equal(new Set(result.groups.map(group => group.id)).size, count)
      for (const camera of fixture.cameras) {
        assert.deepEqual(camera.groups, result.groups.map(group => group.id))
      }
      if (count) assert.ok(result.groups.some(group => group.displayName.length > 50))
    } finally {
      await fixture.close()
    }
  })
}
