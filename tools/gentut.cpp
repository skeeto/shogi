// gentut.cpp - generates the tutorial illustrations under docs/tutorial/img/
// in the same visual style as the game (cream pentagonal pieces with kanji
// on a wood-toned board).  Standalone: no link against src/*.cpp, only the
// glyphs.hpp atlas + a vendored single-header PNG encoder.
//
// Build & run from the repo root:
//   c++ -O2 -std=c++17 -Isrc -Itools -o /tmp/gentut tools/gentut.cpp
//   /tmp/gentut                # writes docs/tutorial/img/*.png
//
// Output is committed - this is an offline tool like genfont.cpp/genicon.cpp,
// not wired into CMake.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "glyphs.hpp"               // shogi::KANJI_GLYPHS, GLYPH_PIX, GlyphInfo

using namespace shogi;

namespace {

struct RGBA { uint8_t r, g, b, a; };

// --- palette: matches src/ui.cpp's C_* constants ---------------------------
constexpr RGBA C_BG       {26,  27,  32,  255};
constexpr RGBA C_WOOD     {222, 184, 120, 255};
constexpr RGBA C_LINE     {48,  34,  16,  255};
constexpr RGBA C_BORDER   {62,  44,  18,  255};
constexpr RGBA C_FACE_B   {244, 212, 150, 255};
constexpr RGBA C_FACE_W   {236, 226, 200, 255};
constexpr RGBA C_KANJI    {38,  26,  12,  255};
constexpr RGBA C_KANJI_P  {176, 36,  24,  255};
constexpr RGBA C_DOT      {40,  90,  200, 210};   // legal-destination marker
constexpr RGBA C_LAST     {120, 170, 110, 180};   // last-move highlight
constexpr RGBA C_BAD      {220, 60,  50,  200};   // illegal-move highlight
constexpr RGBA C_ZONE     {200, 80,  60,  90};    // promotion-zone overlay

// --- piece encoding: matches src/board.hpp's byte layout -------------------
enum : uint8_t {
  PT_NONE=0, PT_PAWN=1, PT_LANCE=2, PT_KNIGHT=3, PT_SILVER=4,
  PT_GOLD=5, PT_BISHOP=6, PT_ROOK=7, PT_KING=8,
  PROMO_BIT=0x10, COLOR_BIT=0x20,
};
enum Color : uint8_t { BLACK=0, WHITE=1 };
using Piece = uint8_t;

Piece P(Color c, int t, bool pr=false) {
  return uint8_t(t) | (pr ? PROMO_BIT : 0) | (c ? COLOR_BIT : 0);
}
int   T (Piece p)  { return p & 0x0F; }
bool  Pr(Piece p)  { return (p & PROMO_BIT) != 0; }
Color Co(Piece p)  { return Color((p & COLOR_BIT) >> 5); }

// Index into KANJI_GLYPHS, parallels src/ui.cpp's kanjiIndex().
int kanjiIndex(Piece pc) {
  int t = T(pc);
  if (Pr(pc)) {
    switch (t) {
      case PT_PAWN:   return 9;    // と
      case PT_LANCE:  return 10;   // 杏
      case PT_KNIGHT: return 11;   // 圭
      case PT_SILVER: return 12;   // 全
      case PT_BISHOP: return 13;   // 馬
      case PT_ROOK:   return 14;   // 龍
      default:        return 4;
    }
  }
  switch (t) {
    case PT_PAWN:   return 0;      // 歩
    case PT_LANCE:  return 1;      // 香
    case PT_KNIGHT: return 2;      // 桂
    case PT_SILVER: return 3;      // 銀
    case PT_GOLD:   return 4;      // 金
    case PT_BISHOP: return 5;      // 角
    case PT_ROOK:   return 6;      // 飛
    case PT_KING:   return Co(pc) == BLACK ? 7 : 8;   // 王 / 玉
    default:        return 0;
  }
}

// --- board state -----------------------------------------------------------
constexpr int N_SQ = 81;
inline int rowOf(int s)        { return s / 9; }
inline int colOf(int s)        { return s % 9; }
inline int sq   (int r, int c) { return r * 9 + c; }

struct Position {
  Piece   board[N_SQ]    = {};
  uint8_t hand[2][8]     = {};   // slots 1..7 = P,L,N,S,G,B,R
  Color   side           = BLACK;
};

Position initialPosition() {
  Position p;
  static const int back[9] = {PT_LANCE, PT_KNIGHT, PT_SILVER, PT_GOLD, PT_KING,
                              PT_GOLD,  PT_SILVER, PT_KNIGHT, PT_LANCE};
  for (int c = 0; c < 9; ++c) p.board[sq(0, c)] = P(WHITE, back[c]);
  p.board[sq(1, 1)] = P(WHITE, PT_ROOK);
  p.board[sq(1, 7)] = P(WHITE, PT_BISHOP);
  for (int c = 0; c < 9; ++c) p.board[sq(2, c)] = P(WHITE, PT_PAWN);
  for (int c = 0; c < 9; ++c) p.board[sq(6, c)] = P(BLACK, PT_PAWN);
  p.board[sq(7, 1)] = P(BLACK, PT_BISHOP);
  p.board[sq(7, 7)] = P(BLACK, PT_ROOK);
  for (int c = 0; c < 9; ++c) p.board[sq(8, c)] = P(BLACK, back[c]);
  p.side = BLACK;
  return p;
}

// Initial position with White's pieces wiped off the board - used for the
// castle / climbing-silver diagrams, where Black is the only side actually
// moving and showing a stationary White only confuses the reader.
Position blackOnlyInitial() {
  Position p = initialPosition();
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 9; ++c)
      p.board[sq(r, c)] = 0;
  return p;
}

