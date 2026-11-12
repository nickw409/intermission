// Contract validator: checks contracts/fixtures against contracts/*.yaml and report.schema.json.
// Usage: npm run validate   (exits 1 and lists every problem on failure)
//
// Fixture file format (contracts/fixtures/{public,model}/<operationId>.json):
//   { "operationId": "...", "cases": [ { "name", "request", "response" } ] }
//   request:  { method, path, query: {name: string}, headers: {}, body? | rawBody? | rawBodyBytes? }
//             rawBody is sent verbatim (e.g. "{not json"); rawBodyBytes: N means "send N bytes of 'a'"
//             (used for the 413 body_too_large cases).
//   response: { status, headers: {}, body? }  — body may be {"$fixtureRef": "reports/als.json"},
//             a path relative to contracts/fixtures/.

import { reportConsistency } from './report-consistency.mjs';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import Ajv2020 from 'ajv/dist/2020.js';
import addFormats from 'ajv-formats';
import { parse as parseYaml } from 'yaml';

const CONTRACTS = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'contracts');
const FIXTURES = join(CONTRACTS, 'fixtures');
const BASE = 'https://intermission.local/contracts/';
const METHODS = ['get', 'put', 'post', 'delete', 'patch', 'head', 'options'];
const OPENAPI_KEYWORDS = ['openapi', 'info', 'servers', 'security', 'tags', 'paths', 'components'];

const errors = [];
const warnings = [];
const fail = (where, msg) => errors.push(`${where}: ${msg}`);
let checkedCases = 0;

const readJson = (path) => JSON.parse(readFileSync(path, 'utf8'));
const escapePtr = (s) => s.replaceAll('~', '~0').replaceAll('/', '~1');
const unescapePtr = (s) => decodeURIComponent(s).replaceAll('~1', '/').replaceAll('~0', '~');
const isSuccess = (status) => status >= 200 && status < 300;
const header = (headers, name) => {
  const key = Object.keys(headers ?? {}).find((k) => k.toLowerCase() === name.toLowerCase());
  return key === undefined ? undefined : headers[key];
};
const fmtErrors = (errs) => (errs ?? []).map((e) => `${e.instancePath || '/'} ${e.message}`).join('; ');

// ---- load contracts ----
const reportSchema = readJson(join(CONTRACTS, 'report.schema.json'));
// Standalone JSON Schemas with no fixtures: they only have to compile.
const OTHER_SCHEMAS = ['per_user.schema.json', 'comparison.schema.json'];
const specs = {
  public: { file: 'public.yaml', doc: parseYaml(readFileSync(join(CONTRACTS, 'public.yaml'), 'utf8')) },
  model: { file: 'model.yaml', doc: parseYaml(readFileSync(join(CONTRACTS, 'model.yaml'), 'utf8')) },
};

function makeAjv(options = {}) {
  const ajv = new Ajv2020({ allErrors: true, strict: true, allowUnionTypes: true, ...options });
  addFormats(ajv);
  ajv.addFormat('binary', true); // OpenAPI's format for raw bytes
  for (const kw of OPENAPI_KEYWORDS) ajv.addKeyword(kw);
  ajv.addSchema(reportSchema);
  for (const { file, doc } of Object.values(specs)) ajv.addSchema({ ...doc, $id: BASE + file });
  return ajv;
}
const ajv = makeAjv();
const coercingAjv = makeAjv({ coerceTypes: true });
const compiled = new Map();
function validatorAt(file, pointer, coerce = false) {
  const key = `${coerce}|${file}#${pointer}`;
  if (!compiled.has(key)) {
    const target = { $ref: `${BASE}${file}#${pointer}` };
    const schema = coerce ? { type: 'object', properties: { v: target } } : target;
    try {
      compiled.set(key, (coerce ? coercingAjv : ajv).compile(schema));
    } catch (e) {
      // Report a broken schema once and let callers continue.
      fail(`${file}#${pointer}`, `schema does not compile: ${e.message}`);
      const broken = () => false;
      broken.errors = [{ instancePath: '', message: 'schema does not compile (see above)' }];
      compiled.set(key, broken);
    }
  }
  return compiled.get(key);
}

