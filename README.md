# Shogi

A standalone Shogi (Japanese chess) game in C++ with an SDL3 interface and a
multi-threaded, incremental Monte-Carlo Tree Search (MCTS) AI.

## Features

- **Three play modes** — human vs human (hotseat), human vs computer (either
  side), and computer vs computer.
- **Multi-threaded MCTS** — one shared search tree grown by a pool of worker
  threads, using virtual loss to keep concurrent threads on distinct paths and
  a UCT rule that balances exploitation against exploration.
- **Incremental search** — the engine searches *continuously*, including while
  a human is thinking ("pondering"). That same ongoing search powers:
  - **Move suggestions** — the engine's current best move is outlined on the
    board (toggle with the `SUGGEST` button or the `S` key).
  - **Position-strength bars** — vertical bars on each side of the board fill
    in proportion to that side's estimated winning chances, updating live as
    the search deepens.
- **Compact state representation** — a `Position` is a small, trivially
  copyable POD (an 81-byte board, hand counts, side to move, Zobrist hash) so
  MCTS can clone nodes with a single cheap copy.
- **Full Shogi rules** — drops, promotion (optional and forced), two-pawn
  (*nifu*) and drop-mate (*uchifuzume*) restrictions, checkmate detection, and
  fourfold-repetition (*sennichite*) draws.
- **Multi-platform** — SDL3 for window, rendering and input; an embedded
  bitmap font means no asset files are needed at runtime.

## Building

Requires CMake (>= 3.16) and a C++17 compiler. SDL3 is used if installed;
otherwise it is fetched and built automatically (needs network access on the
first configure).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/shogi
```

## Playing

- Pick a mode on the start screen.
- **Move a piece:** click it, then click a destination. Legal destinations are
  shown as dots. If a move may optionally promote, a dialog asks; forced
  promotions happen automatically.
- **Drop a piece:** click one of your captured pieces in the side panel, then
  click an empty square.
- `MENU` returns to mode selection; `Esc` does the same (or quits from the
  menu).

## Layout of the code

| File | Responsibility |
|------|----------------|
| `board.{h,cpp}` | State representation, move generation, rules, evaluation |
| `mcts.{h,cpp}`  | Thread-safe incremental MCTS over a shared tree |
| `engine.{h,cpp}`| Worker-thread pool driving the search |
| `ui.{h,cpp}`    | SDL3 rendering, input and game flow |
| `font.h`        | Embedded 5x7 bitmap font |
| `main.cpp`      | Entry point |

## Notes / limitations

- The AI uses MCTS with capped, capture-biased random rollouts finished by a
  material evaluation. It plays a reasonable amateur game; it is not a strong
  engine. Search strength scales with core count and the per-move time budget
  (`THINK_MS` in `ui.cpp`).
- Repetition is scored as a simple fourfold draw; the perpetual-check
  exception (where the checking side loses) is not implemented.
