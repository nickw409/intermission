# Model artifact format (format_version 1)

FROZEN after Wave 0. Written by `recs-train train|retrain` (librecs `write_artifact`), read by
recs-model (librecs `load_artifact`). recs-train never talks to recs-model; this directory layout
is the whole interface between them.

```
data/artifacts/
  CURRENT                     # one line: the active version, e.g. "2026-11-09T21-14-03Z\n"
  2026-11-09T21-14-03Z/
    manifest.json             # written LAST; lists the sha256 of every other file
    catalog/                  # Catalog: id map, movie metadata, rating counts/means, popularity ranks
      ...                     # file names/encoding chosen by workstream C; covered by the manifest
    pop/  pop_count/  knn/  content/  mf/  als/   # one directory per model, written by Model::save
```

## manifest.json
```json
{
  "format_version": 1,
  "version": "2026-11-09T21-14-03Z",
  "created_at": "2026-11-09T21:14:03Z",
  "git_sha": "0288606",
  "dataset": "ml-32m+site",
  "models": ["pop", "knn", "als"],
  "default_model": "als",
  "config": { "pop": { "m": 10 }, "knn": { "...": "..." }, "als": { "...": "..." } },
  "files": { "catalog/movies.bin": "<sha256 hex>", "als/factors.bin": "<sha256 hex>", "...": "..." }
}
```

## Rules
1. **Atomic publish.** Write into `data/artifacts/.tmp-<version>-<pid>/`, fsync every file and the
   directory, then `rename()` to `data/artifacts/<version>/`. A crash leaves only a `.tmp-*` dir,
   which loaders ignore and the next run deletes.
2. **Manifest last.** `manifest.json` is written after every other file and lists the SHA-256 of
   every other file (paths relative to the artifact dir, `/`-separated, sorted).
3. **Verification before use.** `load_artifact` checks that `format_version` is known, every listed
   file exists with the right hash, and there are no unlisted files (other than `manifest.json`).
   Any failure → `ArtifactError`; recs-model keeps serving the previous artifact.
4. **CURRENT pointer.** Updated only after the artifact dir is complete: write `CURRENT.tmp-<pid>`,
   fsync, `rename()` over `CURRENT`. recs-model polls `CURRENT` (and/or uses inotify), loads and
   verifies the new version off the request path, then swaps an `std::shared_ptr` atomically.
5. **Binary files** use little-endian fixed-width integers and IEEE-754 float32/float64, with a
   small header (magic, version, counts) per file. No pointers, no padding-dependent structs.
6. **Retention.** recs-train keeps the 3 most recent artifact versions plus whatever CURRENT points to.
7. **No user identities.** Artifacts never contain usernames or recs-api user IDs. Website users
   appear only as anonymous training rows (and trained models discard per-user factors for them
   on save unless needed for parity checks — they are not needed at serving time).
