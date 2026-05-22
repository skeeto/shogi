// tune_eval.cpp - Texel-style self-play tuning of the static-eval piece values.
//
// No pro-game database is available, so this fits the static evaluation to
// the engine's own deeper judgement: it plays self-play games and labels each
// quiet position with the *search* win probability for that position (not the
// game outcome - in equal-vs-equal self-play the outcome is near a coin flip
// for most positions and tuning to it degenerates the eval).  It then
// coordinate-descends evalPieceBase / evalPiecePromo to minimise the mean of
// (evalBlackWinProb - searchValue)^2.  Copy the tuned values into board.cpp
// and A/B with abprev.
//
// Build (not wired into CMake):
//   g++ -O2 -std=c++17 -Isrc -pthread test/tune_eval.cpp \
//       src/board.cpp src/mate.cpp src/mcts.cpp src/engine.cpp -o build/tune_eval
// Run:  ./build/tune_eval [games] [playout-budget]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "board.hpp"
#include "engine.hpp"

using namespace shogi;

static void waitBudget(Engine& e, int budget) {
  auto t0 = std::chrono::steady_clock::now();
  int last = -1, stalls = 0;
  while (e.visits() < budget) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    int v = e.visits();
    if (v == last) { if (++stalls > 25) break; } else { last = v; stalls = 0; }
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) break;
  }
}

struct Sample { Position pos; double label; };   // label = search win prob

int main(int argc, char** argv) {
  int games  = (argc > 1) ? std::atoi(argv[1]) : 150;
  int budget = (argc > 2) ? std::atoi(argv[2]) : 4000;

  initZobrist();
  Engine eng;
  std::mt19937 rng(0xE7A1C0DE);
  std::vector<Sample> data;
  std::vector<Move> opening;

  for (int g = 0; g < games; ++g) {
    opening.clear();
    Position op = initialPosition();
    int olen = 2 + int(rng() % 11);                  // 2..12 random plies
    for (int i = 0; i < olen; ++i) {
      std::vector<Move> lm;
      generateLegalMoves(op, lm);
      if (lm.empty()) break;
      Move m = lm[rng() % lm.size()];
      opening.push_back(m);
      doMove(op, m);
    }
    Position pos = initialPosition();
    std::vector<uint64_t> hist{pos.hash};
    for (const Move& m : opening) { doMove(pos, m); hist.push_back(pos.hash); }

    int plies = 0;
    for (int ply = 0; ply < 360; ++ply) {
      std::vector<Move> legal;
      generateLegalMoves(pos, legal);
      if (legal.empty()) break;
      eng.setPosition(pos, hist);
      waitBudget(eng, budget);
      MCTS::Stats st = eng.stats();
      eng.stop();
      // Label each quiet position with the search's own value for it - a far
      // cleaner training target than the near-coin-flip game outcome.
      if (ply >= 6 && !inCheck(pos, pos.stm()))
        data.push_back({pos, st.blackWinProb});
      Move mv = st.bestMove;
      if (mv.isNull()) mv = legal[0];
      doMove(pos, mv);
      hist.push_back(pos.hash);
      ++plies;
      int reps = 0;
      for (uint64_t h : hist) if (h == pos.hash) ++reps;
      if (reps >= 4) break;
    }
    std::printf("game %d/%d  plies=%d  samples=%zu\n",
                g + 1, games, plies, data.size());
    std::fflush(stdout);
  }
  eng.stop();
  if (data.empty()) { std::printf("no data\n"); return 1; }

  // --- Texel coordinate descent over the 13 piece values --------------------
  auto loss = [&]() {
    double s = 0.0;
    for (const Sample& d : data) {
      double e = evalBlackWinProb(d.pos) - d.label;
      s += e * e;
    }
    return s / double(data.size());
  };
  int* P[13] = {
    &evalPieceBase[1], &evalPieceBase[2], &evalPieceBase[3], &evalPieceBase[4],
    &evalPieceBase[5], &evalPieceBase[6], &evalPieceBase[7],
    &evalPiecePromo[1], &evalPiecePromo[2], &evalPiecePromo[3],
    &evalPiecePromo[4], &evalPiecePromo[6], &evalPiecePromo[7],
  };
  const char* names[13] = {"P","L","N","S","G","B","R",
                           "+P","+L","+N","+S","+B","+R"};

  double initLoss = loss();
  double best = initLoss;
  for (int step = 32; step >= 1; step /= 2) {
    bool improved = true;
    while (improved) {
      improved = false;
      for (int i = 0; i < 13; ++i) {
        int orig = *P[i];
        *P[i] = orig + step;                         // try larger
        double up = loss();
        if (up < best - 1e-9) { best = up; improved = true; continue; }
        *P[i] = (orig - step > 0) ? orig - step : orig;   // try smaller
        double dn = loss();
        if (*P[i] != orig && dn < best - 1e-9) {
          best = dn; improved = true; continue;
        }
        *P[i] = orig;                                // no gain: revert
      }
    }
  }

  std::printf("\n=== tuned %zu positions, %d games, budget %d ===\n",
              data.size(), games, budget);
  std::printf("loss %.6f -> %.6f\n", initLoss, best);
  std::printf("evalPieceBase  = {0,%4d,%4d,%4d,%4d,%4d,%4d,%4d, 0};\n",
              evalPieceBase[1], evalPieceBase[2], evalPieceBase[3],
              evalPieceBase[4], evalPieceBase[5], evalPieceBase[6],
              evalPieceBase[7]);
  std::printf("evalPiecePromo = {0,%4d,%4d,%4d,%4d,   0,%4d,%4d, 0};\n",
              evalPiecePromo[1], evalPiecePromo[2], evalPiecePromo[3],
              evalPiecePromo[4], evalPiecePromo[6], evalPiecePromo[7]);
  for (int i = 0; i < 13; ++i)
    std::printf("  %-3s %d\n", names[i], *P[i]);
  return 0;
}
