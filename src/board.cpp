// board.cpp - Shogi rules implementation.
#include "board.hpp"
#include <cmath>
#include <random>
#include <string>

namespace shogi {

// ---------------------------------------------------------------------------
// Zobrist hashing
// ---------------------------------------------------------------------------
static uint64_t Z_PIECE[N_SQ][64];
static uint64_t Z_HAND[2][HAND_TYPES][19];
static uint64_t Z_SIDE;

void initZobrist() {
  std::mt19937_64 rng(0x9e3779b97f4a7c15ULL);
  for (int s = 0; s < N_SQ; ++s)
    for (int p = 0; p < 64; ++p) Z_PIECE[s][p] = rng();
  for (int c = 0; c < 2; ++c)
    for (int t = 0; t < HAND_TYPES; ++t)
      for (int n = 0; n < 19; ++n) Z_HAND[c][t][n] = rng();
  Z_SIDE = rng();
}

static uint64_t computeHash(const Position& p) {
  uint64_t h = 0;
  for (int s = 0; s < N_SQ; ++s)
    if (p.board[s]) h ^= Z_PIECE[s][p.board[s] & 63];
  for (int c = 0; c < 2; ++c)
    for (int t = 1; t < HAND_TYPES; ++t) h ^= Z_HAND[c][t][p.hand[c][t]];
  if (p.side == WHITE) h ^= Z_SIDE;
  return h;
}

// ---------------------------------------------------------------------------
// Movement tables.  Deltas are (drow, dcol) given in BLACK orientation;
// for WHITE the row delta is negated.
// ---------------------------------------------------------------------------
struct D { int dr, dc; };

static const D GOLD_STEPS[]   = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,0}};
static const D SILVER_STEPS[] = {{-1,-1},{-1,0},{-1,1},{1,-1},{1,1}};
static const D KING_STEPS[]   = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
static const D PAWN_STEPS[]   = {{-1,0}};
static const D KNIGHT_STEPS[] = {{-2,-1},{-2,1}};
static const D ORTHO[]        = {{-1,0},{1,0},{0,-1},{0,1}};
static const D DIAG[]         = {{-1,-1},{-1,1},{1,-1},{1,1}};
static const D LANCE_RAY[]    = {{-1,0}};

// Fill `steps` with the single-step (non-ray, non-knight) deltas of `pc`.
static int pieceSteps(Piece pc, D* steps) {
  PieceType t = typeOf(pc);
  bool pr = isPromoted(pc);
  int sign = (colorOf(pc) == BLACK) ? 1 : -1;
  const D* src = nullptr;
  int n = 0;
  if (pr && (t == PT_PAWN || t == PT_LANCE || t == PT_KNIGHT || t == PT_SILVER)) {
    src = GOLD_STEPS; n = 6;
  } else if (t == PT_GOLD) {
    src = GOLD_STEPS; n = 6;
  } else if (t == PT_SILVER) {
    src = SILVER_STEPS; n = 5;
  } else if (t == PT_PAWN) {
    src = PAWN_STEPS; n = 1;
  } else if (t == PT_KING) {
    src = KING_STEPS; n = 8;
  } else if (t == PT_BISHOP && pr) {       // Horse: extra orthogonal steps
    src = ORTHO; n = 4;
  } else if (t == PT_ROOK && pr) {         // Dragon: extra diagonal steps
    src = DIAG; n = 4;
  }
  for (int i = 0; i < n; ++i) steps[i] = D{src[i].dr * sign, src[i].dc};
  return n;
}

// Fill `rays` with the sliding directions of `pc`.
static int pieceRays(Piece pc, D* rays) {
  PieceType t = typeOf(pc);
  bool pr = isPromoted(pc);
  int sign = (colorOf(pc) == BLACK) ? 1 : -1;
  const D* src = nullptr;
  int n = 0;
  if (t == PT_LANCE && !pr) { src = LANCE_RAY; n = 1; }
  else if (t == PT_BISHOP)  { src = DIAG;  n = 4; }
  else if (t == PT_ROOK)    { src = ORTHO; n = 4; }
  for (int i = 0; i < n; ++i) rays[i] = D{src[i].dr * sign, src[i].dc};
  return n;
}