// Apply a move without legality checks; just enough to set up scripted
// sequences (opening, castle, climbing silver).
void mv(Position& p, int from, int to, bool promo = false) {
  Piece pc  = p.board[from];
  Piece cap = p.board[to];
  if (cap) p.hand[Co(pc)][T(cap)]++;
  if (promo) pc |= PROMO_BIT;
  p.board[from] = 0;
  p.board[to]   = pc;
  p.side = Color(p.side ^ 1);
}

// --- pixel primitives: lifted verbatim from tools/genicon.cpp --------------
float segDist(float px, float py, float ax, float ay, float bx, float by) {
  float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
  float c = vx * vx + vy * vy;
  float t = c > 0 ? std::clamp((wx * vx + wy * vy) / c, 0.f, 1.f) : 0.f;
  float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
  return std::sqrt(dx * dx + dy * dy);
}
bool pointInPoly(float px, float py, const float* xs, const float* ys, int n) {
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++)
    if (((ys[i] > py) != (ys[j] > py)) &&
        (px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i]))
      in = !in;
  return in;
}
// Straight-alpha "src colour over dst" honouring src.a as a coverage factor.
RGBA over(RGBA dst, RGBA src, float sa) {
  sa = std::clamp(sa, 0.f, 1.f) * (src.a / 255.f);
  float da = dst.a / 255.f;
  float oa = sa + da * (1.f - sa);
  if (oa <= 0.f) return RGBA{0, 0, 0, 0};
  auto ch = [&](float s, float d) {
    return uint8_t(std::clamp((s * sa + d * da * (1.f - sa)) / oa, 0.f, 255.f)
                   + 0.5f);
  };
  return RGBA{ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b),
              uint8_t(oa * 255.f + 0.5f)};
}
// Box-filter downscale in premultiplied alpha (S must be N * integer).
std::vector<RGBA> downscale(const std::vector<RGBA>& src, int Sw, int Sh,
                            int Nw, int Nh) {
  std::vector<RGBA> dst(size_t(Nw) * Nh);
  int bx = Sw / Nw, by = Sh / Nh;
  for (int y = 0; y < Nh; ++y)
    for (int x = 0; x < Nw; ++x) {
      float r = 0, g = 0, b = 0, a = 0;
      for (int sy = 0; sy < by; ++sy)
        for (int sx = 0; sx < bx; ++sx) {
          RGBA p = src[size_t(y * by + sy) * Sw + (x * bx + sx)];
          float pa = p.a / 255.f;
          r += p.r * pa; g += p.g * pa; b += p.b * pa; a += pa;
        }
      RGBA o{0, 0, 0, 0};
      if (a > 0) {
        o.r = uint8_t(std::clamp(r / a, 0.f, 255.f) + 0.5f);
        o.g = uint8_t(std::clamp(g / a, 0.f, 255.f) + 0.5f);
        o.b = uint8_t(std::clamp(b / a, 0.f, 255.f) + 0.5f);
        o.a = uint8_t(std::clamp(a / (bx * by) * 255.f, 0.f, 255.f) + 0.5f);
      }
      dst[size_t(y) * Nw + x] = o;
    }
  return dst;
}

