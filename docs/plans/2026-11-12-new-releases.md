# New releases in the catalog — plan

Status: **approved by the owner (2026-11-12); spec amended (*Data* section).** Frozen-interface
changes still need orchestrator sign-off at merge (§9). Nothing here is built yet.

**Decisions so far (owner, 2026-11-12)**

| Question | Decision |
|---|---|
| Data source | **Wikidata approved** as the catalog supplement source, with `query.wikidata.org` as a batch-time host |
| Fetcher User-Agent contact | The public repo URL: `intermission-fetch/1.0 (+https://github.com/nickw409/intermission)` |
| Inclusion threshold | `min_sitelinks = 5`: about 290 films per six months, mostly wide releases |
| Window start | 2023-01-01. Films already in MovieLens are deduped (§3.2). |
| Scheduling | Build now behind the contracts (Pipelined mode) and merge after Phase 2 sign-off. Refresh runs from a manual weekly runbook until the Phase 5 retrain loop exists. |

## 1. Goal

The catalog is frozen at MovieLens 32M (released 05/2024; the newest titles are from 2023, and
only 952 of those are included). GroupLens has not published a newer dataset. This plan adds
movies released since then so users can find them, rate them and get them recommended.

**Non-goals and hard lines**

- **No web scraping** of IMDb, Letterboxd, Rotten Tomatoes, Metacritic or any other site. Their
  terms forbid it, the review text is copyrighted, reviews carry personal data, and this repo is
  public.
- **No TMDB data in the models or the catalog.** TMDB's API terms (last updated 2023-10-20)
  forbid using TMDB content to train, or to make datasets for training, an ML system (§1.C), and
  count training or validating on it as commercial use (§2.A). TMDB keeps its current job:
  posters, and later the display-only fields of the richer movie page (backlog §3).
- **The evaluation protocol is unchanged** (rule 3). Offline evaluation stays MovieLens-only.
- **The service boundaries are unchanged** (rule 1). recs-train still has no network access, and
  only a `tools/` script does the fetching.

## 2. Source: Wikidata (CC0)

Wikidata is public domain (CC0), so there are no licence conditions on training, storage or
redistribution. Coverage was measured live on 2026-11-12 with SPARQL against `query.wikidata.org`:

| Sample | Items | IMDb ID | TMDB ID | Genre (P136) | Director (P57) | Cast (P161) | ≥ 5 sitelinks |
|---|---|---|---|---|---|---|---|
| Films (P31 = Q11424) released 2026-05-01 to 2026-10-31 | 1,150 | 91% | 85% | 77% | 79% | 64% | 293 |
| 400 random ml-32m movies, joined by IMDb ID (seed 42) | 344 found (86%) | — | — | 79% of sample | 81% | 67% | — |

Coverage holds up both for new films and for the existing catalog. That matters because new movies
are only comparable to old ones if both are described by the same features (§5).

Rejected sources:

| Source | Why not |
|---|---|
| TMDB `/discover`, `/now_playing`, daily ID exports | ML-training restriction (§1). Fine for display, not for the catalog. |
| IMDb non-commercial TSVs | Personal and non-commercial use only, and IMDb forbids repurposing the data "to build a movie database". |
| OMDb | Derived from IMDb data, has a small quota, and needs a key. |
| Scraped reviews | See §1. |

## 3. Pipeline

```
tools/fetch_wikidata.sh  ──►  data/raw/wikidata/YYYY-MM-DD/{catalog_join,new_films}.jsonl + SHA256SUMS
  (host, weekly timer)                         │
                                               ▼
              recs-train ingest --dataset ml-32m --supplement data/raw/wikidata/YYYY-MM-DD
                 (no network; builds the catalog supplement and the Wikidata feature block)
                                               ▼
              recs-train train  ──►  artifact (catalog + content-wd model)  ──►  CURRENT swap
                                               ▼
              recs-model  ──►  recs-api  ──►  New releases page, For you (year filter), search
```

