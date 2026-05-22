// tune.cpp - self-play A/B between two CPUCT settings of the current engine.
//
// Each game pair starts from a short random opening (the same opening is
// played twice with the engines swapping colors), so the near-deterministic
// engines produce varied, decisive games instead of replaying one draw.
//
// Build:
//   g++ -O2 -std=c++17 -Isrc -pthread test/tune.cpp \
//       src/board.cpp src/mcts.cpp src/engine.cpp -o build/tune
//
// Run:  ./build/tune [games] [playout-budget] [cpuctA] [cpuctB]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "engine.hpp"

using namespace shogi;

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
  int    games  = (argc > 1) ? std::atoi(argv[1]) : 24;
  int    budget = (argc > 2) ? std::atoi(argv[2]) : 4000;
  double ca     = (argc > 3) ? std::atof(argv[3]) : 3.0;
  double cb     = (argc > 4) ? std::atof(argv[4]) : 2.0;

  initZobrist();
  Engine engA(0, ca), engB(0, cb);
  std::mt19937 rng(0xC0FFEE);
  std::vector<Move> opening;
  int aWins = 0, bWins = 0, draws = 0;

  for (int g = 0; g < games; ++g) {
    bool aIsBlack = (g % 2 == 0);
    if (g % 2 == 0) {                           // new random opening per pair
      opening.clear();
      Position op = initialPosition();
      int olen = 2 + int(rng() % 9);            // 2..10 random plies
      for (int i = 0; i < olen; ++i) {
        std::vector<Move> lm;
        generateLegalMoves(op, lm);
        if (lm.empty()) break;
        Move m = lm[rng() % lm.size()];
        opening.push_back(m);
        doMove(op, m);
      }
    }

    Position pos = initialPosition();
    std::vector<uint64_t> hist{pos.hash};
    for (const Move& m : opening) { doMove(pos, m); hist.push_back(pos.hash); }
    int result = -1;                            // 0 Black, 1 White, 2 draw

    for (int ply = 0; ply < 400; ++ply) {
      std::vector<Move> legal;
      generateLegalMoves(pos, legal);
      if (legal.empty()) { result = (pos.stm() == BLACK) ? 1 : 0; break; }

      bool aTurn = (pos.stm() == BLACK) == aIsBlack;
      Engine& e = aTurn ? engA : engB;
      e.setPosition(pos);
      waitBudget(e, budget);
      Move mv = e.stats().bestMove;
      e.stop();
      if (mv.isNull()) mv = legal[0];
      doMove(pos, mv);
      hist.push_back(pos.hash);

      int reps = 0;
      for (uint64_t h : hist) if (h == pos.hash) ++reps;
      if (reps >= 4) { result = 2; break; }
    }
    if (result < 0) result = 2;

    const char* tag;
    if (result == 2) { ++draws; tag = "draw"; }
    else {
      bool aWon = ((result == 0) == aIsBlack);
      if (aWon) { ++aWins; tag = "A"; } else { ++bWins; tag = "B"; }
    }
    std::printf("game %2d/%d  A=%s  winner=%s\n", g + 1, games,
                aIsBlack ? "B" : "W", tag);
    std::fflush(stdout);
  }
  engA.stop();
  engB.stop();

  double score = (aWins + 0.5 * draws) / games;
  std::printf("\n=== CPUCT %.2f (A) vs %.2f (B) - %d games, budget %d ===\n",
              ca, cb, games, budget);
  std::printf("A %d  -  B %d  -  draw %d\n", aWins, bWins, draws);
  std::printf("A score: %.1f%%\n", 100.0 * score);
  return 0;
}