// Solid (alpha-blended) rectangle fill.
void fillRect(std::vector<RGBA>& img, int W, int H, int x0, int y0, int w,
              int h, RGBA c) {
  int x1 = x0 + w, y1 = y0 + h;
  for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
    for (int x = std::max(0, x0); x < std::min(W, x1); ++x)
      img[size_t(y) * W + x] = over(img[size_t(y) * W + x], c, 1.f);
}
// One-pixel line (axis-aligned).
void hLine(std::vector<RGBA>& img, int W, int H, int x0, int x1, int y,
           int thick, RGBA c) {
  for (int t = 0; t < thick; ++t)
    if (y + t >= 0 && y + t < H)
      for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x)
        img[size_t(y + t) * W + x] = over(img[size_t(y + t) * W + x], c, 1.f);
}
void vLine(std::vector<RGBA>& img, int W, int H, int x, int y0, int y1,
           int thick, RGBA c) {
  for (int t = 0; t < thick; ++t)
    if (x + t >= 0 && x + t < W)
      for (int y = std::max(0, y0); y <= std::min(H - 1, y1); ++y)
        img[size_t(y) * W + x + t] = over(img[size_t(y) * W + x + t], c, 1.f);
}
// Anti-aliased filled disc.
void fillDisc(std::vector<RGBA>& img, int W, int H, float cx, float cy,
              float r, RGBA c) {
  int x0 = int(std::floor(cx - r - 1)), x1 = int(std::ceil(cx + r + 1));
  int y0 = int(std::floor(cy - r - 1)), y1 = int(std::ceil(cy + r + 1));
  for (int y = std::max(0, y0); y <= std::min(H - 1, y1); ++y)
    for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      float d  = std::sqrt(dx * dx + dy * dy);
      float a  = std::clamp(r - d + 0.5f, 0.f, 1.f);
      if (a > 0) img[size_t(y) * W + x] = over(img[size_t(y) * W + x], c, a);
    }
}
// Anti-aliased "X" through a rect (illegal-move marker).
void drawCross(std::vector<RGBA>& img, int W, int H, float x, float y,
               float w, float h, float thick, RGBA c) {
  float cx = x + w / 2, cy = y + h / 2;
  float r = std::min(w, h) * 0.35f;
  // Two strokes; each is a "rotated rectangle" - sampled per pixel via
  // segDist.  Cheap enough for the handful of pixels involved.
  auto stroke = [&](float ax, float ay, float bx, float by) {
    int x0 = int(std::floor(std::min(ax, bx) - thick)),
        x1 = int(std::ceil (std::max(ax, bx) + thick));
    int y0 = int(std::floor(std::min(ay, by) - thick)),
        y1 = int(std::ceil (std::max(ay, by) + thick));
    for (int yy = std::max(0, y0); yy <= std::min(H - 1, y1); ++yy)
      for (int xx = std::max(0, x0); xx <= std::min(W - 1, x1); ++xx) {
        float d = segDist(xx + 0.5f, yy + 0.5f, ax, ay, bx, by);
        float a = std::clamp(thick / 2 - d + 0.5f, 0.f, 1.f);
        if (a > 0)
          img[size_t(yy) * W + xx] = over(img[size_t(yy) * W + xx], c, a);
      }
  };
  stroke(cx - r, cy - r, cx + r, cy + r);
  stroke(cx + r, cy - r, cx - r, cy + r);
}

// Bilinear sample of glyph #idx in glyph-local coords (0..g.w, 0..g.h).
float sampleKanji(int idx, float gx, float gy) {
  const GlyphInfo& g = KANJI_GLYPHS[idx];
  int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
  float fx = gx - x0, fy = gy - y0;
  auto px = [&](int x, int y) -> float {
    if (x < 0 || y < 0 || x >= g.w || y >= g.h) return 0.f;
    return GLYPH_PIX[g.pix + size_t(y) * g.w + x] / 255.f;
  };
  float a = px(x0, y0)     * (1 - fx) + px(x0 + 1, y0)     * fx;
  float b = px(x0, y0 + 1) * (1 - fx) + px(x0 + 1, y0 + 1) * fx;
  return a * (1 - fy) + b * fy;
}