// ---- 1. every $ref in the OpenAPI files resolves ----
function lookup(root, pointer) {
  let node = root;
  for (const part of pointer.split('/').slice(1)) {
    if (node === null || typeof node !== 'object' || !(unescapePtr(part) in node)) return undefined;
    node = node[unescapePtr(part)];
  }
  return node;
}
function checkRefs(file, doc) {
  const walk = (node, at) => {
    if (Array.isArray(node)) return node.forEach((v, i) => walk(v, `${at}/${i}`));
    if (node === null || typeof node !== 'object') return;
    if (typeof node.$ref === 'string') {
      const [target, frag = ''] = node.$ref.split('#');
      const root = target === '' ? doc : existsSync(join(CONTRACTS, target)) ? readJson(join(CONTRACTS, target)) : undefined;
      if (root === undefined) fail(file, `${at}: $ref ${node.$ref} points to a missing file`);
      else if (lookup(root, frag) === undefined) fail(file, `${at}: $ref ${node.$ref} does not resolve`);
    }
    for (const [k, v] of Object.entries(node)) walk(v, `${at}/${escapePtr(k)}`);
  };
  walk(doc, '#');
}
for (const { file, doc } of Object.values(specs)) checkRefs(file, doc);

// A null value in an OpenAPI file almost always means an unquoted comma inside a YAML flow
// mapping (`{ description: a, b }` parses as keys `description` and `b`).
for (const { file, doc } of Object.values(specs)) {
  const walk = (node, at) => {
    if (node === null || typeof node !== 'object') return;
    for (const [k, v] of Object.entries(node)) {
      if (v === null && !Array.isArray(node) && !['const', 'default', 'example'].includes(k)) fail(file, `${at}/${escapePtr(k)} is null (unquoted comma in a flow mapping?)`);
      walk(v, `${at}/${escapePtr(k)}`);
    }
  };
  walk(doc, '#');
}

// Follows local $ref chains; returns [object, pointer of the object].
function deref(doc, obj, pointer) {
  while (obj && typeof obj.$ref === 'string' && obj.$ref.startsWith('#/')) {
    pointer = obj.$ref.slice(1);
    obj = lookup(doc, pointer);
  }
  return [obj, pointer];
}

// Compile every component schema so schema mistakes fail loudly.
for (const { file, doc } of Object.values(specs)) {
  for (const name of Object.keys(doc.components?.schemas ?? {})) {
    validatorAt(file, `/components/schemas/${escapePtr(name)}`);
  }
}

for (const f of OTHER_SCHEMAS) {
  try { ajv.compile(readJson(join(CONTRACTS, f))); } catch (e) { fail(f, `schema does not compile: ${e.message}`); }
}

// ---- 2. collect operations ----
function operations(file, doc) {
  const ops = new Map();
  for (const [path, item] of Object.entries(doc.paths ?? {})) {
    for (const method of METHODS) {
      const op = item[method];
      if (!op) continue;
      if (ops.has(op.operationId)) fail(file, `duplicate operationId ${op.operationId}`);
      const params = [...(item.parameters ?? []), ...(op.parameters ?? [])].map((p, i) => {
        const base = op.parameters?.includes(p) ? `/paths/${escapePtr(path)}/${method}/parameters/${op.parameters.indexOf(p)}` : `/paths/${escapePtr(path)}/parameters/${i}`;
        const [param, ptr] = deref(doc, p, base);
        return { ...param, schemaPointer: `${ptr}/schema` };
      });
      ops.set(op.operationId, { path, method: method.toUpperCase(), op, params, pointer: `/paths/${escapePtr(path)}/${method}` });
    }
  }
  return ops;
}

