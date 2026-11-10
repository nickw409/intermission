// MSW handlers built from the shared contract fixtures (contracts/fixtures/public/*.json).
// Each operation answers with its first success case, so the UI runs without recs-api.
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

export const handlers: HttpHandler[] = (Object.keys(operations) as OperationId[]).map((id) => handlerFor(id));
