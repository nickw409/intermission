# Wave 1 — task briefs

Wave 1 = Phase 0 core (A1, B1, C1) plus early Phase 2 foundations (I1, K1, L1), per the master plan
(`docs/plans/2026-11-09-parallel-build.md` §5). Each brief is the full context an agent gets.

## Protocol for every Wave 1 agent
1. You work in your own git worktree on branch `wave1/<task-id>`. Write ONLY inside your ownership
   paths (listed in your brief). Frozen files (`core/include/recs/*.hpp`, `contracts/**`, wiring
   files, `CMakeLists.txt` anywhere, `tests/support/`, `tests/fixtures/`) are read-only: if you need
   a change, stop and report it with the reason — do not work around it.
2. Tests first. Write the gate tests from your brief, run them, confirm they fail for the right
   reason (usually `NotImplemented`), then implement.
3. Never loosen a tolerance or threshold to make a test pass. Never change the evaluation protocol.
4. C++: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` must pass
   with zero warnings (warnings are errors). Use `OMP_NUM_THREADS=2` and
   `CMAKE_BUILD_PARALLEL_LEVEL=3` — other agents share this 8-core machine.
   Go: `make -C api check`. Web: `npm --prefix web run check`.
5. Each model/algorithm file starts with a comment block explaining the math, with references.
   Prefer clear code over clever code — this is a learning codebase.
6. Commit to your branch in small steps with clear messages. Never mention Claude/AI; no
   Co-authored-by trailers. Do not push, merge, or touch other branches — the orchestrator does that.
7. Final report: what you built, every gate test and its result (paste the test summary), anything
   surprising, anything you think is wrong in the frozen interfaces (with evidence), and any slow
   job that still needs running (e.g. an `[ml32m]` test).
