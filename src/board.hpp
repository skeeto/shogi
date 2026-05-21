// board.h - Shogi rules, state representation and move generation.
//
// State representation is chosen to be a small, trivially-copyable POD so the
// MCTS search can clone positions cheaply (one memcpy, ~100 bytes).
#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <string>

namespace shogi {

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------
enum Color : uint8_t { BLACK = 0, WHITE = 1 };
inline Color opp(Color c) { return Color(c ^ 1); }

// ---------------------------------------------------------------------------
// Piece types.  A "piece code" packs type + promoted flag + color into a byte.
//   bits 0-3 : type (1..8)
//   bit  4   : promoted
//   bit  5   : color (0 = black, 1 = white)
// 0 means an empty square.
// ---------------------------------------------------------------------------
enum PieceType : uint8_t {
  PT_NONE   = 0,
  PT_PAWN   = 1,
  PT_LANCE  = 2,
  PT_KNIGHT = 3,
  PT_SILVER = 4,
  PT_GOLD   = 5,
  PT_BISHOP = 6,
  PT_ROOK   = 7,
  PT_KING   = 8,
};

constexpr uint8_t PROMO_BIT = 0x10;
constexpr uint8_t COLOR_BIT = 0x20;

using Piece = uint8_t;

inline Piece     makePiece(Color c, PieceType t, bool promo) {
  return uint8_t(t) | (promo ? PROMO_BIT : 0) | (c ? COLOR_BIT : 0);
}
inline bool      isEmpty(Piece p)   { return p == 0; }
inline PieceType typeOf(Piece p)    { return PieceType(p & 0x0F); }
inline bool      isPromoted(Piece p){ return (p & PROMO_BIT) != 0; }
inline Color     colorOf(Piece p)   { return Color((p & COLOR_BIT) >> 5); }

// Piece types as held "in hand" (always unpromoted, no king): PT_PAWN..PT_ROOK.
constexpr int HAND_TYPES = 8;  // index by PieceType, slots 1..7 used

// ---------------------------------------------------------------------------
// Board geometry.  81 squares, row 0 at the top of the screen.
// White (gote) starts at the top and moves down (+row).
// Black (sente) starts at the bottom and moves up (-row).
// ---------------------------------------------------------------------------
constexpr int N_SQ = 81;
inline int sq(int row, int col) { return row * 9 + col; }
inline int rowOf(int s)         { return s / 9; }
inline int colOf(int s)         { return s % 9; }
inline int forward(Color c)     { return c == BLACK ? -1 : 1; }

// In the promotion zone? (last three ranks from the moving side's view)
inline bool inPromoZone(Color c, int row) {
  return c == BLACK ? (row <= 2) : (row >= 6);
}

// ---------------------------------------------------------------------------
// Move.  A drop is encoded with from == DROP_FROM and `drop` set to the type.
// ---------------------------------------------------------------------------
constexpr uint8_t DROP_FROM = 255;

struct Move {
  uint8_t from  = DROP_FROM;
  uint8_t to    = 0;
  uint8_t promo = 0;
  uint8_t drop  = PT_NONE;

  bool isDrop() const { return from == DROP_FROM; }
  bool isNull() const { return from == DROP_FROM && drop == PT_NONE; }
  bool operator==(const Move& m) const {
    return from == m.from && to == m.to && promo == m.promo && drop == m.drop;
  }
};
inline Move nullMove() { return Move{}; }
inline Move boardMove(int from, int to, bool promo) {
  return Move{uint8_t(from), uint8_t(to), uint8_t(promo), PT_NONE};
}
inline Move dropMove(PieceType t, int to) {
  return Move{DROP_FROM, uint8_t(to), 0, uint8_t(t)};
}

// ---------------------------------------------------------------------------
// Position - the full game state.  POD, trivially copyable.
// ---------------------------------------------------------------------------
struct Position {
  uint8_t  board[N_SQ]      = {};   // piece codes, 0 = empty
  uint8_t  hand[2][HAND_TYPES] = {};// hand[color][type] = count
  uint8_t  side             = BLACK;// side to move
  uint16_t ply              = 0;    // half-moves played
  uint64_t hash             = 0;    // Zobrist hash (includes side to move)

  Color stm() const { return Color(side); }
};

enum Result : uint8_t { ONGOING, BLACK_WIN, WHITE_WIN, DRAW };

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void     initZobrist();                       // call once at startup
Position initialPosition();

// Pseudo-legal generation is filtered for king safety, so these are fully legal.
void generateLegalMoves(const Position& p, std::vector<Move>& out);

// Is `color`'s king attacked?
bool inCheck(const Position& p, Color color);

// Is square `s` attacked by any piece of color `by`?
bool isAttacked(const Position& p, int s, Color by);

// Apply a (legal) move in place.
void doMove(Position& p, const Move& m);

// Static evaluation, returned as Black's win probability in [0,1].
double evalBlackWinProb(const Position& p);

// Human-readable move, e.g. "7g7f" or "P*5e" / "8h2b+".
std::string moveToString(const Move& m);

}  // namespace shogi