// ---------------------------------------------------------------------------
// Attack detection
// ---------------------------------------------------------------------------
bool isAttacked(const Position& p, int s, Color by) {
  int r = rowOf(s), c = colOf(s);

  // Knight attackers: a `by` knight on `as` jumps to `s`.
  int kf = forward(by);
  for (int dc = -1; dc <= 1; dc += 2) {
    int ar = r - 2 * kf, ac = c - dc;
    if (ar >= 0 && ar < 9 && ac >= 0 && ac < 9) {
      Piece q = p.board[sq(ar, ac)];
      if (q && colorOf(q) == by && typeOf(q) == PT_KNIGHT && !isPromoted(q))
        return true;
    }
  }
  // Single-step attackers (gold/silver/pawn/king/promoted/horse/dragon steps).
  for (const D& d : KING_STEPS) {
    int ar = r + d.dr, ac = c + d.dc;
    if (ar < 0 || ar >= 9 || ac < 0 || ac >= 9) continue;
    Piece q = p.board[sq(ar, ac)];
    if (!q || colorOf(q) != by) continue;
    D steps[8];
    int n = pieceSteps(q, steps);
    for (int i = 0; i < n; ++i)             // step from attacker `as` to `s` is -d
      if (steps[i].dr == -d.dr && steps[i].dc == -d.dc) return true;
  }
  // Sliding attackers: walk outward, inspect the first piece met.
  static const D ALL_RAYS[8] = {{-1,-1},{-1,1},{1,-1},{1,1},{-1,0},{1,0},{0,-1},{0,1}};
  for (const D& dir : ALL_RAYS) {
    int ar = r + dir.dr, ac = c + dir.dc;
    while (ar >= 0 && ar < 9 && ac >= 0 && ac < 9) {
      Piece q = p.board[sq(ar, ac)];
      if (q) {
        if (colorOf(q) == by) {
          D rays[4];
          int n = pieceRays(q, rays);
          for (int i = 0; i < n; ++i)        // ray from `q` toward `s` is -dir
            if (rays[i].dr == -dir.dr && rays[i].dc == -dir.dc) return true;
        }
        break;
      }
      ar += dir.dr; ac += dir.dc;
    }
  }
  return false;
}

static int kingSquare(const Position& p, Color c) {
  Piece want = makePiece(c, PT_KING, false);
  for (int s = 0; s < N_SQ; ++s)
    if (p.board[s] == want) return s;
  return -1;
}

bool inCheck(const Position& p, Color color) {
  int ks = kingSquare(p, color);
  return ks >= 0 && isAttacked(p, ks, opp(color));
}

// ---------------------------------------------------------------------------
// Move application
// ---------------------------------------------------------------------------
void doMove(Position& p, const Move& m) {
  Color me = p.stm();
  if (m.isDrop()) {
    PieceType t = PieceType(m.drop);
    int cnt = p.hand[me][t];
    p.hash ^= Z_HAND[me][t][cnt] ^ Z_HAND[me][t][cnt - 1];
    p.hand[me][t] = uint8_t(cnt - 1);
    Piece pc = makePiece(me, t, false);
    p.board[m.to] = pc;
    p.hash ^= Z_PIECE[m.to][pc & 63];
  } else {
    Piece moving = p.board[m.from];
    p.hash ^= Z_PIECE[m.from][moving & 63];
    p.board[m.from] = 0;

    Piece captured = p.board[m.to];
    if (captured) {
      p.hash ^= Z_PIECE[m.to][captured & 63];
      PieceType bt = typeOf(captured);            // captured piece reverts
      int cnt = p.hand[me][bt];
      p.hash ^= Z_HAND[me][bt][cnt] ^ Z_HAND[me][bt][cnt + 1];
      p.hand[me][bt] = uint8_t(cnt + 1);
    }
    if (m.promo) moving |= PROMO_BIT;
    p.board[m.to] = moving;
    p.hash ^= Z_PIECE[m.to][moving & 63];
  }
  p.side ^= 1;
  p.hash ^= Z_SIDE;
  ++p.ply;
}

