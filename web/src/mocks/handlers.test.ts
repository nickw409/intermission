import { describe, expect, it } from 'vitest';

async function getJson(path: string) {
  const response = await fetch(new URL(path, globalThis.location.href));
  return { status: response.status, body: (await response.json()) as Record<string, unknown> };
}

describe('mock getReport', () => {
  it.each([
    ['2026-11-09T21-14-03Z_als', 'als'],
    ['2026-11-09T21-00-00Z_pop', 'pop'],
    ['2026-11-09T21-10-00Z_knn', 'knn'], // listed, with no case of its own
    ['2026-11-09T22-02-10Z_als_k128', 'als'],
  ])('answers %s with that run report', async (runId, model) => {
    const { status, body } = await getJson(`/api/reports/${runId}`);
    expect(status).toBe(200);
    expect(body.run_id).toBe(runId);
    expect(body.model).toBe(model);
  });

  it('answers an unknown major version with that report as-is', async () => {
    const { status, body } = await getJson('/api/reports/2026-12-01T00-00-00Z_pop_v2');
    expect(status).toBe(200);
    expect(body.schema_version).toBe(2);
  });

  it('answers 404 not_found for a run that does not exist', async () => {
    const { status, body } = await getJson('/api/reports/2026-01-01T00-00-00Z_nope');
    expect(status).toBe(404);
    expect(body).toEqual({ error: { code: 'not_found', message: 'report not found' } });
  });

  it('answers 400 for a malformed run id', async () => {
    const { status } = await getJson('/api/reports/bad%20id');
    expect(status).toBe(400);
  });
});