// Draw a single shogi piece into `img` at the given rect, in the same style
// as src/ui.cpp's drawPiece() + buildTileTextures().  `up=true` orients the
// pentagon apex toward the top of the image (Black's natural orientation
// when Black sits at the bottom); `up=false` mirrors for White.
void drawPiece(std::vector<RGBA>& img, int W, int H, float x, float y,
               float w, float h, Piece pc, bool up) {
  // Pentagon vertices (matches buildTileTextures): up-pointing or vertically
  // mirrored.  The AA / border widths scale with the design's 128px reference.
  float aa     = 1.3f * (w / 128.f);
  float border = 5.0f * (w / 128.f);
  float mx = 0.10f * w, my = 0.07f * h;
  float L = x + mx, R = x + w - mx, T = y + my, B = y + h - my;
  float sh = (B - T) * 0.34f, in = (R - L) * 0.08f;
  float xs[5]   = {x + w * 0.5f, R, R - in, L + in, L};
  float ysUp[5] = {T, T + sh, B, B, T + sh};
  float ysDn[5] = {B, B - sh, T, T, B - sh};
  const float* ys = up ? ysUp : ysDn;

  RGBA face  = (Co(pc) == BLACK) ? C_FACE_B : C_FACE_W;
  RGBA kanji = Pr(pc) ? C_KANJI_P : C_KANJI;
  int ki = kanjiIndex(pc);
  const GlyphInfo& g = KANJI_GLYPHS[ki];
  float gh  = h * 0.54f;
  float gw  = g.w * (gh / g.h);
  float gcx = x + w / 2.f;
  float gcy = y + h * (up ? 0.55f : 0.45f);

  int x0 = int(std::floor(x)) - 1, x1 = int(std::ceil(x + w)) + 1;
  int y0 = int(std::floor(y)) - 1, y1 = int(std::ceil(y + h)) + 1;
  for (int py = std::max(0, y0); py < std::min(H, y1); ++py)
    for (int px = std::max(0, x0); px < std::min(W, x1); ++px) {
      float fx = px + 0.5f, fy = py + 0.5f, d = 1e9f;
      for (int e = 0; e < 5; ++e)
        d = std::min(d, segDist(fx, fy, xs[e], ys[e],
                                xs[(e + 1) % 5], ys[(e + 1) % 5]));
      float sd = pointInPoly(fx, fy, xs, ys, 5) ? d : -d;
      float oc = std::clamp(0.5f + sd / aa, 0.f, 1.f);
      float ic = std::clamp(0.5f + (sd - border) / aa, 0.f, 1.f);
      if (oc <= 0.f) continue;
      RGBA& p = img[size_t(py) * W + px];
      p = over(p, C_BORDER, oc);
      p = over(p, face, ic);
      // Kanji in face coords, mirrored when facing down.
      float lx = up ? (fx - (gcx - gw / 2)) : ((gcx + gw / 2) - fx);
      float ly = up ? (fy - (gcy - gh / 2)) : ((gcy + gh / 2) - fy);
      float kx = lx / gw * g.w, ky = ly / gh * g.h;
      p = over(p, kanji, sampleKanji(ki, kx, ky) * ic);
    }
}

// --- board renderer --------------------------------------------------------
struct Mark {
  int   square;
  RGBA  color;
};

// Render the 9x9 board into img.  Pieces face outward (Black points up,
// White points down) matching the unflipped in-game orientation.
//
//   highlights  - translucent square overlays (last move, illegal drop, etc.)
//   dots        - small markers for legal destinations (movement diagrams)
//   crosses     - red X markers for illegal squares (nifu / uchifuzume)
void renderBoard(std::vector<RGBA>& img, int W, int H, const Position& pos,
                 const std::vector<Mark>& highlights = {},
                 const std::vector<int>& dots        = {},
                 const std::vector<int>& crosses     = {},
                 bool drawZoneB = false, bool drawZoneW = false) {
  std::fill(img.begin(), img.end(), C_BG);

  float cell = std::min(W, H) / 9.6f;          // 9 cells + a generous margin
  float bx   = (W - 9 * cell) / 2.f;
  float by   = (H - 9 * cell) / 2.f;

  fillRect(img, W, H, int(bx), int(by), int(9 * cell + 0.5f),
           int(9 * cell + 0.5f), C_WOOD);

  // Promotion-zone overlays (rows 0..2 = Black's promo zone, 6..8 = White's).
  if (drawZoneB)
    fillRect(img, W, H, int(bx), int(by), int(9 * cell + 0.5f),
             int(3 * cell + 0.5f), C_ZONE);
  if (drawZoneW)
    fillRect(img, W, H, int(bx), int(by + 6 * cell), int(9 * cell + 0.5f),
             int(3 * cell + 0.5f), C_ZONE);

  // Highlights drawn under pieces.
  for (const Mark& m : highlights) {
    int c = colOf(m.square), r = rowOf(m.square);
    fillRect(img, W, H, int(bx + c * cell), int(by + r * cell),
             int(cell + 1), int(cell + 1), m.color);
  }

  // Grid lines + star points.
  int thick = std::max(1, int(cell / 36 + 0.5f));
  for (int i = 0; i <= 9; ++i) {
    hLine(img, W, H, int(bx), int(bx + 9 * cell),
          int(by + i * cell), thick, C_LINE);
    vLine(img, W, H, int(bx + i * cell), int(by),
          int(by + 9 * cell), thick, C_LINE);
  }
  float sp = std::max(2.f, cell * 0.10f);
  for (int rr : {3, 6})
    for (int cc : {3, 6})
      fillRect(img, W, H, int(bx + cc * cell - sp / 2),
               int(by + rr * cell - sp / 2),
               int(sp + 0.5f), int(sp + 0.5f), C_LINE);

  // Pieces.
  for (int s = 0; s < N_SQ; ++s) {
    Piece pc = pos.board[s];
    if (!pc) continue;
    int c = colOf(s), r = rowOf(s);
    drawPiece(img, W, H, bx + c * cell, by + r * cell, cell, cell, pc,
              Co(pc) == BLACK);
  }

  // Movement dots.
  float dr = cell * 0.14f;
  for (int s : dots) {
    int c = colOf(s), r = rowOf(s);
    fillDisc(img, W, H, bx + c * cell + cell / 2.f,
             by + r * cell + cell / 2.f, dr, C_DOT);
  }
  // Illegal X markers.
  for (int s : crosses) {
    int c = colOf(s), r = rowOf(s);
    drawCross(img, W, H, bx + c * cell, by + r * cell, cell, cell,
              std::max(2.f, cell * 0.10f), C_BAD);
  }
}

