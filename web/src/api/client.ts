// Typed fetch wrapper for the public API (contracts/public.yaml).
//
//   const me = await api.get('/api/me');
//   const movie = await api.get('/api/movies/{movieId}', { path: { movieId: 260 } });
//   await api.put('/api/ratings/{movieId}', { path: { movieId: 260 }, body: { rating: 4.5 } });
//
// Paths, parameters, request bodies and response bodies are all checked against the generated
// `paths` type, so a contract change that breaks a caller is a type error.
// Every request is same-origin and carries `X-Requested-With: recs` (CSRF rule in public.yaml).
import type { components, paths } from './schema';

export type { IntermissionRunReport as RunReport } from './report';

export type Schemas = components['schemas'];
export type ErrorCode = Schemas['ErrorCode'];

// Record<ErrorCode, true> forces this list to match the contract's enum exactly.
const ERROR_CODES: Record<ErrorCode, true> = {
  bad_request: true,
  invalid_json: true,
  body_too_large: true,
  unauthorized: true,
  invalid_credentials: true,
  csrf_rejected: true,
  invalid_invite: true,
  username_taken: true,
  too_many_ratings: true,
  not_found: true,
  unknown_model: true,
  rate_limited: true,
  model_unavailable: true,
  internal: true,
};

function isErrorCode(value: unknown): value is ErrorCode {
  return typeof value === 'string' && Object.hasOwn(ERROR_CODES, value);
}

/** A non-2xx response. Bodies that are not the contract's Error shape become code `internal`. */
export class ApiError extends Error {
  readonly status: number;
  readonly code: ErrorCode;

  constructor(status: number, code: ErrorCode, message: string) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
  }
}

type Method = 'get' | 'put' | 'post' | 'delete';
type Operation<M extends Method, P extends keyof paths> = NonNullable<paths[P][M]>;

/** Paths that support method M. */
export type PathsFor<M extends Method> = {
  [P in keyof paths]: [NonNullable<paths[P][M]>] extends [never] ? never : P;
}[keyof paths];

type PathParams<Op> = Op extends { parameters: { path: infer T } } ? { path: T } : unknown;
type QueryParams<Op> = Op extends { parameters: { query: infer T } }
  ? { query: T }
  : Op extends { parameters: { query?: infer T } }
    ? [T] extends [undefined] ? unknown : { query?: T }
    : unknown;
type RequestBody<Op> = Op extends { requestBody: { content: { 'application/json': infer T } } }
  ? { body: T }
  : unknown;

export type Options<Op> = PathParams<Op> & QueryParams<Op> & RequestBody<Op> & { signal?: AbortSignal };

type JsonOf<R> = R extends { content: { 'application/json': infer T } } ? T : undefined;
/** The body of the operation's 2xx response(s); `undefined` for 204. */
export type SuccessBody<Op> = Op extends { responses: infer R }
  ? { [S in keyof R & (200 | 201 | 204)]: JsonOf<R[S]> }[keyof R & (200 | 201 | 204)]
  : never;

// Options may be omitted when nothing in them is required.
type Args<Op> = object extends Options<Op> ? [options?: Options<Op>] : [options: Options<Op>];

interface RawOptions {
  path?: Record<string, string | number>;
  query?: Record<string, string | number | boolean | undefined>;
  body?: unknown;
  signal?: AbortSignal;
}

function buildUrl(template: string, options: RawOptions): string {
  const path = template.replace(/\{([^}]+)\}/g, (_, name: string) => {
    const value = options.path?.[name];
    if (value === undefined) throw new Error(`missing path parameter ${name} for ${template}`);
    return encodeURIComponent(String(value));
  });
  const query = new URLSearchParams();
  for (const [key, value] of Object.entries(options.query ?? {})) {
    if (value !== undefined) query.set(key, String(value));
  }
  const qs = query.toString();
  // Absolute URL so the same code works in the browser and under Node (tests).
  return new URL(qs ? `${path}?${qs}` : path, globalThis.location.href).toString();
}

async function toApiError(response: Response): Promise<ApiError> {
  let body: unknown;
  try {
    body = await response.json();
  } catch {
    body = undefined;
  }
  const error = (body as { error?: { code?: unknown; message?: unknown } } | undefined)?.error;
  if (isErrorCode(error?.code)) {
    return new ApiError(response.status, error.code, typeof error.message === 'string' ? error.message : '');
  }
  return new ApiError(response.status, 'internal', `unexpected ${response.status} response`);
}

async function request(method: Method, template: string, options: RawOptions = {}): Promise<unknown> {
  const headers: Record<string, string> = { 'X-Requested-With': 'recs', Accept: 'application/json' };
  const init: RequestInit = { method: method.toUpperCase(), credentials: 'same-origin', headers };
  if (options.signal) init.signal = options.signal;
  if (options.body !== undefined) {
    headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(options.body);
  }
  const response = await fetch(buildUrl(template, options), init);
  if (!response.ok) throw await toApiError(response);
  if (response.status === 204) return undefined;
  return response.json();
}

function method<M extends Method>(m: M) {
  return <P extends PathsFor<M>>(path: P, ...[options]: Args<Operation<M, P>>): Promise<SuccessBody<Operation<M, P>>> =>
    request(m, path, options as RawOptions) as Promise<SuccessBody<Operation<M, P>>>;
}

export const api = {
  get: method('get'),
  put: method('put'),
  post: method('post'),
  delete: method('delete'),
};
