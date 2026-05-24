# Shogi — MCTS engine

Standalone Shogi game in C++17: SDL3 UI, multi-threaded incremental Monte-Carlo
Tree Search AI. Human-vs-human, human-vs-computer, computer-vs-computer.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j                 # produces the binary + build/tutorial.html
./build/shogi                          # Linux / Windows
open build/shogi.app                   # macOS (the binary lives at
                                       # build/shogi.app/Contents/MacOS/shogi)
```

On macOS the target is built as `Shogi.app` (a proper bundle with `src/shogi.icns`
in Resources and a generated Info.plist) so it carries its own Dock / Cmd-Tab
icon and version metadata rather than appearing as a generic terminal binary.

SDL3 is found locally or fetched (pinned tarball + SHA-256) via `FetchContent`.

A `cmake --build <build> --target package` step bundles the executable and
`tutorial.html` into a flat ZIP via CPack: `shogi-1.0-win64.zip` for the
mingw cross-build, `shogi-1.0-Darwin.zip` on a macOS native build, and so
on. Skipped automatically for Emscripten (which has its own assembly path).

WebAssembly build: `web/build.sh` (needs the Emscripten SDK) builds two
variants into `build-web/` — multi-threaded (`-pthread`, CMake `SHOGI_PTHREAD`)
and single-threaded — and `web/index.html` picks one per browser. The
script also copies `tutorial.html` next to the wasm artefacts; the in-page
shell shows a "Beginner's guide to shogi" link just below the canvas. The
single-threaded path (`SHOGI_NO_THREADS`, Emscripten without pthreads) has no
worker threads: `Engine::pump()` runs the search inline each frame.

## Source layout

| File | Responsibility |
|------|----------------|
| `src/board.{hpp,cpp}` | Rules, move generation, evaluation, quiescence |
| `src/mcts.{hpp,cpp}`  | Thread-safe PUCT MCTS over a shared tree |
| `src/mate.{hpp,cpp}`  | df-pn proof-number checkmate (tsume) search |
| `src/engine.{hpp,cpp}`| Worker-thread pool driving the search |
| `src/ui.cpp`          | SDL3 rendering, input, game flow |
| `src/glyphs.hpp`      | Embedded glyph atlas (ASCII + kanji), generated |
| `tools/genfont.cpp`   | Generator for `glyphs.hpp` (run offline, not built) |
| `tools/genicon.cpp`   | Generator for `src/shogi.{ico,icns}` + `src/icon.hpp` (offline, takes a TTF) |
| `tools/gentut.cpp`    | Generator for `docs/tutorial/img/*.png` (offline) |
| `tools/genhtml.cpp`   | Generator for `build/tutorial.html` (built by CMake; output not committed) |
| `test/`               | Correctness + strength harnesses (see below) |

Conventions: C++ headers are `.hpp`. Commit eagerly, one logical change per
commit. The `test/` programs are **not** wired into CMake — build them by hand
with the commands below.

`genfont`, `genicon`, and `gentut` are offline one-shot generators - not
wired into CMake, run by hand whenever their committed output
(`glyphs.hpp`, `shogi.ico`, `icon.hpp`, `docs/tutorial/img/*.png`) needs
to change:

```sh
c++ -O2 -std=c++17 -Isrc -Itools tools/gentut.cpp -o build/gentut
./build/gentut       # writes docs/tutorial/img/*.png from the repo root
```

`genhtml` is the exception: it's a regular CMake target. `cmake --build`
compiles `tools/genhtml.cpp`, runs it against `docs/tutorial/tutorial.md`,
and writes `build/tutorial.html` (a single-file HTML with all the PNGs
base64-embedded). The HTML is a build artefact, picked up from `build/` by
downstream packaging (CPack, `web/build.sh`, etc.) rather than committed.
Opt out with `-DSHOGI_BUILD_DOCS=OFF`. Cross-builds (mingw, Emscripten)
need a separate host C++ compiler; CMake auto-detects `c++` / `g++` /
`clang++` on the host, or you can pass `-DSHOGI_HOST_CXX=/path/to/cxx`.

`tools/stb_image_write.h` is the single-header public-domain PNG encoder
vendored for `gentut.cpp`, parallel to how `genfont.cpp` uses the vendored
`tools/stb_truetype.h`. `genhtml.cpp` has no third-party deps: it parses a
narrow markdown subset (ATX headings, `**bold**`, `_italic_`, `` `code` ``,
`[]()` links, `![]()` images, `-`/`N.` lists, GFM pipe tables, `---` rules)
and base64-encodes images inline. Tutorial.md is written to that subset, so
the parser stays small.

## Engine architecture (one-paragraph orientation)

`Position` is a ~110-byte trivially-copyable POD. `generateLegalMoves` uses
pin detection so most moves skip the make-and-test. The static eval
(`evalScore`, centipawns) is material + piece-square tables + king safety,
game-phase scaled; `evalBlackWinProb` is its logistic. `evalLeaf` runs a
capture-only alpha-beta quiescence (`qsearch`) before the static eval — this
is what the MCTS scores leaves with (no random rollouts). MCTS uses PUCT with
heuristic move priors; `cpuct` is a tunable field (default 2.0). Children are
materialised lazily — PUCT ranges over both the realised children and the
not-yet-realised moves, so the search never spends an evaluation on a move it
would not explore. At root pick (`bestChild`), when the top two children's
visit counts are within `closeRatio` (0.50) of each other, the tiebreak goes
to the higher STM-relative win prob when that gap exceeds `valueMargin`
(0.01) — fires ~1.5 times per move at budget 4000 and gains ~+30 Elo over
the visit-only AlphaZero default. An MCTS-Solver propagates proven
win/loss/draw values up the tree, with mate distance so it plays the
shortest forced win, and stops
the search once the root is proven. After each (re)root one worker also runs
a bounded df-pn proof-number search for a forced mate on the root, catching
mates the sampling search can miss. Sennichite (fourfold repetition) and
perpetual check are detected from the game history passed to `setPosition` /
`advance`. The tree is reused across moves via `MCTS::advance` /
`Engine::advance`.

**Hard invariant:** move generation must keep `perft(1..4) =
30 / 900 / 25470 / 719731`. Any change touching `board.cpp` move code is wrong
if perft drifts.

---

# Test & measurement harnesses

Eight standalone programs under `test/`. They are how engine changes are
verified for correctness and for *strength* — read this before tuning the AI.

## `test/perft.cpp` — correctness gate

Move-generation regression + an MCTS smoke test. Run after **every** change to
`board.cpp`, `mcts.cpp`, or `engine.cpp`.

```sh
g++ -O2 -std=c++17 -Isrc -pthread \
    test/perft.cpp src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp -o build/perft
./build/perft
```

Checks, in order:
- **perft(1..4)** from the initial position — must print exactly
  `30 / 900 / 25470 / 719731`. These are known-correct Shogi node counts;
  a mismatch means move generation (drops, promotion, pins, legality) broke.
- **MCTS smoke** — searches 1 s, expects > 100 playouts and prints the
  playout rate and best move. A healthy run is hundreds of thousands of
  playouts and a normal developing move.
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

## `test/tactics.cpp` — solver / tactical checks

Asserts the search proves forced wins (an exact `1.0` / `0.0` win probability,
which an un-proven evaluation never reaches), scores a fourfold repetition as
a draw, and a perpetual check as a loss for the checking side. Run after any
change to the MCTS-Solver or repetition handling.

```sh
g++ -O2 -std=c++17 -Isrc -pthread \
    test/tactics.cpp src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp -o build/tactics
./build/tactics
```

## `test/mate_test.cpp` — df-pn mate-solver differential test

Cross-checks `dfpnMate` (the proof-number search) against a brute-force
AND/OR reference over thousands of random self-play positions: df-pn must
never claim a mate the reference cannot confirm, nor miss a shallow one.
Run after any change to `mate.cpp`.

```sh
g++ -O2 -std=c++17 -Isrc \
    test/mate_test.cpp src/board.cpp src/mate.cpp -o build/mate_test
./build/mate_test
```

## `test/selfplay.cpp` — A/B strength match vs. the baseline

Plays the **current** engine against the **pre-improvement baseline** and
reports win rate, an Elo estimate, and early king-walk counts. This is the
primary "did the engine get stronger" measurement.

```sh
g++ -O2 -std=c++17 -Isrc -Itest -pthread \
    test/selfplay.cpp src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp \
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
  percentage, an approximate Elo delta (`-400*log10(1/score - 1)`), how
  many games each side walked its king off the back two ranks before ply 40,
  and per-result-class game length (mean + median ply when the game ended).

Reference result (30 games, budget 2500): new `30-0-0`, 100% — the search
rework beats this baseline every game, so `8a6430f` now only catches gross
regressions. Use `abprev.cpp` (below) to measure an incremental change. Early
king-walks new `0`, baseline `14`.

Regenerating the baseline (only if you ever need a different reference point):
```sh
for f in board mcts engine; do
  for e in hpp cpp; do
    git show <commit>:src/$f.$e | sed 's/namespace shogi/namespace shogibase/g' \
      > test/baseline/$f.$e
  done
done
```

## `test/abprev.cpp` — A/B vs. a recent snapshot

Like `selfplay.cpp`, but plays against a snapshot of an earlier build under
`test/prev/` (namespace `shogiprev`) instead of the fixed `8a6430f` baseline,
with `tune.cpp`-style random openings. Since the engine now beats `8a6430f`
every game, this is the tool for measuring an *incremental* change. Reports
the same Elo + per-result-class game-length output as `selfplay.cpp`, plus
a `bestChildOverride` counter (how often the value-tiebreak in `bestChild`
flipped the choice vs. visits-only — useful when sweeping the tiebreak's
`closeRatio` and `valueMargin` knobs). Refresh `test/prev/` to the
pre-change commit first:

```sh
for f in board mate mcts engine; do for e in hpp cpp; do \
  git show <commit>:src/$f.$e \
    | sed 's/namespace shogi/namespace shogiprev/g' > test/prev/$f.$e; done; done
g++ -O2 -std=c++17 -Isrc -Itest -pthread test/abprev.cpp \
    src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp \
    test/prev/board.cpp test/prev/mate.cpp test/prev/mcts.cpp \
    test/prev/engine.cpp -o build/abprev
./build/abprev [games] [playout-budget]
```

## `test/tune.cpp` — CPUCT self-play tuning

Self-plays the **current** engine against itself with two different `cpuct`
values to tune the PUCT exploration constant.

```sh
g++ -O2 -std=c++17 -Isrc -pthread \
    test/tune.cpp src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp -o build/tune
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

Output: `A wins - B wins - draws` and `A score %`. Reference result (50 games,
budget 4000): `cpuct` 1.0 / 1.5 / 2.0 are within noise of one another and 2.0
beats 3.0 (62%), so the default stays 2.0.

## `test/tune_eval.cpp` — self-play evaluation tuning

Fits the eval's 13 piece values (`evalPieceBase` / `evalPiecePromo`) to the
engine's own play.  It plays self-play games, labels each quiet position with
the *search* win probability for it — the game outcome is too noisy a target
in equal-vs-equal self-play and degenerates the fit — then coordinate-descends
the values to minimise the prediction error.  Prints the tuned values to copy
into `board.cpp`; A/B the result with `abprev`.

```sh
g++ -O2 -std=c++17 -Isrc -pthread test/tune_eval.cpp \
    src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp -o build/tune_eval
./build/tune_eval [games] [playout-budget]
```

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
- `waitBudget` in the harnesses caps the wait at 30 s wall-clock, and also
  returns early once the visit count plateaus — a position the solver has
  settled, where searching further is pointless.

## Tuning history (tried, dropped — don't repeat without a new angle)

Engine knobs that were investigated and rejected, with the result.  If you
have a fundamentally different idea for the same target, by all means
retest — but a straight reapplication of any of these will reproduce the
regression.

- **Decisive-move bonus in `movePrior`** (when STM is winning, multiply
  capture + check priors by ~1.5–1.8×).  At a 0.75 win-prob threshold the
  bonus fired ~580 times per move and cost ≈22 Elo + made cur-wins ~13 plies
  *longer* in 200-game `abprev` runs at budget 4000.  Tightening to a 0.90
  threshold barely changed the fire rate and left Elo near-neutral with
  game length still slightly worse.  Hypothesis: it fights MCTS's natural
  consolidate-then-attack pattern.

- **Asymmetric dynamic `cpuct`** (shrink `cpuct` toward `cpuct * (1 -
  cpuctShrinkMax)` as the parent's STM win-prob approaches 1.0, full
  `cpuct` when losing).  At `cpuctK=2, cpuctShrinkMax=0.5` it cost ≈14 Elo
  on top of the value-tiebreak at 400 games; at `cpuctK=4,
  cpuctShrinkMax=0.25` (very gentle) it landed within noise.  Less
  exploration in winning positions consistently lost more than it gained,
  even with the asymmetric guard.

## License

Public domain — see `UNLICENSE`.