// --- movement-diagram targets ---------------------------------------------
struct D { int dr, dc; };
static const D GOLD_S[]   = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,0}};
static const D SILVER_S[] = {{-1,-1},{-1,0},{-1,1},{1,-1},{1,1}};
static const D KING_S[]   = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},
                             {1,-1},{1,0},{1,1}};
static const D ORTHO_S[]  = {{-1,0},{1,0},{0,-1},{0,1}};
static const D DIAG_S[]   = {{-1,-1},{-1,1},{1,-1},{1,1}};

// All squares the piece at `from` could reach on an empty board.  Mirrors
// src/board.cpp's pieceSteps() / pieceRays() / knight inline handling.
std::vector<int> targets(Piece pc, int from) {
  std::vector<int> out;
  int sign = (Co(pc) == BLACK) ? 1 : -1;
  int t = T(pc); bool pr = Pr(pc);
  int r = rowOf(from), c = colOf(from);

  auto in = [](int rr, int cc) {
    return rr >= 0 && rr < 9 && cc >= 0 && cc < 9;
  };
  auto step = [&](D d) {
    int tr = r + d.dr * sign, tc = c + d.dc;
    if (in(tr, tc)) out.push_back(sq(tr, tc));
  };
  auto ray  = [&](D d) {
    int tr = r + d.dr * sign, tc = c + d.dc;
    while (in(tr, tc)) {
      out.push_back(sq(tr, tc));
      tr += d.dr * sign; tc += d.dc;
    }
  };

  if (pr && (t == PT_PAWN || t == PT_LANCE || t == PT_KNIGHT ||
             t == PT_SILVER)) {
    for (auto& d : GOLD_S) step(d);
  } else if (t == PT_GOLD) {
    for (auto& d : GOLD_S) step(d);
  } else if (t == PT_SILVER) {
    for (auto& d : SILVER_S) step(d);
  } else if (t == PT_PAWN) {
    step({-1, 0});
  } else if (t == PT_KING) {
    for (auto& d : KING_S) step(d);
  } else if (t == PT_LANCE && !pr) {
    ray({-1, 0});
  } else if (t == PT_KNIGHT && !pr) {
    // The step tables encode "forward = row decreasing" (Black's view), and
    // the rest of this function multiplies dr by `sign` (+1 Black, -1 White)
    // so the SAME table works for either side.  The knight has no table -
    // its jump is hand-rolled here, so the forward jump is "-2 * sign" to
    // match: Black goes up (r-2), White goes down (r+2).
    for (int dc = -1; dc <= 1; dc += 2) {
      int tr = r - 2 * sign, tc = c + dc;
      if (in(tr, tc)) out.push_back(sq(tr, tc));
    }
  } else if (t == PT_BISHOP) {
    for (auto& d : DIAG_S) ray(d);
    if (pr) for (auto& d : ORTHO_S) step(d);   // Horse
  } else if (t == PT_ROOK) {
    for (auto& d : ORTHO_S) ray(d);
    if (pr) for (auto& d : DIAG_S) step(d);    // Dragon
  }
  return out;
}

// --- output: supersample, downscale, write PNG -----------------------------
void writePng(const char* path, int W, int H,
              const std::vector<RGBA>& px) {
  if (!stbi_write_png(path, W, H, 4, px.data(), W * 4)) {
    std::fprintf(stderr, "stbi_write_png failed for %s\n", path);
    std::exit(1);
  }
  std::printf("%s  (%dx%d)\n", path, W, H);
}

// Render `fn(big, 2W, 2H)` then box-downscale 2->1 and write PNG.
template <class Fn>
void emit(const char* path, int W, int H, Fn fn) {
  int SW = 2 * W, SH = 2 * H;
  std::vector<RGBA> big(size_t(SW) * SH, RGBA{0, 0, 0, 255});
  fn(big, SW, SH);
  std::vector<RGBA> small = downscale(big, SW, SH, W, H);
  writePng(path, W, H, small);
}

