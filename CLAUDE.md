# Shogi — MCTS engine

Standalone Shogi game in C++17: SDL3 UI, multi-threaded incremental Monte-Carlo
Tree Search AI. Human-vs-human, human-vs-computer, computer-vs-computer.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/shogi
```

SDL3 is found locally or fetched (pinned tarball + SHA-256) via `FetchContent`.

WebAssembly build: `web/build.sh` (needs the Emscripten SDK) builds two
variants into `build-web/` — multi-threaded (`-pthread`, CMake `SHOGI_PTHREAD`)
and single-threaded — and `web/index.html` picks one per browser. The
single-threaded path (`SHOGI_NO_THREADS`, Emscripten without pthreads) has no
worker threads: `Engine::pump()` runs the search inline each frame.

## Source layout

| File | Responsibility |
|------|----------------|
| `src/board.{hpp,cpp}` | Rules, move generation, evaluation, quiescence |
| `src/mcts.{hpp,cpp}`  | Thread-safe PUCT MCTS over a shared tree |
| `src/engine.{hpp,cpp}`| Worker-thread pool driving the search |
| `src/ui.{hpp,cpp}`    | SDL3 rendering, input, game flow |
| `src/glyphs.hpp`      | Embedded glyph atlas (ASCII + kanji), generated |
| `tools/genfont.cpp`   | Generator for `glyphs.hpp` (run offline, not built) |
| `test/`               | Correctness + strength harnesses (see below) |

Conventions: C++ headers are `.hpp`. Commit eagerly, one logical change per
commit. The `test/` programs are **not** wired into CMake — build them by hand
with the commands below.

## Engine architecture (one-paragraph orientation)

`Position` is a ~110-byte trivially-copyable POD. `generateLegalMoves` uses
pin detection so most moves skip the make-and-test. The static eval
(`evalScore`, centipawns) is material + piece-square tables + king safety,
game-phase scaled; `evalBlackWinProb` is its logistic. `evalLeaf` runs a
capture-only alpha-beta quiescence (`qsearch`) before the static eval — this
is what the MCTS scores leaves with (no random rollouts). MCTS uses PUCT with
heuristic move priors; `cpuct` is a tunable field (default 2.0). The tree is
reused across moves via `MCTS::advance` / `Engine::advance`.

**Hard invariant:** move generation must keep `perft(1..4) =
30 / 900 / 25470 / 719731`. Any change touching `board.cpp` move code is wrong
if perft drifts.

---

# Test & measurement harnesses

Four standalone programs under `test/`. They are how engine changes are
verified for correctness and for *strength* — read this before tuning the AI.

## `test/perft.cpp` — correctness gate

Move-generation regression + an MCTS smoke test. Run after **every** change to
`board.cpp`, `mcts.cpp`, or `engine.cpp`.

```sh
g++ -O2 -std=c++17 -Isrc -pthread \
    test/perft.cpp src/board.cpp src/mcts.cpp src/engine.cpp -o build/perft
./build/perft
```

Checks, in order:
- **perft(1..4)** from the initial position — must print exactly
  `30 / 900 / 25470 / 719731`. These are known-correct Shogi node counts;
  a mismatch means move generation (drops, promotion, pins, legality) broke.
- **MCTS smoke** — searches 1 s, expects > 100 playouts and prints the
  playout rate and best move. A healthy run is tens of thousands of playouts
  and a normal developing move.
- **Tree-reuse smoke** — calls `Engine::advance` with the best move and
  confirms the subtree's accumulated visits carried over (not restarted).

## `test/eval_test.cpp` — evaluation unit checks

Asserts properties of the static evaluation. Run after any change to the eval
or piece-square tables.

```sh
g++ -O2 -std=c++17 -Isrc test/eval_test.cpp src/board.cpp -o build/eval_test
./build/eval_test
```

Six assertions: the opening evaluates to exactly 0.5; an exposed king scores
worse than a home king; extra material raises the win probability; the eval is
**mirror-antisymmetric** (colour-swap + 180-degree board flip maps `x` to
`1-x`, which validates the `80 - s` PST mirroring); the king-safety penalty
fades to nothing in a bare-king endgame; enemy pressure near the king lowers
the score.

## `test/selfplay.cpp` — A/B strength match vs. the baseline

Plays the **current** engine against the **pre-improvement baseline** and
reports win rate, an Elo estimate, and early king-walk counts. This is the
primary "did the engine get stronger" measurement.

```sh
g++ -O2 -std=c++17 -Isrc -Itest -pthread \
    test/selfplay.cpp src/board.cpp src/mcts.cpp src/engine.cpp \
    test/baseline/board.cpp test/baseline/mcts.cpp test/baseline/engine.cpp \
    -o build/selfplay
