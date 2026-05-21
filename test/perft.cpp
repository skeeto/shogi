// perft.cpp - move-generation correctness check + a quick MCTS smoke test.
#include <cstdio>
#include "board.hpp"
#include "engine.hpp"
#include <chrono>
#include <thread>

using namespace shogi;

static long perft(const Position& p, int depth) {
  if (depth == 0) return 1;
  std::vector<Move> moves;
  generateLegalMoves(p, moves);
  if (depth == 1) return long(moves.size());
  long total = 0;
  for (const Move& m : moves) {
    Position c = p;
    doMove(c, m);
    total += perft(c, depth - 1);
  }
  return total;
}

int main() {
  initZobrist();
  Position p = initialPosition();

  // Known shogi perft values from the initial position.
  const long expect[] = {1, 30, 900, 25470, 719731};
  int rc = 0;
  for (int d = 1; d <= 4; ++d) {
    long got = perft(p, d);
    bool ok = (got == expect[d]);
    std::printf("perft(%d) = %-9ld expected %-9ld %s\n", d, got, expect[d],
                ok ? "OK" : "FAIL");
    if (!ok) rc = 1;
  }

  // MCTS smoke test: run the engine for one second, expect many playouts.
  Engine eng;
  std::printf("engine threads: %d\n", eng.threadCount());
  eng.setPosition(p);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  auto st = eng.stats();
  std::printf("after 1s: %d playouts, best=%s blackWinProb=%.3f\n",
              st.rootVisits, moveToString(st.bestMove).c_str(),
              st.blackWinProb);
  if (st.rootVisits < 100) { std::printf("FAIL: too few playouts\n"); rc = 1; }

  // Tree-reuse smoke test: advancing by the best move must keep its subtree.
  Position p2 = p;
  doMove(p2, st.bestMove);
  eng.advance(st.bestMove, p2);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  auto st2 = eng.stats();
  eng.stop();
  std::printf("after advance(%s): %d playouts, best=%s\n",
              moveToString(st.bestMove).c_str(), st2.rootVisits,
              moveToString(st2.bestMove).c_str());
  if (st2.rootVisits < 50) { std::printf("FAIL: advance lost the tree\n"); rc = 1; }
  std::printf(rc ? "\nTESTS FAILED\n" : "\nALL TESTS PASSED\n");
  return rc;
}
