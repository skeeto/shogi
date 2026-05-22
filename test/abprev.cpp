// abprev.cpp - A/B self-play: the current engine vs a snapshot of an earlier
// version, snapshotted under namespace `shogiprev` in test/prev/.  Used to
// measure the strength delta of one change cleanly (selfplay.cpp's fixed
// 8a6430f baseline is now too weak to discriminate).
//
// Each game pair starts from a short random opening played twice with the
// engines swapping colours, so the near-identical engines produce decisive
// games instead of replaying one draw (see tune.cpp).
//
// Refresh the snapshot to the pre-change commit before measuring:
//   mkdir -p test/prev
//   for f in board mate mcts engine; do for e in hpp cpp; do \
//     git show <commit>:src/$f.$e \
//       | sed 's/namespace shogi/namespace shogiprev/g' > test/prev/$f.$e; \
//   done; done
//
// Build:
//   g++ -O2 -std=c++17 -Isrc -Itest -pthread test/abprev.cpp \
//       src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp \
//       test/prev/board.cpp test/prev/mate.cpp test/prev/mcts.cpp \
//       test/prev/engine.cpp -o build/abprev
//
// Run:  ./build/abprev [games] [playout-budget]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "engine.hpp"            // current engine, namespace shogi
#include "prev/engine.hpp"       // snapshot engine, namespace shogiprev

namespace S = shogi;
namespace P = shogiprev;

// The two Position structs are layout-identical; copy field by field.
static P::Position toPrev(const S::Position& p) {
  P::Position b;
  for (int i = 0; i < S::N_SQ; ++i) b.board[i] = p.board[i];
  for (int c = 0; c < 2; ++c)
    for (int t = 0; t < S::HAND_TYPES; ++t) b.hand[c][t] = p.hand[c][t];
  b.side = p.side;
  b.ply  = p.ply;
  b.hash = p.hash;
  return b;
}

template <class Eng>
static void waitBudget(Eng& e, int budget) {
  auto t0 = std::chrono::steady_clock::now();
  int last = -1, stalls = 0;
  while (e.visits() < budget) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    int v = e.visits();
    if (v == last) {                        // no progress: the position solved
      if (++stalls > 25) break;
    } else { last = v; stalls = 0; }
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) break;
  }
}

int main(int argc, char** argv) {
  int games  = (argc > 1) ? std::atoi(argv[1]) : 30;
  int budget = (argc > 2) ? std::atoi(argv[2]) : 2500;

  S::initZobrist();
  P::initZobrist();
  S::Engine curEng;
  P::Engine prevEng;
  std::mt19937 rng(0xC0FFEE);
  std::vector<S::Move> opening;
  int curWins = 0, prevWins = 0, draws = 0;

  for (int g = 0; g < games; ++g) {
    bool curIsBlack = (g % 2 == 0);
    if (g % 2 == 0) {                           // new random opening per pair
      opening.clear();
      S::Position op = S::initialPosition();
      int olen = 2 + int(rng() % 9);            // 2..10 random plies
      for (int i = 0; i < olen; ++i) {
        std::vector<S::Move> lm;
        S::generateLegalMoves(op, lm);
        if (lm.empty()) break;
        S::Move m = lm[rng() % lm.size()];
        opening.push_back(m);
        S::doMove(op, m);
      }
    }

    S::Position pos = S::initialPosition();
    std::vector<uint64_t> hist{pos.hash};
    for (const S::Move& m : opening) {
      S::doMove(pos, m);
      hist.push_back(pos.hash);
    }
    int result = -1;                            // 0 Black, 1 White, 2 draw

    for (int ply = 0; ply < 400; ++ply) {
      std::vector<S::Move> legal;
      S::generateLegalMoves(pos, legal);
      if (legal.empty()) { result = (pos.stm() == S::BLACK) ? 1 : 0; break; }

      bool curTurn = (pos.stm() == S::BLACK) == curIsBlack;
      S::Move mv;
      if (curTurn) {
        curEng.setPosition(pos, hist);
        waitBudget(curEng, budget);
        mv = curEng.stats().bestMove;
        curEng.stop();
      } else {
        prevEng.setPosition(toPrev(pos));
        waitBudget(prevEng, budget);
        P::Move pm = prevEng.stats().bestMove;
        prevEng.stop();
        mv.from = pm.from; mv.to = pm.to; mv.promo = pm.promo; mv.drop = pm.drop;
      }
      if (mv.isNull()) mv = legal[0];
      S::doMove(pos, mv);
      hist.push_back(pos.hash);

      int reps = 0;
      for (uint64_t h : hist) if (h == pos.hash) ++reps;
      if (reps >= 4) { result = 2; break; }     // fourfold repetition
    }
    if (result < 0) result = 2;                 // ply cap -> draw

    const char* tag;
    if (result == 2) { ++draws; tag = "draw"; }
    else {
      bool curWon = ((result == 0) == curIsBlack);
      if (curWon) { ++curWins;  tag = "cur";  }
      else        { ++prevWins; tag = "prev"; }
    }
    std::printf("game %2d/%d  cur=%s  winner=%-4s\n", g + 1, games,
                curIsBlack ? "B" : "W", tag);
    std::fflush(stdout);
  }
  curEng.stop();
  prevEng.stop();

  double score = (curWins + 0.5 * draws) / games;
  std::printf("\n=== current vs prev snapshot (%d games, budget %d) ===\n",
              games, budget);
  std::printf("cur %d  -  prev %d  -  draw %d\n", curWins, prevWins, draws);
  std::printf("cur score: %.1f%%\n", 100.0 * score);
  if (score > 0.0 && score < 1.0)
    std::printf("approx Elo delta: %+.0f\n",
                -400.0 * std::log10(1.0 / score - 1.0));
  return 0;
}