function pathRegex(template) {
  const names = [];
  const re = template.replace(/[.*+?^$()|[\]\\]/g, '\\$&').replace(/\\?\{([^}]+)\\?\}/g, (_, n) => { names.push(n); return '([^/]+)'; });
  return { re: new RegExp(`^${re}$`), names };
}

// ---- 3. fixture cases ----
function loadBody(where, body) {
  if (body && typeof body === 'object' && !Array.isArray(body) && typeof body.$fixtureRef === 'string') {
    const target = join(FIXTURES, body.$fixtureRef);
    if (!target.startsWith(FIXTURES) || !existsSync(target)) { fail(where, `$fixtureRef ${body.$fixtureRef} not found`); return undefined; }
    return readJson(target);
  }
  return body;
}

function validateCase(kind, file, doc, opInfo, c, where) {
  checkedCases++;
  const { request: rq, response: rs } = c;
  if (!rq || !rs || typeof c.name !== 'string') return fail(where, 'case needs name, request and response');
  if (rq.method !== opInfo.method) fail(where, `method ${rq.method} should be ${opInfo.method}`);
  if (typeof rq.query !== 'object' || typeof rq.headers !== 'object') fail(where, 'request needs query and headers objects');
  if (typeof rs.headers !== 'object') fail(where, 'response needs a headers object');
  if (['body', 'rawBody', 'rawBodyBytes'].filter((k) => k in rq).length > 1) fail(where, 'use only one of body, rawBody, rawBodyBytes');
  if ('rawBody' in rq && typeof rq.rawBody !== 'string') fail(where, 'rawBody must be a string');
  if ('rawBodyBytes' in rq && !Number.isInteger(rq.rawBodyBytes)) fail(where, 'rawBodyBytes must be an integer');
  for (const [k, v] of Object.entries(rq.query ?? {})) if (typeof v !== 'string') fail(where, `query ${k} must be a string`);

  const { re, names } = pathRegex(opInfo.path);
  const m = re.exec(rq.path ?? '');
  if (!m) return fail(where, `path ${rq.path} does not match ${opInfo.path}`);
  const success = isSuccess(rs.status);

  if (success) {
    // Parameters (path + query) must be valid for success cases.
    const values = { path: Object.fromEntries(names.map((n, i) => [n, decodeURIComponent(m[i + 1])])), query: rq.query ?? {} };
    for (const p of opInfo.params) {
      if (p.in === 'header') {
        if (p.required && header(rq.headers, p.name) === undefined) fail(where, `missing required header ${p.name}`);
        continue;
      }
      const v = values[p.in]?.[p.name];
      if (v === undefined) { if (p.required) fail(where, `missing required ${p.in} parameter ${p.name}`); continue; }
      if (v === '' && p.allowEmptyValue) continue; // an empty value the contract allows (it means "not set")
      const validate = validatorAt(file, p.schemaPointer, true);
      if (!validate({ v })) fail(where, `${p.in} parameter ${p.name}=${v}: ${fmtErrors(validate.errors)}`);
    }
    const declared = new Set(opInfo.params.filter((p) => p.in === 'query').map((p) => p.name));
    for (const k of Object.keys(rq.query ?? {})) if (!declared.has(k)) fail(where, `undeclared query parameter ${k}`);

    // Request body.
    const [rb, rbPtr] = deref(doc, opInfo.op.requestBody, `${opInfo.pointer}/requestBody`);
    if (rb) {
      if (!('body' in rq)) { if (rb.required) fail(where, 'success case needs a request body'); }
      else {
        const validate = validatorAt(file, `${rbPtr}/content/application~1json/schema`);
        if (!validate(rq.body)) fail(where, `request body: ${fmtErrors(validate.errors)}`);
      }
    } else if ('body' in rq || 'rawBody' in rq) fail(where, 'operation takes no request body');
  }

  // Response.
  const [resp, respPtr] = deref(doc, opInfo.op.responses?.[String(rs.status)], `${opInfo.pointer}/responses/${rs.status}`);
  if (!resp) return fail(where, `status ${rs.status} is not declared for this operation`);
  for (const h of Object.keys(resp.headers ?? {})) if (header(rs.headers, h) === undefined) fail(where, `response is missing declared header ${h}`);
  if (!resp.content) {
    if (rs.body !== undefined) fail(where, `status ${rs.status} has no response body`);
  } else {
    const ct = (header(rs.headers, 'Content-Type') ?? 'application/json').split(';')[0].trim();
    if (!resp.content[ct]) fail(where, `content type ${ct} not declared for status ${rs.status}`);
    else if (rs.body === undefined) fail(where, `status ${rs.status} needs a response body`);
    else {
      const body = loadBody(where, rs.body);
      const validate = validatorAt(file, `${respPtr}/content/${escapePtr(ct)}/schema`);
      if (body !== undefined && !validate(body)) fail(where, `response body: ${fmtErrors(validate.errors)}`);
    }
  }

  // Cross-cutting rules from the contract descriptions.
  if (kind === 'model' && !header(rs.headers, 'X-Model-Version')) fail(where, 'every recs-model response carries X-Model-Version');
  if (kind === 'public') {
    const code = rs.body?.error?.code;
    const mutating = ['POST', 'PUT', 'DELETE'].includes(rq.method);
    const csrfOk = header(rq.headers, 'X-Requested-With') === 'recs' && header(rq.headers, 'Origin') !== undefined;
    const hasSession = /(^|;\s*)session=/.test(header(rq.headers, 'Cookie') ?? '');
    const needsSession = (opInfo.op.security ?? doc.security ?? []).some((s) => 'session' in s);
    if (success && mutating && !csrfOk) fail(where, 'mutating success case must send Origin and X-Requested-With: recs');
    if (success && needsSession && !hasSession) fail(where, 'success case for a session route must send the session cookie');
    if (code === 'csrf_rejected' && csrfOk) fail(where, 'csrf_rejected case must omit Origin or X-Requested-With');
    if (code === 'unauthorized' && hasSession) fail(where, 'unauthorized case must not send a session cookie');
  }
}

