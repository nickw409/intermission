# Long-running job queue

Run at most one ml-32m job at a time, release builds only.

| Job | Status | Started | Result |
|---|---|---|---|
| download ml-32m | done | 2026-11-10 | MD5 verified; 32,000,205 lines in ratings.csv (incl. header) |
| ml-32m ingest + cache (A1 `[ml32m]` test, release) | done | 2026-11-10 | exact counts 32,000,204 / 87,585 / 200,948 / 2,000,072 tags; parse 11.1 s, cache save 3.4 s, cache load 5.5 s, cache 574 MiB, peak RSS 2.42 GB |
| ml-32m item-KNN fit (D, release, 4 threads) | done | 2026-11-10 | 32.7 s fit, peak RSS 1.81 GB; 23,350 items with neighbour lists, 2,335,000 entries |
| ml-32m content fit (E, release, 4 threads) | done | 2026-11-10 | 1.86 s fit, +0.2 GB; 83,131 of 87,585 movies scoreable; ~1.3 ms scoring per user |
| ml-32m ALS fit (G, release, 4 threads, k=64, 15 iters) | done | 2026-11-10 | 159.7 s (machine load ~12), peak RSS 1.79 GiB; objective 2.58e8 → 1.21e8 monotone; parity 1.0 |
| libFuzzer recs-model, 4 harnesses × 900 s (1 CPU-hour), ASan+UBSan, fuzz preset from main | done | 2026-11-10 | 0 crashes; executions: post_bodies 1.95M, get_routes 3.00M, http_raw 2.59M, search_text 30.9M |
| Go native fuzzing, every recs-api JSON decoder × 15 min | done | 2026-11-10 | all PASS: FuzzParseSignup 19.9M execs, FuzzParseLogin 28.7M, FuzzParsePut 27.0M, FuzzDecodeStrict 32.1M (all model responses), FuzzParseFixture 59.1M |
