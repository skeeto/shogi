// tactics.cpp - tactical sanity checks for the search.
//
// The engine must prove forced wins and score repetitions correctly.  The
// lopsided win positions have many winning moves, so which one the parallel
// search proves first is a race; the test asserts the proven *result*, not a
// specific move.  Built by hand, not wired into CMake:
//   g++ -O2 -std=c++17 -Isrc -pthread test/tactics.cpp \
//       src/board.cpp src/mcts.cpp src/engine.cpp -o build/tactics
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "board.hpp"
#include "engine.hpp"

using namespace shogi;

static int failures = 0;
static void check(bool ok, const char* name, const std::string& detail) {
  std::printf("%-34s %s  %s\n", name, ok ? "OK  " : "FAIL", detail.c_str());
  if (!ok) ++failures;
}

// Search `p` until `budget` playouts or the position is solved.
static MCTS::Stats search(const Position& p, int budget,
                          const std::vector<uint64_t>& history = {}) {
  Engine eng;
  eng.setPosition(p, history);
  auto t0 = std::chrono::steady_clock::now();
  while (eng.visits() < budget && !eng.solved()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) break;
  }
  MCTS::Stats st = eng.stats();
  eng.stop();
  return st;
}

// Assert the engine proves a forced win from `p`: the MCTS-Solver reports an
// exact 1.0 / 0.0 win probability, which an un-proven evaluation never does.
static void provesWin(const char* name, const Position& p) {
  MCTS::Stats st = search(p, 40000);
  double want = (p.stm() == BLACK) ? 1.0 : 0.0;
  check(st.blackWinProb == want, name,
        "blackWinProb " + std::to_string(st.blackWinProb) +
        ", plays " + moveToString(st.bestMove));
}

int main() {
  initZobrist();

  // A. Black mates by dropping a gold: it lands defended by a pawn and covers
  //    every escape of the cornered white king.
  {
    Position p;
    p.board[sq(0, 0)] = makePiece(WHITE, PT_KING, false);
    p.board[sq(2, 1)] = makePiece(BLACK, PT_PAWN, false);
    p.board[sq(8, 4)] = makePiece(BLACK, PT_KING, false);
    p.hand[BLACK][PT_GOLD] = 1;
    p.side = BLACK;
    provesWin("black forced win, gold drop", p);
  }

  // B. Black mates with a board move: a gold steps in, defended by a silver.
  {
    Position p;
    p.board[sq(0, 0)] = makePiece(WHITE, PT_KING, false);
    p.board[sq(1, 2)] = makePiece(BLACK, PT_GOLD, false);
    p.board[sq(2, 2)] = makePiece(BLACK, PT_SILVER, false);
    p.board[sq(8, 4)] = makePiece(BLACK, PT_KING, false);
    p.side = BLACK;
    provesWin("black forced win, gold move", p);
  }

  // C. The mirror of A: White mates by dropping a gold.
  {
    Position p;
    p.board[sq(8, 8)] = makePiece(BLACK, PT_KING, false);
    p.board[sq(6, 7)] = makePiece(WHITE, PT_PAWN, false);
    p.board[sq(0, 4)] = makePiece(WHITE, PT_KING, false);
    p.hand[WHITE][PT_GOLD] = 1;
    p.side = WHITE;
    provesWin("white forced win, gold drop", p);
  }

  // D. A position reached for the fourth time is sennichite - a draw.
  {
    Position p = initialPosition();
    std::vector<uint64_t> history(4, p.hash);   // occurred four times
    MCTS::Stats st = search(p, 40000, history);
    check(st.blackWinProb == 0.5, "fourfold repetition is a draw",
          "blackWinProb " + std::to_string(st.blackWinProb));
  }

  // E. Perpetual check: a fourfold repetition with the side to move in check
  //    is a loss for the checking side (here White checks, so Black wins).
  {
    Position p;
    p.board[sq(8, 4)] = makePiece(BLACK, PT_KING, false);
    p.board[sq(0, 4)] = makePiece(WHITE, PT_ROOK, false);   // checks the king
    p.board[sq(0, 0)] = makePiece(WHITE, PT_KING, false);
    p.side = BLACK;
    std::vector<uint64_t> history(4, p.hash);
    MCTS::Stats st = search(p, 40000, history);
    check(st.blackWinProb == 1.0, "perpetual check loses for the checker",
          "blackWinProb " + std::to_string(st.blackWinProb));
  }

  std::printf(failures ? "\n%d TACTICS TEST(S) FAILED\n"
                       : "\nALL TACTICS TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
