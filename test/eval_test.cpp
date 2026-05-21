// eval_test.cpp - unit checks for the static evaluation (board.cpp).
//
// Build manually (not wired into CMake):
//   g++ -O2 -std=c++17 -Isrc test/eval_test.cpp src/board.cpp -o build/eval_test
#include <cmath>
#include <cstdio>
#include "board.hpp"

using namespace shogi;

static int failures = 0;
static void check(bool ok, const char* name) {
  std::printf("%-44s %s\n", name, ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

// Colour-swap + 180-degree board reflection.  Evaluation must be antisymmetric
// under this: a mirrored position's Black win prob is 1 minus the original's.
static Position mirror(const Position& p) {
  Position m;
  for (int s = 0; s < N_SQ; ++s)
    if (p.board[s]) m.board[80 - s] = Piece(p.board[s] ^ COLOR_BIT);
  for (int t = 0; t < HAND_TYPES; ++t) {
    m.hand[BLACK][t] = p.hand[WHITE][t];
    m.hand[WHITE][t] = p.hand[BLACK][t];
  }
  m.side = opp(p.stm());
  m.ply = p.ply;
  return m;
}

int main() {
  initZobrist();

  // A. The opening position is symmetric -> exactly 0.5.
  double start = evalBlackWinProb(initialPosition());
  check(std::fabs(start - 0.5) < 1e-9, "opening position evaluates to 0.5");

  // B. A king walked out to the centre is clearly worse for its side.
  {
    Position p = initialPosition();
    p.board[sq(8, 4)] = 0;
    p.board[sq(4, 4)] = makePiece(BLACK, PT_KING, false);
    double walked = evalBlackWinProb(p);
    check(walked < start - 0.03, "exposed king scores worse than home king");
  }

  // C. Removing a White rook helps Black.
  {
    Position p = initialPosition();
    p.board[sq(1, 7)] = 0;                       // White's rook
    double v = evalBlackWinProb(p);
    check(v > start + 0.05, "extra material raises the side's win prob");

    // D. ... and the mirrored position must give 1 - v.
    double mv = evalBlackWinProb(mirror(p));
    check(std::fabs(v + mv - 1.0) < 1e-9, "evaluation is mirror-antisymmetric");
  }

  // E. In a bare-king endgame the king-position penalty fades to nothing.
  {
    Position p;
    p.board[sq(4, 4)] = makePiece(BLACK, PT_KING, false);
    p.board[sq(0, 4)] = makePiece(WHITE, PT_KING, false);
    p.side = BLACK;
    double v = evalBlackWinProb(p);
    check(std::fabs(v - 0.5) < 1e-9, "king-safety penalty fades in the endgame");
  }

  // F. More enemy pressure around the king is worse, at equal material.
  {
    Position p = initialPosition();                // relocate White's silvers
    p.board[sq(0, 2)] = 0;
    p.board[sq(0, 6)] = 0;
    p.board[sq(7, 3)] = makePiece(WHITE, PT_SILVER, false);
    p.board[sq(7, 5)] = makePiece(WHITE, PT_SILVER, false);
    double v = evalBlackWinProb(p);
    check(v < start - 0.01, "enemy pieces near the king lower the win prob");
  }

  std::printf(failures ? "\n%d TEST(S) FAILED\n" : "\nALL EVAL TESTS PASSED\n",
              failures);
  return failures ? 1 : 0;
}