./build/selfplay [games] [playout-budget-per-move]      # default: 30 2500
```

How it works:
- The baseline is commit `8a6430f` (material-only eval, random rollouts, plain
  UCT) snapshotted in `test/baseline/` with `namespace shogi` rewritten to
  `namespace shogibase`. Renaming the namespace lets both engines link into a
  single binary with no symbol clash; the two `Position` structs are
  layout-identical and copied field-by-field.
- Each ply, the side-to-move's engine is given a **fixed playout budget**
  (`Engine::visits()` is polled until it reaches the budget), then plays its
  `bestMove`. A fixed *playout* budget — not wall-clock — is deliberate: it
  isolates move *quality* from raw search *speed* (the current engine searches
  far more nodes/second, which a wall-clock match would conflate with quality).
- Engines use `setPosition` each move (no tree reuse) so the per-move budget
  stays clean.
- Colours alternate every game. Output: `new W - base L - draw D`, a score
  percentage, an approximate Elo delta (`-400*log10(1/score - 1)`), and how
  many games each side walked its king off the back two ranks before ply 40.

Reference result (30 games, budget 2500): new `26-4-0`, 86.7%, ~+325 Elo;
early king-walks new `0`, baseline `12`.

Regenerating the baseline (only if you ever need a different reference point):
```sh
for f in board mcts engine; do
  for e in hpp cpp; do
    git show <commit>:src/$f.$e | sed 's/namespace shogi/namespace shogibase/g' \
      > test/baseline/$f.$e
  done
done
```

## `test/tune.cpp` — CPUCT self-play tuning

Self-plays the **current** engine against itself with two different `cpuct`
values to tune the PUCT exploration constant.

```sh
g++ -O2 -std=c++17 -Isrc -pthread \
    test/tune.cpp src/board.cpp src/mcts.cpp src/engine.cpp -o build/tune
./build/tune [games] [playout-budget] [cpuctA] [cpuctB]   # default: 24 4000 3.0 2.0
```

Both players are the current engine (one binary, `cpuct` is a runtime field);
only the exploration constant differs.

**Critical detail — opening variety.** The engine is near-deterministic, so
self-play from the fixed start position just replays one game; an early run
drew 30/30 and measured nothing. `tune.cpp` therefore starts each *game pair*
from a short random opening (2-10 random legal plies), played twice with the
engines swapping colours. This produces varied, decisive games. Any future
self-play tuning harness must do the same.

Output: `A wins - B wins - draws` and `A score %`. Reference result (30 games,
budget 5000): `cpuct 2.0` beat `cpuct 3.0` `13-5` with 12 draws (~63%), which
is why the default is 2.0.

## Measurement notes / gotchas

- **Fixed playout budget vs. wall-clock.** Strength changes that improve move
  *quality* (eval, priors, quiescence) show up at a fixed playout budget.
  Changes that only improve *speed* (faster move gen) or retain prior work
  (tree reuse) do **not** — they need a fixed wall-clock budget and the
  `advance` path to be visible. Pick the budget type to match what you changed.
- Self-play between two near-identical configs needs randomized openings or it
  just draws (see `tune.cpp`).
- A/B runs are slow (the baseline searches ~14x slower per playout); 30 games
  at budget 2500 is roughly 30 minutes. Run them in the background.
- `waitBudget` in the harnesses caps the wait at 30 s wall-clock as a safety.

## License

Public domain — see `UNLICENSE`.
