// Checks live responses recorded by the integration tests against the OpenAPI contracts.
//
//   node tests/integration/validate-observations.mjs <observations.jsonl> [--require-coverage]
//
// Each line of the input is one observed exchange, written by tests/integration (Go):
//   { "spec": "model"|"public", "operationId", "case", "method", "path",
//     "status", "headers": { lower-case name: value }, "body": <parsed JSON or null>,
//     "contentType", "bodyBytes" }
//
// For every observation:
//   - the operation exists in contracts/<spec>.yaml, and method and path match its template;
//   - the status is documented for that operation;
//   - a JSON body validates against the documented response schema (error bodies against Error);
//   - every response header the contract declares for that status is present;
//   - model spec: every response carries X-Model-Version (contract rule) and X-Request-ID;
//     public spec: every response carries X-Request-ID.
//   - the path is routed to this operation and no other: a literal template segment
//     (/api/movies/search) wins over a parameter ({movieId}), as in recs-api's router;
//   - a response whose contract declares no content has no body.
// With --require-coverage, every operation (minus the listed phase-6 exclusions) must have been
// observed with every documented status. A coverage table is printed either way.
//
// Uses the pinned ajv/yaml from tools/contracts (run `npm ci` there first).

import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..');
const CONTRACTS = join(REPO, 'contracts');
const require = createRequire(join(REPO, 'tools', 'contracts', 'package.json'));
const Ajv2020 = require('ajv/dist/2020.js').default;
const addFormats = require('ajv-formats').default ?? require('ajv-formats');
const { parse: parseYaml } = require('yaml');

const BASE = 'https://intermission.local/contracts/';
const METHODS = ['get', 'put', 'post', 'delete', 'patch'];
const OPENAPI_KEYWORDS = ['openapi', 'info', 'servers', 'security', 'tags', 'paths', 'components'];

// Operations outside this gate: getFeedbackStats is phase 6 and not built yet. postFeedback is
// built (not_interested) and observed, but not every documented status is exercised here (its
// 413 and 429 are covered by the recs-api unit tests against the fixtures).
const EXCLUDED = { public: new Set(['postFeedback', 'getFeedbackStats']), model: new Set() };

const args = process.argv.slice(2);
const file = args.find((a) => !a.startsWith('--'));
const requireCoverage = args.includes('--require-coverage');
if (!file) {
  console.error('usage: validate-observations.mjs <observations.jsonl> [--require-coverage]');
  process.exit(2);
}

const specs = {
  model: parseYaml(readFileSync(join(CONTRACTS, 'model.yaml'), 'utf8')),
  public: parseYaml(readFileSync(join(CONTRACTS, 'public.yaml'), 'utf8')),
};
const ajv = new Ajv2020({ allErrors: true, strict: true, allowUnionTypes: true });
addFormats(ajv);
ajv.addFormat('binary', true);
for (const kw of OPENAPI_KEYWORDS) ajv.addKeyword(kw);
ajv.addSchema(JSON.parse(readFileSync(join(CONTRACTS, 'report.schema.json'), 'utf8')));
for (const [name, doc] of Object.entries(specs)) ajv.addSchema({ ...doc, $id: `${BASE}${name}.yaml` });

const escapePtr = (s) => s.replaceAll('~', '~0').replaceAll('/', '~1');
const lookup = (root, pointer) => pointer.split('/').slice(1).reduce((n, p) => n?.[p.replaceAll('~1', '/').replaceAll('~0', '~')], root);
function deref(doc, obj, pointer) {
  while (obj && typeof obj.$ref === 'string' && obj.$ref.startsWith('#/')) {
    pointer = obj.$ref.slice(1);
    obj = lookup(doc, pointer);
  }
  return [obj, pointer];
}
const compiled = new Map();
function validator(spec, pointer) {
  const key = `${spec}#${pointer}`;
  if (!compiled.has(key)) compiled.set(key, ajv.compile({ $ref: `${BASE}${spec}.yaml#${pointer}` }));
  return compiled.get(key);
}

// operationId -> { path, method, op, pointer, re }
const ops = {};
for (const [spec, doc] of Object.entries(specs)) {
  ops[spec] = new Map();
  for (const [path, item] of Object.entries(doc.paths)) {
    for (const method of METHODS) {
      if (!item[method]) continue;
      // Escape regex specials in the template, then let each {param} match one path segment.
      const escaped = path.replace(/[.*+?^$()|[\]\\]/g, '\\$&');
      const re = new RegExp(`^${escaped.replace(/\{[^}/]+\}/g, '[^/]+')}$`);
      const literals = path.split('/').filter((seg) => seg !== '' && !seg.startsWith('{')).length;
      ops[spec].set(item[method].operationId, { path, method: method.toUpperCase(), op: item[method], pointer: `/paths/${escapePtr(path)}/${method}`, re, literals });
    }
  }
}

