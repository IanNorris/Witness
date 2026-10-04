import { build } from 'esbuild'
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const repository = fileURLToPath(new URL('../../../', import.meta.url))
await build({
  entryPoints: [resolve(repository, 'web-ui/tests/fixtures/clientHealthBrowser.ts')],
  bundle: true, format: 'esm', platform: 'browser',
  outfile: resolve(repository, 'artifacts/client-health-browser-reporter.js'),
  define: { __BUILD_HASH__: JSON.stringify('browser-history-fixture') },
})
