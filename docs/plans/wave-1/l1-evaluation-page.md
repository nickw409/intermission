# L1 — Evaluation page (charts from run reports)

**Owns:** `web/src/pages/evaluation/`, `web/src/lib/reports/`
**Contract:** `contracts/report.schema.json` (generated `web/src/api/report.d.ts`), the
`listReports` / `getReport` operations in `contracts/public.yaml`, and the fixtures in
`contracts/fixtures/reports/` (illustrative numbers).
**Spec:** "Frontend → Evaluation page charts" and rules; Phase 2 gate "The Evaluation page renders
every chart from fixture reports and rejects an unknown schema major version."

## Gate tests (write first; Vitest + Testing Library + MSW serving the report fixtures)
- `parseReport(json)` in `web/src/lib/reports/`: accepts schema_version 1; for any other
  `schema_version` (e.g. `unknown_major.json`) throws a typed `UnsupportedReportVersion` and the
  page shows "This report uses a newer format (v2) that this site can't display" instead of
  guessing. Also rejects structurally invalid JSON with a clear error.
- Each chart renders from the fixtures (assert on SVG presence + accessible title/description) AND
  has a data-table fallback (a `<table>` with the same numbers, toggled by a "Show data" control
  and always present for screen readers):
  1. NDCG@10 by model: point + 95% CI whisker, popularity baseline as a reference line.
  2. Recall@k curves (k = 1–50), one line per model.
  3. Coverage@10 vs NDCG@10 scatter (accuracy–diversity trade-off).
  4. Training curves: `training_curve` by iteration, labelled with `training_curve_metric`
     (models without a curve are omitted with a note).
  5. Cold-user slice vs all users, per model.
  6. Run picker: choose two runs of the same model (e.g. als vs als_k128) and compare their
     headline metrics and curves side by side.
- Model "failure" labelling: a model whose NDCG@10 CI is entirely below or overlapping
  popularity's is labelled "does not beat popularity" (spec: such a model is reported as a failure).
  Use the CI-overlap rule only (paired bootstrap results aren't in the public report).
- Loading and error states; empty state when `/api/reports` returns no items.
- Listing entries with an unknown `schema_version` (null model/headline) are shown as
  "unsupported report format" and excluded from charts. The run picker labels runs with their
  `config` (e.g. "als · k=128").

## Notes
- Charts: Observable Plot (pinned exact version) rendering to SVG; no canvas, no CDN.
- Colors must work in light and dark mode and be distinguishable for color-blind users
  (use a vetted categorical palette and also vary marker shape/line dash).
- One model per color consistently across all charts (pop, pop_count, knn, content, mf, als).
