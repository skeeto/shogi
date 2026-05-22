// selfplay.cpp - A/B strength match: the current engine vs the pre-improvement
// baseline (commit 8a6430f, snapshotted under namespace `shogibase`).  Also
// reports early king-walks per side, the behaviour this work set out to fix.
//
// Build:
//   g++ -O2 -std=c++17 -Isrc -Itest -pthread test/selfplay.cpp \
//       src/board.cpp src/mcts.cpp src/engine.cpp \
//       test/baseline/board.cpp test/baseline/mcts.cpp test/baseline/engine.cpp \
//       -o build/selfplay
//
// Run:  ./build/selfplay [games] [playout-budget-per-move]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "engine.hpp"            // current engine, namespace shogi
#include "baseline/engine.hpp"   // baseline engine, namespace shogibase

namespace S = shogi;
namespace B = shogibase;

// The two Position structs are layout-identical; copy field by field.
static B::Position toBase(const S::Position& p) {
  B::Position b;
  for (int i = 0; i < S::N_SQ; ++i) b.board[i] = p.board[i];
  for (int c = 0; c < 2; ++c)
    for (int t = 0; t < S::HAND_TYPES; ++t) b.hand[c][t] = p.hand[c][t];
  b.side = p.side;
  b.ply  = p.ply;
  b.hash = p.hash;
  return b;
}

// Spin until the engine has searched `budget` playouts (with a wall-clock cap).
template <class Eng>
static void waitBudget(Eng& e, int budget) {
  auto start = std::chrono::steady_clock::now();
  int last = -1, stalls = 0;
  while (e.visits() < budget) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    int v = e.visits();
    if (v == last) {                        // no progress: the position solved
      if (++stalls > 25) break;
    } else { last = v; stalls = 0; }
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30))
      break;
  }
}

int main(int argc, char** argv) {
  int games  = (argc > 1) ? std::atoi(argv[1]) : 30;
  int budget = (argc > 2) ? std::atoi(argv[2]) : 2500;

  S::initZobrist();
  B::initZobrist();
  S::Engine newEng;
  B::Engine baseEng;

  int newWins = 0, baseWins = 0, draws = 0;
  int newKingOut = 0, baseKingOut = 0;   // games with an early (<ply 40) king walk

  for (int g = 0; g < games; ++g) {
    bool newIsBlack = (g % 2 == 0);
    S::Position pos = S::initialPosition();
    std::vector<uint64_t> hist{pos.hash};
    int result = -1;                     // 0 Black wins, 1 White wins, 2 draw
    bool newOut = false, baseOut = false;

    for (int ply = 0; ply < 400; ++ply) {
      std::vector<S::Move> legal;
      S::generateLegalMoves(pos, legal);
      if (legal.empty()) { result = (pos.stm() == S::BLACK) ? 1 : 0; break; }

      bool newTurn = (pos.stm() == S::BLACK) == newIsBlack;
      S::Move mv;
      if (newTurn) {
        newEng.setPosition(pos, hist);
        waitBudget(newEng, budget);
        mv = newEng.stats().bestMove;
        newEng.stop();
      } else {
        baseEng.setPosition(toBase(pos));
        waitBudget(baseEng, budget);
        B::Move bm = baseEng.stats().bestMove;
        baseEng.stop();
        mv.from = bm.from; mv.to = bm.to; mv.promo = bm.promo; mv.drop = bm.drop;
      }
      if (mv.isNull()) mv = legal[0];
      S::doMove(pos, mv);
      hist.push_back(pos.hash);

      // King-walk watch: a king off its home two ranks before ply 40.
      for (int s = 0; s < S::N_SQ; ++s) {
        S::Piece q = pos.board[s];
        if (!q || S::typeOf(q) != S::PT_KING) continue;
        int row = S::rowOf(s);
        bool home = (S::colorOf(q) == S::BLACK) ? (row >= 7) : (row <= 1);
        if (!home && pos.ply < 40) {
          if (((S::colorOf(q) == S::BLACK) == newIsBlack)) newOut = true;
          else                                             baseOut = true;
        }
      }
      int reps = 0;
      for (uint64_t h : hist) if (h == pos.hash) ++reps;
      if (reps >= 4) { result = 2; break; }     // fourfold repetition
    }
    if (result < 0) result = 2;                 // ply cap -> draw

    const char* tag;
    if (result == 2) { ++draws; tag = "draw"; }
    else {
      bool newWon = ((result == 0) == newIsBlack);
      if (newWon) { ++newWins;  tag = "new";  }
      else        { ++baseWins; tag = "base"; }
    }
    if (newOut)  ++newKingOut;
    if (baseOut) ++baseKingOut;
    std::printf("game %2d/%d  new=%s  winner=%-4s  kingOut new=%d base=%d\n",
                g + 1, games, newIsBlack ? "B" : "W", tag, newOut, baseOut);
    std::fflush(stdout);
  }
  newEng.stop();
  baseEng.stop();

  double score = (newWins + 0.5 * draws) / games;
  std::printf("\n=== A/B result (%d games, budget %d playouts/move) ===\n",
              games, budget);
  std::printf("new %d  -  base %d  -  draw %d\n", newWins, baseWins, draws);
  std::printf("new score: %.1f%%\n", 100.0 * score);
  if (score > 0.0 && score < 1.0)
    std::printf("approx Elo delta: %+.0f\n",
                -400.0 * std::log10(1.0 / score - 1.0));
  std::printf("early king-walks: new in %d game(s), baseline in %d game(s)\n",
              newKingOut, baseKingOut);
  return 0;
}
