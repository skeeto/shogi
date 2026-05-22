// mate_test.cpp - differential test of the df-pn mate solver (src/mate.cpp)
// against a simple brute-force AND/OR reference.  The reference is obviously
// correct but slow; df-pn must agree with it, and must never claim a mate
// that does not exist.  Build (not wired into CMake):
//   g++ -O2 -std=c++17 -Isrc test/mate_test.cpp src/board.cpp src/mate.cpp \
//       -o build/mate_test
#include <cstdint>
#include <cstdio>
#include <vector>

#include "board.hpp"
#include "mate.hpp"

using namespace shogi;

static bool givesCheck(const Position& p, const Move& m) {
  Position c = p;
  doMove(c, m);
  return inCheck(c, c.stm());        // c.stm() is the opponent after the move
}

// Brute-force tsume: shortest forced mate for the side to move within
// `maxDepth` plies, else 0.  OR node = attacker plays a check; AND node =
// defender must be lost on every reply.  Slow but obviously correct.
static int bruteMate(const Position& p, int maxDepth) {
  if (maxDepth <= 0) return 0;
  std::vector<Move> legal;
  generateLegalMoves(p, legal);
  int best = 0;
  for (const Move& m : legal) {
    if (!givesCheck(p, m)) continue;
    Position after = p;
    doMove(after, m);
    std::vector<Move> replies;
    generateLegalMoves(after, replies);
    int worst;                                   // longest the defender holds
    if (replies.empty()) {
      worst = 1;                                 // checkmate now
    } else {
      worst = 0;
      for (const Move& r : replies) {
        Position d = after;
        doMove(d, r);
        int sub = bruteMate(d, maxDepth - 2);
        if (sub == 0) { worst = 0; break; }      // this reply escapes
        if (sub + 2 > worst) worst = sub + 2;
      }
    }
    if (worst > 0 && (best == 0 || worst < best)) best = worst;
  }
  return best;
}

static uint64_t rngState = 0x9e3779b97f4a7c15ULL;
static uint32_t rnd() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 7;
  rngState ^= rngState << 17;
  return uint32_t(rngState);
}

int main() {
  initZobrist();
  int failures = 0;

  // A. Two known mate-in-1 positions: df-pn must report length 1.
  auto mateInOne = [&](const char* name, const Position& p) {
    Move mm;
    int d = dfpnMate(p, 200000, mm);
    bool ok = (d == 1 && givesCheck(p, mm));
    std::printf("%-32s %s  (len %d)\n", name, ok ? "OK" : "FAIL", d);
    if (!ok) ++failures;
  };
  {
    Position p;
    p.board[sq(0, 0)] = makePiece(WHITE, PT_KING, false);
    p.board[sq(2, 1)] = makePiece(BLACK, PT_PAWN, false);
    p.board[sq(8, 4)] = makePiece(BLACK, PT_KING, false);
    p.hand[BLACK][PT_GOLD] = 1;
    p.side = BLACK;
    mateInOne("mate-in-1, gold drop", p);
  }
  {
    Position p;
    p.board[sq(0, 0)] = makePiece(WHITE, PT_KING, false);
    p.board[sq(1, 2)] = makePiece(BLACK, PT_GOLD, false);
    p.board[sq(2, 2)] = makePiece(BLACK, PT_SILVER, false);
    p.board[sq(8, 4)] = makePiece(BLACK, PT_KING, false);
    p.side = BLACK;
    mateInOne("mate-in-1, gold move", p);
  }

  // B. The opening has no forced mate.
  {
    Move mm;
    int d = dfpnMate(initialPosition(), 200000, mm);
    std::printf("%-32s %s  (len %d)\n", "opening: no mate",
                d == 0 ? "OK" : "FAIL", d);
    if (d != 0) ++failures;
  }

  // C. Differential test over random self-play positions.  df-pn must never
  //    claim a mate the brute-force search (depth 7) cannot confirm, and must
  //    not miss a shallow mate the brute-force search finds.
  int checked = 0, withMate = 0, falsePos = 0, missed = 0;
  for (int g = 0; g < 200; ++g) {
    Position p = initialPosition();
    for (int ply = 0; ply < 70; ++ply) {
      std::vector<Move> legal;
      generateLegalMoves(p, legal);
      if (legal.empty()) break;

      Move mm;
      int d = dfpnMate(p, 150000, mm);
      int b = bruteMate(p, 7);
      ++checked;
      if (d > 0) ++withMate;
      if (d > 0 && !givesCheck(p, mm)) ++falsePos;        // bad first move
      if (d > 0 && d <= 7 && b == 0)  ++falsePos;          // mate that isn't
      if (b > 0 && d == 0)            ++missed;            // missed a real one

      doMove(p, legal[rnd() % legal.size()]);
    }
  }
  std::printf("\ndifferential: %d positions, %d with a df-pn mate\n",
              checked, withMate);
  std::printf("false positives: %d   missed shallow mates: %d\n",
              falsePos, missed);
  if (falsePos > 0) { ++failures; }
  if (missed   > 0) { ++failures; }

  std::printf(failures ? "\n%d MATE TEST(S) FAILED\n"
                       : "\nALL MATE TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
