// Fails when the committed generated API types differ from what the contracts produce now.
import { mkdtempSync, readFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { generate } from './gen-api.mjs';

const committed = fileURLToPath(new URL('../src/api/', import.meta.url));
const tmp = mkdtempSync(join(tmpdir(), 'intermission-api-'));
try {
  await generate(tmp);
  const stale = ['schema.d.ts', 'report.d.ts', 'operations.ts'].filter((f) => {
    const path = join(committed, f);
    return !existsSync(path) || readFileSync(path, 'utf8') !== readFileSync(join(tmp, f), 'utf8');
  });
  if (stale.length > 0) {
    console.error(`generated API types are stale — run npm run gen:api (${stale.join(', ')})`);
    process.exitCode = 1;
  } else {
    console.log('generated API types are up to date');
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