// ---------------------------------------------------------------------------
// Move generation
// ---------------------------------------------------------------------------
static void addBoardMove(int from, int to, Piece pc, std::vector<Move>& out) {
  PieceType t = typeOf(pc);
  Color c = colorOf(pc);
  bool promotable = !isPromoted(pc) &&
      (t == PT_PAWN || t == PT_LANCE || t == PT_KNIGHT ||
       t == PT_SILVER || t == PT_BISHOP || t == PT_ROOK);
  bool canPromo = promotable &&
      (inPromoZone(c, rowOf(from)) || inPromoZone(c, rowOf(to)));
  bool mustPromo = false;
  if (promotable) {
    int tr = rowOf(to);
    if (t == PT_PAWN || t == PT_LANCE)
      mustPromo = (c == BLACK) ? (tr == 0) : (tr == 8);
    else if (t == PT_KNIGHT)
      mustPromo = (c == BLACK) ? (tr <= 1) : (tr >= 7);
  }
  if (canPromo) out.push_back(boardMove(from, to, true));
  if (!mustPromo) out.push_back(boardMove(from, to, false));
}

static void genPseudo(const Position& p, std::vector<Move>& out) {
  Color me = p.stm();
  for (int from = 0; from < N_SQ; ++from) {
    Piece pc = p.board[from];
    if (!pc || colorOf(pc) != me) continue;
    int r = rowOf(from), c = colOf(from);

    // Knight jumps.
    if (typeOf(pc) == PT_KNIGHT && !isPromoted(pc)) {
      int kf = forward(me);
      for (int dc = -1; dc <= 1; dc += 2) {
        int tr = r + 2 * kf, tc = c + dc;
        if (tr < 0 || tr >= 9 || tc < 0 || tc >= 9) continue;
        Piece q = p.board[sq(tr, tc)];
        if (!q || colorOf(q) != me) addBoardMove(from, sq(tr, tc), pc, out);
      }
    }
    // Single steps.
    D steps[8];
    int ns = pieceSteps(pc, steps);
    for (int i = 0; i < ns; ++i) {
      int tr = r + steps[i].dr, tc = c + steps[i].dc;
      if (tr < 0 || tr >= 9 || tc < 0 || tc >= 9) continue;
      Piece q = p.board[sq(tr, tc)];
      if (!q || colorOf(q) != me) addBoardMove(from, sq(tr, tc), pc, out);
    }
    // Sliding rays.
    D rays[4];
    int nr = pieceRays(pc, rays);
    for (int i = 0; i < nr; ++i) {
      int tr = r + rays[i].dr, tc = c + rays[i].dc;
      while (tr >= 0 && tr < 9 && tc >= 0 && tc < 9) {
        Piece q = p.board[sq(tr, tc)];
        if (!q) {
          addBoardMove(from, sq(tr, tc), pc, out);
        } else {
          if (colorOf(q) != me) addBoardMove(from, sq(tr, tc), pc, out);
          break;
        }
        tr += rays[i].dr; tc += rays[i].dc;
      }
    }
  }

  // Drops.
  for (int t = PT_PAWN; t <= PT_ROOK; ++t) {
    if (p.hand[me][t] == 0) continue;
    for (int s = 0; s < N_SQ; ++s) {
      if (p.board[s]) continue;
      int row = rowOf(s);
      bool lastRank = (me == BLACK) ? (row == 0) : (row == 8);
      bool lastTwo  = (me == BLACK) ? (row <= 1) : (row >= 7);
      if (t == PT_PAWN || t == PT_LANCE) { if (lastRank) continue; }
      if (t == PT_KNIGHT) { if (lastTwo) continue; }
      if (t == PT_PAWN) {                                // nifu
        bool nifu = false;
        int col = colOf(s);
        for (int rr = 0; rr < 9; ++rr) {
          Piece q = p.board[sq(rr, col)];
          if (q && colorOf(q) == me && typeOf(q) == PT_PAWN && !isPromoted(q)) {
            nifu = true; break;
          }
        }
        if (nifu) continue;
      }
      out.push_back(dropMove(PieceType(t), s));
    }
  }
}