### 3.1 Fetcher (`tools/fetch_wikidata.sh`, plus a small Python script run with `python3 -I`)

- **Query A (join):** for every IMDb ID in `links.csv`, fetch the QID, P136 genres, P57
  directors, P161 cast, P495 country, P364 original language, P4947 TMDB ID and the sitelink
  count. Send `VALUES` batches of 500 IDs, sequentially, with at least 1 s between requests.
  That's about 175 requests for ml-32m.
- **Query B (new films):** films with an earliest P577 between 2023-01-01 and the fetch date (no
  future releases, which can't be rated yet) that have an IMDb ID or a TMDB ID and at least
  `min_sitelinks` (5) sitelinks. Fetch the same fields plus the English label. Split by month so each
  query stays well under the 60-second query limit.
- Each run writes into a new dated directory with a `SHA256SUMS` file. It never overwrites an
  earlier snapshot. Keep the last 8 snapshots. The directory is gitignored under `data/`.
- **User-Agent:** `intermission-fetch/1.0 (+https://github.com/nickw409/intermission)`, the
  descriptive User-Agent with contact details that Wikimedia's policy asks for. Fail on any
  non-200 response and retry 429/503 with backoff. Do not parallelise.
- No secrets are needed.

### 3.2 Catalog supplement (in `core/src/data/`)

- **IDs:** `movieId = 1,000,000,000 + QID number`. This is deterministic and stable across
  refreshes, so ratings in `app.db` stay valid without an ID registry. It is far above the
  MovieLens maximum of 292,757 and fits the int32 `MovieId` in `public.yaml` while
  QID ≤ 1,147,483,647. Ingest fails loudly on any QID beyond that.
- **Dedup:** if a Query B film's IMDb ID or TMDB ID is already in `links.csv`, skip it. The
  MovieLens entry wins.
- **Title** comes from the English label (films without one are skipped). **Year** comes from the
  earliest P577. **Release date** (new field) is the earliest P577 at day precision.
- **Genres:** a reviewed mapping file, `configs/wikidata-genres.json`, maps Wikidata genre QIDs
  to the 19 MovieLens genres (for example "superhero film" → Action, Adventure). Unmapped genres
  are dropped from the MovieLens genre list but kept as Wikidata features (§5). Films with no
  mapped genre are kept, with an empty genre list.
- **TMDB ID** comes from P4947, so the existing poster pipeline works without changes.
- Each item records its `source` (`movielens` or `wikidata`) and sitelink count.

## 4. Evaluation stays MovieLens-only

- `recs-train eval` never loads supplement items. Evaluation catalog = MovieLens catalog, so
  catalog coverage@10 and every other metric keep their current meaning.
- Supplement items have no ratings, so they could never be in a test set anyway. They are
  excluded so that a model can't spend top-10 slots on them during evaluation.
- The report records the Wikidata snapshot date and SHA-256 whenever Wikidata features are used.
  That is a new provenance field, so `schema_version` gets bumped. No metric definition changes.

## 5. Models

- **Content model, new config `content-wd`.** This config adds a Wikidata term block to the
  existing TF-IDF vector: `genre:Q…`, `director:Q…`, `cast:Q…`, `country:Q…` and `lang:Q…`
  terms. The same TF-IDF weighting applies (with `min_tag_count` as the df floor), the block is
  L2-normalised on its own, and a `wikidata_weight` scales it, as `genre_weight` already does for
  genres. The block is built for **every** movie that has Wikidata data, old and new, so a new
  film by a director a user loves scores well. If only new films had these features, they would
  overlap with old films on genres alone.
  - Evaluated under the frozen protocol on MovieLens items against today's `content`. Ship it
    only if NDCG@10 is not significantly worse (overlapping 95% CIs). Otherwise keep `content` and
    give new films the genre-only path.
  - Update the math header comment in `content.cpp` (rule 9).
- **Popularity, item-KNN, MF-SGD and ALS:** a supplement item has no ratings, so these models
  never place it in a top-k. This needs a gate test (§8). For ALS and MF, a zero or random factor
  could otherwise land mid-ranking.
- **Collaborative signal** for new films comes only from site users' ratings, once
  `merge_site_ratings` lands. It is currently a Phase 5 stub. Until then, new films are
  recommended by the content model only.

## 6. API and UI

- `model.yaml`: movie metadata gains `release_date` (nullable) and `source`. New
  `GET /v1/movies/new?since=&limit=` returns supplement and MovieLens items ordered by
  release date descending, then sitelinks descending, then movieId.
- `public.yaml`: new `GET /api/movies/new` (no auth, cacheable) and the same new fields on
  `Movie`.
- Web: a **New releases** page with a grid of new films, a rate-inline control and a "few ratings
  yet" badge. Users can already filter For you to recent years with `year_from` (shipped in #63).
- Footer and movie page add a line saying movie metadata for newer titles comes from Wikidata.
  CC0 doesn't require it, but it's honest. The GroupLens and TMDB credits stay as they are.

## 7. Refresh cadence

- **Weekly:** fetch, then ingest, then retrain, then swap `CURRENT`. Before the Phase 5 retrain
  loop exists, this is a manual runbook (`deploy/runbooks/refresh-new-releases.md`). After Phase
  5, it becomes one more step in the nightly or weekly timer (`nightly-jobs.md`).
- If a fetch fails or Wikidata is down, keep serving the last artifact. Nothing at runtime
  depends on Wikidata.

## 8. Gates (each written as a failing test first)

1. **Eval protocol unchanged:** for every existing config, the eval report produced with a
   supplement on disk equals the one produced without it, ignoring timestamps and the provenance
   field.
2. **Stable IDs:** the same snapshot ingested twice gives an identical catalog and artifact bytes.
   A QID-ordered fixture maps to the expected movieIds, and a QID above 1,147,483,647 fails
   ingest.
3. **Dedup:** a fixture film whose IMDb ID is in `links.csv` does not appear twice.
4. **Cold items stay out of CF models:** pop, item-KNN, MF-SGD and ALS never return a zero-rating
   supplement item in top-k, tested on the synthetic dataset plus supplement.
5. **Content cold start:** a synthetic user who rated five films by director D gets an unrated
   supplement film by D in the content-wd top 10.
6. **Fetcher:** a recorded-response fixture (no network in CI) parses into the expected JSONL.
   Malformed rows fail with the row number.
7. **Contracts:** fixtures for the new endpoints validate (`tools/contracts`).
8. **Rating new films:** recs-api accepts a rating for a supplement movieId present in the
   loaded catalog and rejects an unknown one.

## 9. Owner decisions

All owner decisions are settled (see the top of this plan), and the spec's *Data* section is
amended.

These need owner or orchestrator sign-off before merge (rules 1, 5, 8):

- `core/include/recs/types.hpp` (`MovieMeta.release_date`, `source`, `sitelinks`) and
  `dataset.hpp` (supplement loader)
- The artifact format (`contracts/artifact.md`, version bump), `report.schema.json`
  (`schema_version` bump), `model.yaml`, `public.yaml`
- The spec's *Data* section, plus a delta `security-auditor` pass on the fetcher and the new
  endpoints

## 10. Later and optional

- **Wikipedia plot summaries (CC BY-SA 4.0)** as a text TF-IDF block. They give the richest cold
  features, but you have to fetch and parse wikitext, and showing excerpts would require
  attribution and share-alike.
- **Wikidata P444 review scores** (Rotten Tomatoes and Metacritic numbers, where editors entered
  them) as a weak quality prior on New releases. Coverage is patchy.
- **A future MovieLens release** that includes supplement films would need a one-off remap of
  `app.db` ratings from `1e9 + QID` to the new MovieLens ID, using the IMDb ID join.
