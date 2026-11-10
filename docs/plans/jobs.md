# Long-running job queue

Run at most one ml-32m job at a time, release builds only.

| Job | Status | Started | Result |
|---|---|---|---|
| download ml-32m | done | 2026-11-10 | MD5 verified; 32,000,205 lines in ratings.csv (incl. header) |
| ml-32m ingest + cache (A1 `[ml32m]` test, release) | done | 2026-11-10 | exact counts 32,000,204 / 87,585 / 200,948 / 2,000,072 tags; parse 11.1 s, cache save 3.4 s, cache load 5.5 s, cache 574 MiB, peak RSS 2.42 GB |
