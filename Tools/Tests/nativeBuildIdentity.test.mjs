import { mkdtemp, mkdir, writeFile, readFile, stat, rm } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { execFileSync } from 'node:child_process'
import assert from 'node:assert/strict'

const root = await mkdtemp(join(tmpdir(), 'witness-native-id-'))
const cmake = process.argv[2] ?? 'cmake'
try {
  for (const name of ['WitnessCommon', 'WitnessCamera', 'WitnessServer']) {
    await mkdir(join(root, name, 'Source'), { recursive: true })
    await writeFile(join(root, name, 'CMakeLists.txt'), '# test')
    await writeFile(join(root, name, 'Source', 'test.cpp'), '// native source')
  }
  await writeFile(join(root, 'CMakeLists.txt'), '# root')
  const header = join(root, 'NativeBuildIdentity.h')
  const build = () => execFileSync(cmake, [`-DSOURCE_ROOT=${root}`, `-DOUTPUT_HEADER=${header}`,
    '-DTOOLCHAIN=test', '-P', 'Tools/WriteNativeBuildIdentity.cmake'], { stdio: 'pipe' })
  build()
  const first = await readFile(header, 'utf8'), time = (await stat(header)).mtimeMs
  await mkdir(join(root, 'WitnessServer', 'Web'))
  await writeFile(join(root, 'WitnessServer', 'Web', 'index.html'), 'new web bundle')
  build()
  assert.equal(await readFile(header, 'utf8'), first)
  assert.equal((await stat(header)).mtimeMs, time, 'unchanged identity must not trigger native recompilation')
  await writeFile(join(root, 'WitnessCamera', 'Source', 'test.cpp'), '// changed camera source')
  build()
  const second = await readFile(header, 'utf8')
  assert.notEqual(second, first)
  await writeFile(join(root, 'vcpkg.json'), '{"builtin-baseline":"changed"}')
  build()
  assert.notEqual(await readFile(header, 'utf8'), second)
  console.log('PASS: source/manifest changes alter native identity; web-only changes leave identity and header timestamp unchanged')
} finally {
  await rm(root, { recursive: true, force: true })
}