void generateLegalMoves(const Position& p, std::vector<Move>& out) {
  out.clear();
  std::vector<Move> pseudo;
  pseudo.reserve(128);
  genPseudo(p, pseudo);
  Color me = p.stm();
  for (const Move& m : pseudo) {
    Position c = p;
    doMove(c, m);
    if (inCheck(c, me)) continue;                         // leaves own king in check
    if (m.isDrop() && m.drop == PT_PAWN && inCheck(c, opp(me))) {
      // Uchifuzume: dropping a pawn for immediate checkmate is illegal.
      std::vector<Move> reply;
      generateLegalMoves(c, reply);
      if (reply.empty()) continue;
    }
    out.push_back(m);
  }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
static int baseValue(PieceType t) {
  switch (t) {
    case PT_PAWN:   return 90;
    case PT_LANCE:  return 230;
    case PT_KNIGHT: return 250;
    case PT_SILVER: return 360;
    case PT_GOLD:   return 440;
    case PT_BISHOP: return 560;
    case PT_ROOK:   return 640;
    default:        return 0;
  }
}
static int promoValue(PieceType t) {
  switch (t) {
    case PT_PAWN:   return 540;
    case PT_LANCE:  return 480;
    case PT_KNIGHT: return 510;
    case PT_SILVER: return 490;
    case PT_BISHOP: return 830;
    case PT_ROOK:   return 950;
    default:        return 0;
  }
}

double evalBlackWinProb(const Position& p) {
  int score = 0;
  for (int s = 0; s < N_SQ; ++s) {
    Piece q = p.board[s];
    if (!q || typeOf(q) == PT_KING) continue;
    int v = isPromoted(q) ? promoValue(typeOf(q)) : baseValue(typeOf(q));
    score += (colorOf(q) == BLACK) ? v : -v;
  }
  for (int t = PT_PAWN; t <= PT_ROOK; ++t) {
    int v = baseValue(PieceType(t));
    score += p.hand[BLACK][t] * v;
    score -= p.hand[WHITE][t] * v;
  }
  return 1.0 / (1.0 + std::exp(-double(score) / 512.0));
}

// ---------------------------------------------------------------------------
// Notation
// ---------------------------------------------------------------------------
std::string moveToString(const Move& m) {
  static const char* PT = " PLNSGBRK";
  auto coord = [](int s) {
    std::string r;
    r += char('1' + colOf(s));
    r += char('a' + rowOf(s));
    return r;
  };
  if (m.isNull()) return "----";
  if (m.isDrop()) {
    std::string r;
    r += PT[m.drop];
    r += '*';
    r += coord(m.to);
    return r;
  }
  std::string r = coord(m.from) + coord(m.to);
  if (m.promo) r += '+';
  return r;
}

Position initialPosition() {
  Position p;  // default member initializers zero the board, hands and hash
  const PieceType back[9] = {PT_LANCE, PT_KNIGHT, PT_SILVER, PT_GOLD, PT_KING,
                             PT_GOLD,  PT_SILVER, PT_KNIGHT, PT_LANCE};
  // White (top): rows 0-2.
  for (int c = 0; c < 9; ++c) p.board[sq(0, c)] = makePiece(WHITE, back[c], false);
  p.board[sq(1, 1)] = makePiece(WHITE, PT_ROOK, false);
  p.board[sq(1, 7)] = makePiece(WHITE, PT_BISHOP, false);
  for (int c = 0; c < 9; ++c) p.board[sq(2, c)] = makePiece(WHITE, PT_PAWN, false);
  // Black (bottom): rows 6-8.
  for (int c = 0; c < 9; ++c) p.board[sq(6, c)] = makePiece(BLACK, PT_PAWN, false);
  p.board[sq(7, 1)] = makePiece(BLACK, PT_BISHOP, false);
  p.board[sq(7, 7)] = makePiece(BLACK, PT_ROOK, false);
  for (int c = 0; c < 9; ++c) p.board[sq(8, c)] = makePiece(BLACK, back[c], false);
  p.side = BLACK;
  p.ply = 0;
  p.hash = computeHash(p);
  return p;
}

}  // namespace shogi