function validateFixtures(kind, { file, doc }, skip = []) {
  const dir = join(FIXTURES, kind);
  const ops = operations(file, doc);
  const files = existsSync(dir) ? readdirSync(dir).filter((f) => f.endsWith('.json')) : [];
  for (const id of ops.keys()) if (!skip.includes(id) && !files.includes(`${id}.json`)) fail(`fixtures/${kind}`, `missing fixture for operation ${id}`);
  for (const f of files) {
    const where = `fixtures/${kind}/${f}`;
    let fx;
    try { fx = readJson(join(dir, f)); } catch (e) { fail(where, `invalid JSON: ${e.message}`); continue; }
    const opInfo = ops.get(fx.operationId);
    if (!opInfo) { fail(where, `operationId ${fx.operationId} is not in ${file}`); continue; }
    if (basename(f, '.json') !== fx.operationId) fail(where, `file name should be ${fx.operationId}.json`);
    if (!Array.isArray(fx.cases) || fx.cases.length === 0) { fail(where, 'needs a non-empty cases array'); continue; }
    const seen = new Set();
    for (const c of fx.cases) {
      if (seen.has(c.name)) fail(where, `duplicate case name ${c.name}`);
      seen.add(c.name);
      validateCase(kind, file, doc, opInfo, c, `${where} [${c.name}]`);
    }
    const statuses = new Set(fx.cases.map((c) => c.response?.status));
    if (!fx.cases.some((c) => isSuccess(c.response?.status))) fail(where, 'needs at least one success case');
    for (const s of Object.keys(opInfo.op.responses ?? {})) {
      if (!isSuccess(Number(s)) && !statuses.has(Number(s))) fail(where, `no case for declared status ${s}`);
    }
  }
  return files.length;
}

const publicCount = validateFixtures('public', specs.public);
const modelCount = validateFixtures('model', specs.model, ['metrics']);

