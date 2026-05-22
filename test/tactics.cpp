// tactics.cpp - tactical sanity checks for the search.
//
// The engine must find forced mates.  Repetition checks are added once
// sennichite handling lands.  Built by hand, not wired into CMake:
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
static MCTS::Stats search(const Position& p, int budget) {
  Engine eng;
  eng.setPosition(p);
  auto t0 = std::chrono::steady_clock::now();
  while (eng.visits() < budget && !eng.solved()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) break;
  }
  MCTS::Stats st = eng.stats();
  eng.stop();
  return st;
}

// True if `m` checkmates the opponent (leaves them in check with no reply).
static bool isMatingMove(const Position& p, Move m) {
  if (m.isNull()) return false;
  Position c = p;
  doMove(c, m);
  std::vector<Move> reply;
  generateLegalMoves(c, reply);
  return reply.empty() && inCheck(c, c.stm());
}

// Assert the engine finds a mate-in-1 from `p` AND proves the win: the
// MCTS-Solver reports an exact 1.0 / 0.0, which an un-proven win never does.
static void mateInOne(const char* name, const Position& p) {
  MCTS::Stats st = search(p, 40000);
  bool mates  = isMatingMove(p, st.bestMove);
  bool proven = st.blackWinProb == ((p.stm() == BLACK) ? 1.0 : 0.0);
  check(mates && proven, name,
        "played " + moveToString(st.bestMove) +
        (mates ? "" : " NOT-MATE") + (proven ? " proven" : " UNPROVEN"));
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
    mateInOne("black mate-in-1, gold drop", p);
  }

  // B. Black mates with a board move: a gold steps in, defended by a silver.
  {
    Position p;
    p.board[sq(0, 0)] = makePiece(WHITE, PT_KING, false);
    p.board[sq(1, 2)] = makePiece(BLACK, PT_GOLD, false);
    p.board[sq(2, 2)] = makePiece(BLACK, PT_SILVER, false);
    p.board[sq(8, 4)] = makePiece(BLACK, PT_KING, false);
    p.side = BLACK;
    mateInOne("black mate-in-1, gold move", p);
  }

  // C. The mirror of A: White mates by dropping a gold.
  {
    Position p;
    p.board[sq(8, 8)] = makePiece(BLACK, PT_KING, false);
    p.board[sq(6, 7)] = makePiece(WHITE, PT_PAWN, false);
    p.board[sq(0, 4)] = makePiece(WHITE, PT_KING, false);
    p.hand[WHITE][PT_GOLD] = 1;
    p.side = WHITE;
    mateInOne("white mate-in-1, gold drop", p);
  }

  std::printf(failures ? "\n%d TACTICS TEST(S) FAILED\n"
                       : "\nALL TACTICS TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
