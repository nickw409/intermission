// Validates real run reports (not fixtures) against contracts/report.schema.json.
// Usage: node tools/contracts/validate-reports.mjs <dir>  — every *.json directly in <dir> must validate.
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import Ajv2020 from 'ajv/dist/2020.js';
import addFormats from 'ajv-formats';

const dir = process.argv[2];
if (!dir || !existsSync(dir)) { console.error(`reports dir not found: ${dir}`); process.exit(1); }
const schemaPath = fileURLToPath(new URL('../../contracts/report.schema.json', import.meta.url));
const ajv = new Ajv2020({ allErrors: true, strict: true });
addFormats(ajv);
const validate = ajv.compile(JSON.parse(readFileSync(schemaPath, 'utf8')));
const files = readdirSync(dir).filter((f) => f.endsWith('.json'));
if (files.length === 0) { console.error(`no reports in ${dir}`); process.exit(1); }
let bad = 0;
for (const f of files) {
  if (!validate(JSON.parse(readFileSync(join(dir, f), 'utf8')))) {
    bad++; console.error(`INVALID ${f}: ${ajv.errorsText(validate.errors)}`);
  }
}
console.log(`${files.length - bad}/${files.length} reports valid in ${dir}`);
process.exit(bad ? 1 : 0);
