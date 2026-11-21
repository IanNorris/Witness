import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'

const code = ts.transpileModule(await readFile('src/utils/editorHistory.ts', 'utf8'), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2020 } }).outputText
const { EditorHistory } = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'))

test('undo and redo walk snapshots; new edits clear redo', () => {
  const history = new EditorHistory(JSON.stringify({ n: 0 }))
  assert.equal(history.push({ n: 1 }), true)
  assert.equal(history.push({ n: 2 }), true)
  assert.equal(history.push({ n: 2 }), false, 'unchanged state is not recorded')
  assert.deepEqual(history.undo(), { n: 1 })
  assert.deepEqual(history.undo(), { n: 0 })
  assert.equal(history.undo(), null)
  assert.deepEqual(history.redo(), { n: 1 })
  history.push({ n: 5 })
  assert.equal(history.canRedo, false)
})

test('inspector edits with the same key coalesce within the window', () => {
  const history = new EditorHistory(JSON.stringify({ name: '' }))
  history.push({ name: 'W' }, 'name', 0)
  history.push({ name: 'Wa' }, 'name', 100)
  history.push({ name: 'Wal' }, 'name', 200)
  assert.deepEqual(history.undo(), { name: '' }, 'typing is one undo step')
  history.redo()
  history.push({ name: 'Wall' }, 'name', 5000)
  assert.deepEqual(history.undo(), { name: 'Wal' }, 'outside the window starts a new step')
})

test('history is bounded', () => {
  const history = new EditorHistory(JSON.stringify(0), 3)
  for (let i = 1; i <= 10; i++) history.push(i)
  let steps = 0
  while (history.undo() !== null) steps++
  assert.equal(steps, 3)
})