// --- piece-card renderer (single piece centred on a neutral card) ---------
void renderPieceCard(std::vector<RGBA>& img, int W, int H, Piece pc) {
  std::fill(img.begin(), img.end(), C_BG);
  // A small wood-tone square behind the piece so it sits in context.
  float pad = W * 0.10f;
  fillRect(img, W, H, int(pad), int(pad), int(W - 2 * pad),
           int(H - 2 * pad), C_WOOD);
  float tile = std::min(W, H) * 0.74f;
  float x    = (W - tile) / 2.f, y = (H - tile) / 2.f;
  drawPiece(img, W, H, x, y, tile, tile, pc, Co(pc) == BLACK);
}

// --- movement-grid renderer (piece on a centred 9x9 with destination dots) -
void renderMovement(std::vector<RGBA>& img, int W, int H, Piece pc) {
  Position p;
  p.board[sq(4, 4)] = pc;
  renderBoard(img, W, H, p, {}, targets(pc, sq(4, 4)));
}

// --- main: emit every tutorial image --------------------------------------
struct PieceEntry { const char* file; Piece pc; };

// Unpromoted (8) - one per type.  King is shown as the Black 王.
const PieceEntry UNPROMOTED[] = {
  {"piece-king.png",   P(BLACK, PT_KING)},
  {"piece-rook.png",   P(BLACK, PT_ROOK)},
  {"piece-bishop.png", P(BLACK, PT_BISHOP)},
  {"piece-gold.png",   P(BLACK, PT_GOLD)},
  {"piece-silver.png", P(BLACK, PT_SILVER)},
  {"piece-knight.png", P(BLACK, PT_KNIGHT)},
  {"piece-lance.png",  P(BLACK, PT_LANCE)},
  {"piece-pawn.png",   P(BLACK, PT_PAWN)},
};
// Promoted (6) - rook/bishop/silver/knight/lance/pawn all gain promo forms.
const PieceEntry PROMOTED[] = {
  {"piece-dragon.png",  P(BLACK, PT_ROOK,   true)},
  {"piece-horse.png",   P(BLACK, PT_BISHOP, true)},
  {"piece-narigin.png", P(BLACK, PT_SILVER, true)},
  {"piece-narikei.png", P(BLACK, PT_KNIGHT, true)},
  {"piece-narikyo.png", P(BLACK, PT_LANCE,  true)},
  {"piece-tokin.png",   P(BLACK, PT_PAWN,   true)},
};

}  // namespace

