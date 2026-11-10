// MSW handlers built from the shared contract fixtures (contracts/fixtures/public/*.json).
// Each operation answers with its first success case, so the UI runs without recs-api, except
// getReport, which answers by run id (see reportHandler).
import { http, HttpResponse, type HttpHandler } from 'msw';
import { operations, type OperationId } from '../api/operations';

export interface FixtureCase {
  name: string;
  request: {
    method: string;
    path: string;
    query: Record<string, string>;
    headers: Record<string, string>;
    body?: unknown;
    rawBody?: string;
    rawBodyBytes?: number;
  };
  response: { status: number; headers: Record<string, string>; body?: unknown };
}

export interface Fixture {
  operationId: OperationId;
  cases: FixtureCase[];
}

const fixtureFiles = import.meta.glob<Fixture>('../../../contracts/fixtures/public/*.json', {
  eager: true,
  import: 'default',
});
const referencedFiles = import.meta.glob<unknown>('../../../contracts/fixtures/reports/*.json', {
  eager: true,
  import: 'default',
});

export const fixtures: Fixture[] = Object.values(fixtureFiles);

export function fixtureFor(operationId: OperationId): Fixture {
  const fixture = fixtures.find((f) => f.operationId === operationId);
  if (!fixture) throw new Error(`no fixture for ${operationId}`);
  return fixture;
}

/** Resolves `{"$fixtureRef": "reports/als.json"}` bodies. */
function resolveBody(body: unknown): unknown {
  if (body !== null && typeof body === 'object' && '$fixtureRef' in body) {
    const ref = `../../../contracts/fixtures/${String(body.$fixtureRef)}`;
    if (!(ref in referencedFiles)) throw new Error(`unknown $fixtureRef ${ref}`);
    return referencedFiles[ref];
  }
  return body;
}

/** Turns one fixture case into a Response (Set-Cookie is dropped: browsers forbid it here). */
export function responseFor(c: FixtureCase): Response {
  const headers = Object.fromEntries(
    Object.entries(c.response.headers).filter(([name]) => name.toLowerCase() !== 'set-cookie'),
  );
  const init = { status: c.response.status, headers };
  const body = resolveBody(c.response.body);
  if (body === undefined) return new HttpResponse(null, init);
  if (typeof body === 'string') return new HttpResponse(body, init);
  return HttpResponse.json(body as Record<string, unknown>, init);
}

const toMswPath = (template: string) => template.replace(/\{([^}]+)\}/g, ':$1');

export function handlerFor(operationId: OperationId, caseName?: string): HttpHandler {
  const { method, path } = operations[operationId];
  const fixture = fixtureFor(operationId);
  const chosen = caseName
    ? fixture.cases.find((c) => c.name === caseName)
    : fixture.cases.find((c) => c.response.status >= 200 && c.response.status < 300);
  if (!chosen) throw new Error(`no case ${caseName ?? '(success)'} for ${operationId}`);
  const register = { GET: http.get, PUT: http.put, POST: http.post, DELETE: http.delete }[method];
  return register(toMswPath(path), () => responseFor(chosen));
}

/**
 * GET /api/reports/{runId} answers by run id, like recs-api: a fixture case for exactly that path
 * wins, then any report fixture with that run_id, then the 404 case. Malformed ids get the 400 case.
 */
export function reportHandler(): HttpHandler {
  const fixture = fixtureFor('getReport');
  const caseFor = (name: string) => {
    const found = fixture.cases.find((c) => c.name === name);
    if (!found) throw new Error(`no getReport case ${name}`);
    return found;
  };
  const reportsById = new Map<string, unknown>();
  for (const report of Object.values(referencedFiles)) {
    if (report !== null && typeof report === 'object' && 'run_id' in report && typeof report.run_id === 'string') {
      reportsById.set(report.run_id, report);
    }
  }
  return http.get(toMswPath(operations.getReport.path), ({ params }) => {
    const runId = String(params.runId);
    if (!RUN_ID.test(runId)) return responseFor(caseFor('bad_run_id'));
    const exact = fixture.cases.find((c) => c.request.path === `/api/reports/${runId}` && c.response.status === 200);
    if (exact) return responseFor(exact);
    const report = reportsById.get(runId);
    if (report !== undefined) return HttpResponse.json(report as Record<string, unknown>);
    return responseFor(caseFor('not_found'));
  });
}

// Same pattern as contracts/public.yaml getReport runId.
const RUN_ID = /^[A-Za-z0-9_-]{1,100}$/;

export const handlers: HttpHandler[] = (Object.keys(operations) as OperationId[]).map((id) =>
  id === 'getReport' ? reportHandler() : handlerFor(id),
);