// ---- 4. run reports ----
const reportsDir = join(FIXTURES, 'reports');
const validReports = [];
const unknownReports = [];
if (!existsSync(reportsDir)) {
  warnings.push('fixtures/reports does not exist yet; report fixtures were not checked');
} else {
  const validateReport = ajv.getSchema(reportSchema.$id);
  const files = readdirSync(reportsDir).filter((f) => f.endsWith('.json'));
  if (!files.includes('unknown_major.json')) fail('fixtures/reports', 'unknown_major.json (schema_version 2) is missing');
  for (const f of files) {
    const report = readJson(join(reportsDir, f));
    const ok = validateReport(report);
    if (f === 'unknown_major.json') {
      if (ok) fail(`fixtures/reports/${f}`, 'must be REJECTED by report.schema.json but it validated');
      else if (!validateReport.errors.some((e) => e.instancePath === '/schema_version')) fail(`fixtures/reports/${f}`, `rejected, but not for schema_version: ${fmtErrors(validateReport.errors)}`);
      else unknownReports.push(report);
    } else if (!ok) fail(`fixtures/reports/${f}`, fmtErrors(validateReport.errors));
    else {
      for (const pr of reportConsistency(report)) fail(`fixtures/reports/${f}`, pr);
      validReports.push({ file: f, report });
    }
  }

  // listReports must summarise every report fixture: known-major reports newest first, then
  // unknown-major ones with only run_id and schema_version set.
  const lr = existsSync(join(FIXTURES, 'public', 'listReports.json')) ? readJson(join(FIXTURES, 'public', 'listReports.json')) : null;
  const items = lr?.cases?.find((c) => isSuccess(c.response?.status))?.response?.body?.items;
  if (items) {
    const known = validReports.map(({ report: r }) => ({
      run_id: r.run_id, schema_version: r.schema_version, model: r.model, dataset: r.dataset, evaluated_on: r.evaluated_on, created_at: r.created_at, config: r.config,
      headline: { 'ndcg@10': r.metrics['ndcg@10'], 'recall@50': r.metrics['recall@50'], 'coverage@10': r.aggregates['coverage@10'] },
    })).sort((a, b) => Date.parse(b.created_at) - Date.parse(a.created_at));
    const unknown = unknownReports.map((r) => ({ run_id: r.run_id, schema_version: r.schema_version, model: null, dataset: null, evaluated_on: null, created_at: null, config: null, headline: null }))
      .sort((a, b) => (a.run_id < b.run_id ? -1 : 1));
    if (JSON.stringify(items) !== JSON.stringify([...known, ...unknown])) {
      fail('fixtures/public/listReports.json', 'ok case must summarise every report fixture (fields copied from the report, config included), known-major newest first, then unknown-major ones with null fields');
    }
  }
  // getReport cases must reference the report whose run_id is in the path.
  const gr = existsSync(join(FIXTURES, 'public', 'getReport.json')) ? readJson(join(FIXTURES, 'public', 'getReport.json')) : null;
  for (const c of gr?.cases ?? []) {
    const ref = c.response?.body?.$fixtureRef;
    if (!ref || !existsSync(join(FIXTURES, ref))) continue;
    const runId = decodeURIComponent(c.request.path.split('/').pop());
    if (readJson(join(FIXTURES, ref)).run_id !== runId) fail(`fixtures/public/getReport.json [${c.name}]`, `${ref} has a different run_id than ${runId}`);
  }
}

// ---- 5. result ----
for (const w of warnings) console.warn(`WARNING: ${w}`);
if (errors.length > 0) {
  console.error(`\nContract validation FAILED (${errors.length} problem${errors.length === 1 ? '' : 's'}):`);
  for (const e of errors) console.error(`  - ${e}`);
  process.exit(1);
}
console.log(`OK: ${publicCount} public + ${modelCount} model fixture files, ${checkedCases} cases, ${validReports.length} valid reports (+ unknown_major.json rejected as required).`);