int main() {
  std::printf("gentut: writing docs/tutorial/img/*.png\n");

  // ---- starting position ---------------------------------------------------
  emit("docs/tutorial/img/start.png", 540, 540,
       [](std::vector<RGBA>& img, int W, int H) {
         renderBoard(img, W, H, initialPosition());
       });

  // ---- piece cards ---------------------------------------------------------
  auto emitCard = [](const char* file, Piece pc) {
    std::string path = std::string("docs/tutorial/img/") + file;
    emit(path.c_str(), 220, 220,
         [pc](std::vector<RGBA>& img, int W, int H) {
           renderPieceCard(img, W, H, pc);
         });
  };
  for (const auto& e : UNPROMOTED) emitCard(e.file, e.pc);
  for (const auto& e : PROMOTED)   emitCard(e.file, e.pc);

  // ---- movement diagrams ---------------------------------------------------
  auto emitMove = [](const char* file, Piece pc) {
    std::string path = std::string("docs/tutorial/img/move-") + file;
    emit(path.c_str(), 480, 480,
         [pc](std::vector<RGBA>& img, int W, int H) {
           renderMovement(img, W, H, pc);
         });
  };
  // Unpromoted: strip "piece-" prefix from the filename.
  for (const auto& e : UNPROMOTED)
    emitMove(std::string(e.file).substr(6).c_str(), e.pc);
  for (const auto& e : PROMOTED)
    emitMove(std::string(e.file).substr(6).c_str(), e.pc);

  // ---- nifu: a black pawn already on file 5, the would-be drop is illegal --
  emit("docs/tutorial/img/nifu.png", 480, 480,
       [](std::vector<RGBA>& img, int W, int H) {
         Position p;
         p.board[sq(6, 4)] = P(BLACK, PT_PAWN);   // unmoved pawn on file 5
         p.hand[BLACK][PT_PAWN] = 1;              // a pawn in hand
         std::vector<Mark> hl{{sq(4, 4), C_BAD}}; // try to drop here
         renderBoard(img, W, H, p, hl, {}, {sq(4, 4)});
       });

  // ---- uchifuzume: pawn drop that would be mate is illegal -----------------
  // The white king at 1a is NOT in check.  Black rook at 2b covers 2a and
  // 1b; black silver at 3c defends the rook.  Dropping a black pawn at 1b
  // (sq(1, 8)) would deliver mate (king can't move, can't capture the
  // pawn defended by the rook, can't capture the rook defended by the
  // silver) - so the drop is illegal under uchifuzume.
  emit("docs/tutorial/img/uchifuzume.png", 480, 480,
       [](std::vector<RGBA>& img, int W, int H) {
         Position p;
         p.board[sq(0, 8)] = P(WHITE, PT_KING);
         p.board[sq(1, 7)] = P(BLACK, PT_ROOK);
         p.board[sq(2, 6)] = P(BLACK, PT_SILVER);
         p.hand[BLACK][PT_PAWN] = 1;
         std::vector<Mark> hl{{sq(1, 8), C_BAD}};
         renderBoard(img, W, H, p, hl, {}, {sq(1, 8)});
       });

  // ---- promotion zone: both shaded; show a black pawn ready to promote ----
  emit("docs/tutorial/img/promotion-zone.png", 480, 480,
       [](std::vector<RGBA>& img, int W, int H) {
         Position p;
         p.board[sq(3, 4)] = P(BLACK, PT_PAWN);  // about to step into row 2
         renderBoard(img, W, H, p, {}, {}, {}, true, true);
       });

  // ---- a clean back-rank lance mate: black lance at 1h checks the white
  //      king at 1a along file 1; black gold at 3b covers both flight
  //      squares (2a, 2b).  Lance attack on 1b means the king can't even
  //      capture the gold by stepping into the corner.  Three pieces, no
  //      extras - the position reads as a finished mate at a glance.
  emit("docs/tutorial/img/mate-example.png", 480, 480,
       [](std::vector<RGBA>& img, int W, int H) {
         Position p;
         p.board[sq(0, 8)] = P(WHITE, PT_KING);
         p.board[sq(7, 8)] = P(BLACK, PT_LANCE);
         p.board[sq(1, 6)] = P(BLACK, PT_GOLD);
         std::vector<Mark> hl{{sq(0, 8), C_BAD}};
         renderBoard(img, W, H, p, hl);
       });

  // ---- sennichite (perpetual check by horse).  Worked-out cycle:
  //        T0 : K@1a, Horse@3c (giving check).  K's only flight: 2a.
  //        T1 : K@2a, Horse@3c (no check).      Black plays Horse-3b.
  //        T2 : K@2a, Horse@3b (giving check).  K's only flight: 1a.
  //        T3 : K@1a, Horse@3b (no check).      Black plays Horse-3c.
  //        T4 = T0.
  //      The pawn at 1b blocks the king's south escape; the lance at 3f
  //      defends the horse on its 3b square (preventing K from capturing
  //      it).  Black gives check on every other ply; after four cycles
  //      the same position has recurred four times - because black is the
  //      one forcing the checks, the perpetual-check rule means black
  //      loses (not a draw).  This is the diagrammed moment, T0.
  emit("docs/tutorial/img/sennichite.png", 480, 480,
       [](std::vector<RGBA>& img, int W, int H) {
         Position p;
         p.board[sq(0, 8)] = P(WHITE, PT_KING);
         p.board[sq(1, 8)] = P(WHITE, PT_PAWN);
         p.board[sq(2, 6)] = P(BLACK, PT_BISHOP, true);   // horse at 3c
         p.board[sq(5, 6)] = P(BLACK, PT_LANCE);          // lance at 3f
         std::vector<Mark> hl{{sq(0, 8), C_BAD}};         // K is in check
         renderBoard(img, W, H, p, hl);
       });

  // ---- a short opening sequence ending in a bishop exchange, the
  //      classic illustration of why pieces in hand matter.
  //        1. P-7f       black opens its bishop diagonal
  //        1...P-3d      white opens its own
  //        2. Bx2b+      black bishop captures the white bishop, promoting
  //                      to a horse on the way in - both sides' diagonals
  //                      now run clear from one bishop's starting square
  //                      to the other
  //        2...Sx2b      white silver from 3a recaptures the horse
  //      After 2...Sx2b each player has a bishop in hand (the captured
  //      piece reverts to its unpromoted form), making "drops" the main
  //      strategic question for the rest of the opening.
  auto emitOpening = [](int idx, const Position& p, int lastSq) {
    std::string path = "docs/tutorial/img/opening-" + std::to_string(idx) +
                       ".png";
    emit(path.c_str(), 480, 480,
         [&p, lastSq](std::vector<RGBA>& img, int W, int H) {
           std::vector<Mark> hl;
           if (lastSq >= 0) hl.push_back({lastSq, C_LAST});
           renderBoard(img, W, H, p, hl);
         });
  };
  {
    Position p = initialPosition();
    emitOpening(1, p, -1);
    mv(p, sq(6, 2), sq(5, 2));            // 1. P-7f
    emitOpening(2, p, sq(5, 2));
    mv(p, sq(2, 6), sq(3, 6));            // 1...P-3d
    emitOpening(3, p, sq(3, 6));
    mv(p, sq(7, 1), sq(1, 7), true);      // 2. Bx2b+  (bishop captures + promo)
    emitOpening(4, p, sq(1, 7));
    mv(p, sq(0, 6), sq(1, 7));            // 2...Sx2b  (silver recaptures)
    emitOpening(5, p, sq(1, 7));
  }

  // ---- mino castle (Black, Fourth-file Rook).  Five frames, each showing
  //      one structural step of the build.  White is omitted entirely from
  //      these diagrams because White's responses would clutter the picture
  //      with pieces that never move between frames.  Last move highlighted.
  auto emitMino = [](int idx, const Position& p, int lastSq) {
    std::string path = "docs/tutorial/img/mino-" + std::to_string(idx) +
                       ".png";
    emit(path.c_str(), 480, 480,
         [&p, lastSq](std::vector<RGBA>& img, int W, int H) {
           std::vector<Mark> hl;
           if (lastSq >= 0) hl.push_back({lastSq, C_LAST});
           renderBoard(img, W, H, p, hl);
         });
  };
  {
    // Frame 1: Black's starting setup.
    Position p = blackOnlyInitial();
    emitMino(1, p, -1);

    // Frame 2: R-6h - rook swings from 2h (sq(7,7)) over to 6h (sq(7,3)),
    // freeing the right side of the board for the king to castle into.
    mv(p, sq(7, 7), sq(7, 3));        // R-6h
    emitMino(2, p, sq(7, 3));

    // Frame 3: king migrates 5i -> 4h -> 3h -> 2h (three single-square
    // steps in a real game; shown here as a completed trip for clarity).
    p.board[sq(8, 4)] = 0;
    p.board[sq(7, 7)] = P(BLACK, PT_KING);
    emitMino(3, p, sq(7, 7));

    // Frame 4: S-3h then G-5h complete the Mino's shield - king tucked at
    // 2h, silver guarding 3h, both golds covering 49 (unchanged) and 58.
    mv(p, sq(8, 6), sq(7, 6));        // S-3h
    mv(p, sq(8, 3), sq(7, 4));        // G-5h (from 69, not 49)
    p.side = BLACK;
    emitMino(4, p, sq(7, 6));

    // Frame 5: P-1f - push the lance pawn forward one square.  This gives
    // the king an escape route at 1g and is a standard finishing touch on
    // any Mino-style castle.
    mv(p, sq(6, 8), sq(5, 8));        // P-1f
    p.side = BLACK;
    emitMino(5, p, sq(5, 8));
  }

  // ---- climbing silver: the file-3 silver marches up the rook side to
  //      support an attack on White's defences along file 2.  Five frames,
  //      each one Black move further; White's responses are elided.
  auto emitClimb = [](int idx, const Position& p, int lastSq) {
    std::string path = "docs/tutorial/img/climbing-silver-" +
                       std::to_string(idx) + ".png";
    emit(path.c_str(), 480, 480,
         [&p, lastSq](std::vector<RGBA>& img, int W, int H) {
           std::vector<Mark> hl;
           if (lastSq >= 0) hl.push_back({lastSq, C_LAST});
           renderBoard(img, W, H, p, hl);
         });
  };
  {
    // Black-only board for the same reason as the Mino sequence: White
    // doesn't visibly move between these frames, so showing White is
    // distracting.
    Position p = blackOnlyInitial();

    // Frame 1: P-2f - the rook pawn opens the attacking file.
    mv(p, sq(6, 7), sq(5, 7));        // P-2f
    p.side = BLACK;
    emitClimb(1, p, sq(5, 7));

    // Frame 2: P-2e - the same pawn advances another rank, claiming space.
    mv(p, sq(5, 7), sq(4, 7));        // P-2e
    p.side = BLACK;
    emitClimb(2, p, sq(4, 7));

    // Frame 3: S-3h - the silver leaves the back rank to begin climbing.
    mv(p, sq(8, 6), sq(7, 6));        // S-3h
    emitClimb(3, p, sq(7, 6));

    // Frame 4: S-2g - silver moves diagonally up the rook's file.
    mv(p, sq(7, 6), sq(6, 7));        // S-2g
    p.side = BLACK;
    emitClimb(4, p, sq(6, 7));

    // Frame 5: S-2f - silver sits behind its own pawn at 2e, ready to push
    // through.  In a real game the pawn at 2e is sacrificed next, and the
    // silver recaptures to break White's defence on file 2.
    mv(p, sq(6, 7), sq(5, 7));        // S-2f
    p.side = BLACK;
    emitClimb(5, p, sq(5, 7));
  }

  std::printf("done.\n");
  return 0;
}