const errors = [];
const seen = new Map(); // "spec op status" -> count
const lines = readFileSync(file, 'utf8').split('\n').filter((l) => l.trim() !== '');
for (const [i, line] of lines.entries()) {
  const o = JSON.parse(line);
  const where = `#${i + 1} ${o.spec} ${o.operationId} [${o.case}] ${o.method} ${o.path} -> ${o.status}`;
  const fail = (msg) => errors.push(`${where}: ${msg}`);
  const info = ops[o.spec]?.get(o.operationId);
  if (!info) { fail('unknown operation'); continue; }
  const key = `${o.spec} ${o.operationId} ${o.status}`;
  seen.set(key, (seen.get(key) ?? 0) + 1);
  if (o.method !== info.method) fail(`method should be ${info.method}`);
  const bare = o.path.split('?')[0];
  if (!info.re.test(bare)) fail(`path does not match ${info.path}`);
  else if (routedTo(o.spec, o.method, bare) !== o.operationId) fail(`path is routed to ${routedTo(o.spec, o.method, bare)}, not ${o.operationId}`);
  const h = o.headers ?? {};

  if (o.spec === 'model' && !h['x-model-version']) fail('no X-Model-Version header (model.yaml: every response carries it)');
  if (!h['x-request-id']) fail('no X-Request-ID header');

  const doc = specs[o.spec];
  const declared = info.op.responses[String(o.status)];
  if (!declared) { fail(`status ${o.status} is not documented (documented: ${Object.keys(info.op.responses).join(', ')})`); continue; }
  const [resp, respPtr] = deref(doc, declared, `${info.pointer}/responses/${o.status}`);
  for (const name of Object.keys(resp.headers ?? {})) {
    if (!h[name.toLowerCase()]) fail(`declared header ${name} missing`);
  }
  const content = resp.content ?? {};
  const types = Object.keys(content);
  const ct = (o.contentType ?? '').split(';')[0].trim().toLowerCase();
  if (types.length === 0) {
    if ((o.bodyBytes ?? 0) > 0) fail(`contract declares no content, but the response has a ${o.bodyBytes}-byte body`);
    continue;
  }
  if (!types.includes(ct)) { fail(`content type '${ct}' not in ${types.join(', ')}`); continue; }
  if (ct === 'application/json') {
    if (o.body === null || o.body === undefined) { fail('JSON body missing or unparseable'); continue; }
    const v = validator(o.spec, `${respPtr}/content/application~1json/schema`);
    if (!v(o.body)) fail(`body does not match schema: ${v.errors.map((e) => `${e.instancePath || '/'} ${e.message}`).join('; ')}`);
  }
}

// routedTo returns the operation a request would reach: among the operations with this method
// whose template matches the path, the one with the most literal segments.
function routedTo(spec, method, path) {
  let best;
  for (const [id, info] of ops[spec]) {
    if (info.method !== method || !info.re.test(path)) continue;
    if (!best || info.literals > best.literals) best = { id, literals: info.literals };
  }
  return best?.id;
}

// Coverage table.
const missing = [];
console.log('coverage (operation: status=count; "-" = documented, not observed):');
for (const [spec, map] of Object.entries(ops)) {
  for (const [id, info] of map) {
    if (EXCLUDED[spec].has(id)) { console.log(`  ${spec} ${id}: excluded (phase 6)`); continue; }
    const cells = Object.keys(info.op.responses).map((s) => {
      const n = seen.get(`${spec} ${id} ${s}`) ?? 0;
      const k = `${spec} ${id} ${s}`;
      if (n === 0) missing.push(k);
      return `${s}=${n === 0 ? '-' : n}`;
    });
    console.log(`  ${spec} ${id}: ${cells.join(' ')}`);
  }
}
console.log(`${lines.length} observations checked`);

if (requireCoverage && missing.length > 0) {
  for (const k of missing) errors.push(`coverage: ${k} was never observed`);
}
if (errors.length > 0) {
  console.error(`FAIL: ${errors.length} problem(s)`);
  for (const e of errors) console.error(`  ${e}`);
  process.exit(1);
}
console.log('PASS: every observation matches the contract');
