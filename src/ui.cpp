// ui.cpp - SDL3 user interface and game flow.  This is the
// SDL_MAIN_USE_CALLBACKS translation unit: the SDL_App* entry points at the
// foot of the file replace a conventional main(), and SDL owns the main loop
// (a plain loop natively, the browser's animation-frame callback on the web).
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "board.hpp"
#include "engine.hpp"
#include "glyphs.hpp"
#include "icon.hpp"

namespace shogi {
namespace {

// --- Layout -----------------------------------------------------------------
// Packed left to right: win-prob bar | hand | board | hand | win-prob bar.
// The hand columns are deliberately narrow so the window stays close to
// square, which fits mobile screens far better.
constexpr int WIN_W = 880, WIN_H = 720;
constexpr int CELL = 58;
constexpr int BOARD_X = 179, BOARD_Y = 96;
constexpr int BOARD_PX = 9 * CELL;
constexpr int HAND_W = 116, HAND_EH = 74;
constexpr int WHITE_HAND_X = 44, BLACK_HAND_X = 714;
constexpr int WBAR_X = 18, BBAR_X = 846, BAR_W = 16;
// Computer move timing: think at least MIN_THINK_MS, then until MIN_PLAYOUTS
// playouts have been searched from the position, but never past MAX_THINK_MS.
// On a fast machine MIN_THINK_MS governs; on a slow one MAX_THINK_MS does.
// MIN_PLAYOUTS is matched to MAX_NODES - search saturates once the tree fills.
constexpr int    MIN_PLAYOUTS = 700000;
constexpr Uint64 MIN_THINK_MS = 4000;
constexpr Uint64 MAX_THINK_MS = 10000;

struct RGBA { Uint8 r, g, b, a; };

const RGBA C_BG       {26, 27, 32, 255};
const RGBA C_WOOD     {222, 184, 120, 255};
const RGBA C_LINE     {48, 34, 16, 255};
const RGBA C_PANEL    {44, 46, 54, 255};
const RGBA C_TEXT     {232, 230, 224, 255};
const RGBA C_DIM      {150, 150, 158, 255};
const RGBA C_BTN      {66, 70, 84, 255};
const RGBA C_BTN_HOT  {96, 120, 160, 255};
const RGBA C_SEL      {250, 224, 96, 200};
const RGBA C_LAST     {120, 170, 110, 150};
const RGBA C_DOT      {40, 90, 200, 190};
const RGBA C_SUGGEST  {235, 130, 40, 255};
const RGBA C_BAR_B    {226, 162, 60, 255};
const RGBA C_BAR_W    {206, 212, 224, 255};

// --- Small drawing helpers --------------------------------------------------
void setColor(SDL_Renderer* r, RGBA c) {
  SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
void fillRect(SDL_Renderer* r, float x, float y, float w, float h, RGBA c) {
  setColor(r, c);
  SDL_FRect q{x, y, w, h};
  SDL_RenderFillRect(r, &q);
}
void outlineRect(SDL_Renderer* r, float x, float y, float w, float h, RGBA c) {
  setColor(r, c);
  SDL_FRect q{x, y, w, h};
  SDL_RenderRect(r, &q);
}
// Mouse position in logical (render) coordinates - the window may be a
// different size, so window pixels must be converted.
void mousePos(SDL_Renderer* r, float& x, float& y) {
  float wx = 0, wy = 0;
  SDL_GetMouseState(&wx, &wy);
  SDL_RenderCoordinatesFromWindow(r, wx, wy, &x, &y);
}
// --- Text: anti-aliased glyph textures built once from glyphs.hpp -----------
SDL_Texture* g_ascii[95] = {};
SDL_Texture* g_kanji[15] = {};

// Build a white texture whose alpha channel is the glyph's coverage, so it can
// be tinted to any colour with SDL_SetTextureColorMod.
SDL_Texture* makeGlyphTex(SDL_Renderer* r, const GlyphInfo& g) {
  if (g.w <= 0 || g.h <= 0) return nullptr;          // e.g. the space glyph
  SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC, g.w, g.h);
  if (!t) return nullptr;
  std::vector<uint8_t> px(size_t(g.w) * g.h * 4);
  for (int i = 0; i < g.w * g.h; ++i) {
    px[i * 4 + 0] = 255;
    px[i * 4 + 1] = 255;
    px[i * 4 + 2] = 255;
    px[i * 4 + 3] = GLYPH_PIX[g.pix + i];            // coverage -> alpha
  }
  SDL_UpdateTexture(t, nullptr, px.data(), g.w * 4);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
  return t;
}
void buildGlyphs(SDL_Renderer* r) {
  for (int i = 0; i < 95; ++i) g_ascii[i] = makeGlyphTex(r, ASCII_GLYPHS[i]);
  for (int i = 0; i < 15; ++i) g_kanji[i] = makeGlyphTex(r, KANJI_GLYPHS[i]);
}

// Width of `s` rendered at pixel height `px`.
int textW(const std::string& s, int px) {
  float k = float(px) / ASCII_PX, w = 0;
  for (char ch : s) {
    int i = (ch >= 0x20 && ch <= 0x7E) ? ch - 0x20 : ('?' - 0x20);
    w += ASCII_GLYPHS[i].advance * k;
  }
  return int(w + 0.5f);
}

// Draw `s` with the top-left of its line box at (x, y), glyphs `px` px tall.
void drawText(SDL_Renderer* r, int x, int y, int px, const std::string& s,
              RGBA c) {
  float k = float(px) / ASCII_PX;
  float pen = float(x), baseline = y + ASCII_ASCENT * k;
  for (char ch : s) {
    int i = (ch >= 0x20 && ch <= 0x7E) ? ch - 0x20 : ('?' - 0x20);
    const GlyphInfo& g = ASCII_GLYPHS[i];
    if (g_ascii[i]) {
      SDL_SetTextureColorMod(g_ascii[i], c.r, c.g, c.b);
      SDL_SetTextureAlphaMod(g_ascii[i], c.a);
      SDL_FRect dst{pen + g.xoff * k, baseline + g.yoff * k, g.w * k, g.h * k};
      SDL_RenderTexture(r, g_ascii[i], nullptr, &dst);
    }
    pen += g.advance * k;
  }
}
void drawTextC(SDL_Renderer* r, int cx, int y, int px, const std::string& s,
               RGBA c) {
  drawText(r, cx - textW(s, px) / 2, y, px, s, c);
}

void fillPoly(SDL_Renderer* r, const SDL_FPoint* pts, int n, RGBA c) {
  SDL_FColor fc{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
  std::vector<SDL_Vertex> v(n);
  for (int i = 0; i < n; ++i) {
    v[i].position = pts[i];
    v[i].color = fc;
    v[i].tex_coord = SDL_FPoint{0, 0};
  }
  std::vector<int> idx;
  for (int i = 1; i + 1 < n; ++i) {
    idx.push_back(0);
    idx.push_back(i);
    idx.push_back(i + 1);
  }
  SDL_RenderGeometry(r, nullptr, v.data(), n, idx.data(), int(idx.size()));
}
void fillDisc(SDL_Renderer* r, float cx, float cy, float rad, RGBA c) {
  SDL_FPoint p[16];
  for (int i = 0; i < 16; ++i) {
    float a = float(i) / 16.f * 6.2831853f;
    p[i] = SDL_FPoint{cx + std::cos(a) * rad, cy + std::sin(a) * rad};
  }
  fillPoly(r, p, 16, c);
}

// Index into KANJI_GLYPHS for a piece (see tools/genfont.cpp for the order).
int kanjiIndex(Piece pc) {
  PieceType t = typeOf(pc);
  if (isPromoted(pc)) {
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
    case PT_KING:   return colorOf(pc) == BLACK ? 7 : 8;   // 王 / 玉
    default:        return 0;
  }
}

// --- Piece tiles: anti-aliased pentagon textures built once at startup ------
SDL_Texture* g_tileOuter = nullptr;    // full pentagon  (border colour)
SDL_Texture* g_tileInner = nullptr;    // inset pentagon (face colour)

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
// Build the pentagon silhouette and inset-face masks from a signed distance
// field, so both the outline and the border edge are anti-aliased.  The masks
// are downscaled when drawn, which keeps the edges smooth.
void buildTileTextures(SDL_Renderer* r) {
  const int S = 128;
  const float border = 5.0f, aa = 1.3f;             // in tile pixels
  float mx = 0.10f * S, my = 0.07f * S;
  float L = mx, R = S - mx, T = my, B = S - my;
  float sh = (B - T) * 0.34f, in = (R - L) * 0.08f;
  float xs[5] = {S * 0.5f, R, R - in, L + in, L};   // up-pointing pentagon
  float ys[5] = {T, T + sh, B, B, T + sh};
  std::vector<uint8_t> outer(size_t(S) * S * 4), inner(size_t(S) * S * 4);
  for (int yy = 0; yy < S; ++yy)
    for (int xx = 0; xx < S; ++xx) {
      float fx = xx + 0.5f, fy = yy + 0.5f, d = 1e9f;
      for (int e = 0; e < 5; ++e)
        d = std::min(d, segDist(fx, fy, xs[e], ys[e], xs[(e + 1) % 5],
                                ys[(e + 1) % 5]));
      float sd = pointInPoly(fx, fy, xs, ys, 5) ? d : -d;   // + inside
      float oc = std::clamp(0.5f + sd / aa, 0.f, 1.f);
      float ic = std::clamp(0.5f + (sd - border) / aa, 0.f, 1.f);
      int i = (yy * S + xx) * 4;
      outer[i] = outer[i + 1] = outer[i + 2] = 255;
      inner[i] = inner[i + 1] = inner[i + 2] = 255;
      outer[i + 3] = uint8_t(oc * 255 + 0.5f);
      inner[i + 3] = uint8_t(ic * 255 + 0.5f);
    }
  auto mk = [&](const std::vector<uint8_t>& d) {
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, S, S);
    SDL_UpdateTexture(t, nullptr, d.data(), S * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
  };
  g_tileOuter = mk(outer);
  g_tileInner = mk(inner);
}

// Draw a shogi piece: an anti-aliased pentagon tile pointing toward the enemy,
// with the piece kanji (red when promoted).
void drawPiece(SDL_Renderer* r, float x, float y, float w, float h, Piece pc,
               bool selected, bool flipped) {
  bool up = (colorOf(pc) == BLACK) != flipped;   // bottom player faces the viewer
  float cx = x + w / 2.f;
  SDL_FRect dst{x, y, w, h};
  SDL_FlipMode flip = up ? SDL_FLIP_NONE : SDL_FLIP_VERTICAL;
  RGBA face = selected ? RGBA{252, 236, 132, 255}
              : up ? RGBA{244, 212, 150, 255} : RGBA{236, 226, 200, 255};
  SDL_SetTextureColorMod(g_tileOuter, 62, 44, 18);          // border
  SDL_RenderTextureRotated(r, g_tileOuter, nullptr, &dst, 0.0, nullptr, flip);
  SDL_SetTextureColorMod(g_tileInner, face.r, face.g, face.b);
  SDL_RenderTextureRotated(r, g_tileInner, nullptr, &dst, 0.0, nullptr, flip);

  // Kanji glyph, in red when promoted.  White's pieces face the other way, so
  // the glyph is rotated 180 degrees - just like a real shogi board.
  RGBA col = isPromoted(pc) ? RGBA{176, 36, 24, 255} : RGBA{38, 26, 12, 255};
  int ki = kanjiIndex(pc);
  if (g_kanji[ki]) {
    const GlyphInfo& g = KANJI_GLYPHS[ki];
    float gh = h * 0.54f;                  // kanji ~54% of tile height: a
                                           // little inset so wide glyphs
                                           // (飛, 銀 ...) clear the edges
    float gw = g.w * (gh / g.h);
    float gcx = cx, gcy = y + h * (up ? 0.55f : 0.45f);  // toward the wide end
    SDL_FRect dst{gcx - gw / 2, gcy - gh / 2, gw, gh};
    SDL_SetTextureColorMod(g_kanji[ki], col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(g_kanji[ki], 255);
    if (up)
      SDL_RenderTexture(r, g_kanji[ki], nullptr, &dst);
    else
      SDL_RenderTextureRotated(r, g_kanji[ki], nullptr, &dst, 180.0, nullptr,
                               SDL_FLIP_NONE);
  }
}

// --- Buttons ----------------------------------------------------------------
struct Button {
  SDL_FRect rect;
  std::string label;
  bool hit(float mx, float my) const {
    return mx >= rect.x && mx < rect.x + rect.w && my >= rect.y &&
           my < rect.y + rect.h;
  }
};
void drawButton(SDL_Renderer* r, const Button& b, bool highlight) {
  fillRect(r, b.rect.x, b.rect.y, b.rect.w, b.rect.h,
           highlight ? C_BTN_HOT : C_BTN);
  outlineRect(r, b.rect.x, b.rect.y, b.rect.w, b.rect.h, RGBA{20, 22, 28, 255});
  int px = 24;
  drawTextC(r, int(b.rect.x + b.rect.w / 2),
            int(b.rect.y + b.rect.h / 2 - px * 0.43f), px, b.label, C_TEXT);
}

// ---------------------------------------------------------------------------
// The application / game object.
// ---------------------------------------------------------------------------
enum Screen { SCR_MENU, SCR_PLAY };
enum Mode { MODE_HVH, MODE_HVC, MODE_CVH, MODE_CVC };

class App {
 public:
  ~App();                                // stop the engine, tear down SDL
  bool init();                           // window/renderer/assets; false => fail
  SDL_AppResult iterate();               // advance + render one frame
  SDL_AppResult onEvent(SDL_Event& e);   // handle one input event

 private:
  void startGame(Mode m);
  void recomputeLegal();
  void updateResult();
  void applyMove(const Move& m);
  bool playerIsComputer(Color c) const;

  void onClick(float mx, float my);
  void onMenuClick(float mx, float my);
  void onBoardClick(int square);

  void render();
  void renderMenu();
  void renderBoard();
  void renderHands();
  void renderBars(const MCTS::Stats& st);
  void renderStatus(const MCTS::Stats& st);
  void renderPromoDialog();
  void renderResult();

  void syncCanvasSize();               // web: fit canvas to viewport (no-op native)

  int squareAt(float mx, float my) const;       // -1 if outside board
  SDL_FRect handEntry(Color c, int idx) const;

  SDL_Renderer* ren_ = nullptr;
  SDL_Window* win_ = nullptr;
  Engine engine_;

  Screen screen_ = SCR_MENU;
  Mode mode_ = MODE_HVC;
  bool running_ = true;
  int  webCssW_ = 0, webCssH_ = 0;     // last canvas CSS size synced (web)
  bool human_[2] = {true, true};
  bool flipped_ = false;               // CVH: orient with White at the bottom

  Position pos_;
  std::vector<uint64_t> hashes_;
  std::vector<Move> legal_;
  Move lastMove_ = nullMove();
  Result result_ = ONGOING;

  int selSq_ = -1;                 // selected board square (-1 none)
  PieceType selHand_ = PT_NONE;    // selected hand piece type
  bool promoDialog_ = false;
  int pendFrom_ = 0, pendTo_ = 0;
  bool showSuggest_ = true;

  bool thinking_ = false;
  Uint64 thinkStart_ = 0;

  std::vector<Button> menuButtons_;
  Button newGameBtn_, suggestBtn_, promoYes_, promoNo_;
};

bool App::playerIsComputer(Color c) const { return !human_[c]; }

void App::recomputeLegal() { generateLegalMoves(pos_, legal_); }

void App::updateResult() {
  if (legal_.empty()) {                       // checkmate or stalemate
    result_ = (pos_.stm() == BLACK) ? WHITE_WIN : BLACK_WIN;
    return;
  }
  int reps = 0;
  for (uint64_t h : hashes_)
    if (h == pos_.hash) ++reps;
  if (reps >= 4 || pos_.ply >= 512) {
    result_ = DRAW;
    return;
  }
  result_ = ONGOING;
}

void App::applyMove(const Move& m) {
  lastMove_ = m;
  doMove(pos_, m);
  hashes_.push_back(pos_.hash);
  selSq_ = -1;
  selHand_ = PT_NONE;
  promoDialog_ = false;
  thinking_ = false;
  recomputeLegal();
  updateResult();
  if (result_ == ONGOING)
    engine_.advance(m, pos_, hashes_);   // reuse the subtree pondered for `m`
  else
    engine_.stop();
}

void App::startGame(Mode m) {
  mode_ = m;
  human_[BLACK] = (m == MODE_HVH || m == MODE_HVC);
  human_[WHITE] = (m == MODE_HVH || m == MODE_CVH);
  flipped_ = (m == MODE_CVH);           // human plays White: White on the bottom
  pos_ = initialPosition();
  hashes_.clear();
  hashes_.push_back(pos_.hash);
  lastMove_ = nullMove();
  result_ = ONGOING;
  selSq_ = -1;
  selHand_ = PT_NONE;
  promoDialog_ = false;
  thinking_ = false;
  recomputeLegal();
  engine_.setPosition(pos_, hashes_);
  screen_ = SCR_PLAY;
}

// --- Geometry ---------------------------------------------------------------
int App::squareAt(float mx, float my) const {
  if (mx < BOARD_X || mx >= BOARD_X + BOARD_PX || my < BOARD_Y ||
      my >= BOARD_Y + BOARD_PX)
    return -1;
  int col = int((mx - BOARD_X) / CELL);
  int row = int((my - BOARD_Y) / CELL);
  if (flipped_) { col = 8 - col; row = 8 - row; }
  return sq(row, col);
}
SDL_FRect App::handEntry(Color c, int idx) const {
  // The bottom player's hand sits on the right; default is Black on bottom.
  int x = ((c == WHITE) != flipped_) ? WHITE_HAND_X : BLACK_HAND_X;
  return SDL_FRect{float(x), float(BOARD_Y + idx * HAND_EH), float(HAND_W),
                   float(HAND_EH - 10)};
}

// --- Input ------------------------------------------------------------------
void App::onMenuClick(float mx, float my) {
  for (size_t i = 0; i < menuButtons_.size(); ++i)
    if (menuButtons_[i].hit(mx, my)) {
      startGame(Mode(i));
      return;
    }
}

void App::onBoardClick(int s) {
  Color me = pos_.stm();
  Piece here = pos_.board[s];

  // Completing a drop.
  if (selHand_ != PT_NONE) {
    for (const Move& m : legal_)
      if (m.isDrop() && m.drop == selHand_ && m.to == s) {
        applyMove(m);
        return;
      }
    selHand_ = PT_NONE;
    if (here && colorOf(here) == me) selSq_ = s;
    return;
  }
  // Selecting a piece.
  if (selSq_ < 0) {
    if (here && colorOf(here) == me) selSq_ = s;
    return;
  }
  if (s == selSq_) {                       // click again to deselect
    selSq_ = -1;
    return;
  }
  // Completing a board move.
  std::vector<Move> match;
  for (const Move& m : legal_)
    if (!m.isDrop() && m.from == selSq_ && m.to == s) match.push_back(m);
  if (match.empty()) {
    selSq_ = (here && colorOf(here) == me) ? s : -1;
    return;
  }
  if (match.size() == 1) {
    applyMove(match[0]);
  } else {                                 // optional promotion -> ask
    promoDialog_ = true;
    pendFrom_ = selSq_;
    pendTo_ = s;
  }
}

void App::onClick(float mx, float my) {
  if (screen_ == SCR_MENU) {
    onMenuClick(mx, my);
    return;
  }
  // Promotion dialog is modal.
  if (promoDialog_) {
    if (promoYes_.hit(mx, my))
      applyMove(boardMove(pendFrom_, pendTo_, true));
    else if (promoNo_.hit(mx, my))
      applyMove(boardMove(pendFrom_, pendTo_, false));
    return;
  }
  if (newGameBtn_.hit(mx, my)) {
    engine_.stop();
    screen_ = SCR_MENU;
    return;
  }
  if (suggestBtn_.hit(mx, my)) {
    showSuggest_ = !showSuggest_;
    return;
  }
  if (result_ != ONGOING) return;
  if (playerIsComputer(pos_.stm())) return;       // not a human's turn

  int s = squareAt(mx, my);
  if (s >= 0) {
    onBoardClick(s);
    return;
  }
  // Hand clicks (only the side to move, only a human).
  for (int c = 0; c < 2; ++c) {
    for (int i = 0; i < 7; ++i) {
      SDL_FRect rc = handEntry(Color(c), i);
      if (mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h) {
        PieceType t = PieceType(PT_PAWN + i);
        if (Color(c) == pos_.stm() && pos_.hand[c][t] > 0) {
          selHand_ = t;
          selSq_ = -1;
        }
        return;
      }
    }
  }
}

// --- Rendering --------------------------------------------------------------
void App::renderMenu() {
  drawTextC(ren_, WIN_W / 2, 100, 46, "SHOGI", C_TEXT);
  drawTextC(ren_, WIN_W / 2, 188, 17,
            "MULTITHREADED MONTE CARLO TREE SEARCH", C_DIM);
  float mx, my;
  mousePos(ren_, mx, my);
  for (const Button& b : menuButtons_)
    drawButton(ren_, b, b.hit(mx, my));
  drawTextC(ren_, WIN_W / 2, 624, 17,
            "ENGINE THREADS  " + std::to_string(engine_.threadCount()), C_DIM);
  drawTextC(ren_, WIN_W / 2, 660, 17, "CLICK A MODE TO BEGIN", C_DIM);
}

void App::renderBoard() {
  fillRect(ren_, BOARD_X, BOARD_Y, BOARD_PX, BOARD_PX, C_WOOD);

  // Highlight last move and current selection.
  auto squareRect = [this](int s) {
    int c = colOf(s), r = rowOf(s);
    if (flipped_) { c = 8 - c; r = 8 - r; }
    return SDL_FRect{float(BOARD_X + c * CELL), float(BOARD_Y + r * CELL),
                     float(CELL), float(CELL)};
  };
  if (!lastMove_.isNull()) {
    SDL_FRect t = squareRect(lastMove_.to);
    fillRect(ren_, t.x, t.y, t.w, t.h, C_LAST);
    if (!lastMove_.isDrop()) {
      SDL_FRect f = squareRect(lastMove_.from);
      fillRect(ren_, f.x, f.y, f.w, f.h, C_LAST);
    }
  }
  if (selSq_ >= 0) {
    SDL_FRect s = squareRect(selSq_);
    fillRect(ren_, s.x, s.y, s.w, s.h, C_SEL);
  }
  // Grid lines.
  setColor(ren_, C_LINE);
  for (int i = 0; i <= 9; ++i) {
    SDL_RenderLine(ren_, BOARD_X + i * CELL, BOARD_Y, BOARD_X + i * CELL,
                   BOARD_Y + BOARD_PX);
    SDL_RenderLine(ren_, BOARD_X, BOARD_Y + i * CELL, BOARD_X + BOARD_PX,
                   BOARD_Y + i * CELL);
  }
  for (int rr : {3, 6})
    for (int cc : {3, 6})
      fillRect(ren_, BOARD_X + cc * CELL - 3, BOARD_Y + rr * CELL - 3, 6, 6,
               C_LINE);

  // Pieces.
  for (int s = 0; s < N_SQ; ++s) {
    Piece pc = pos_.board[s];
    if (!pc) continue;
    SDL_FRect rc = squareRect(s);
    drawPiece(ren_, rc.x, rc.y, rc.w, rc.h, pc, s == selSq_, flipped_);
  }
  // Legal-destination dots for the current selection.
  std::vector<int> targets;
  for (const Move& m : legal_) {
    if (selSq_ >= 0 && !m.isDrop() && m.from == selSq_) targets.push_back(m.to);
    if (selHand_ != PT_NONE && m.isDrop() && m.drop == selHand_)
      targets.push_back(m.to);
  }
  for (int s : targets) {
    SDL_FRect rc = squareRect(s);
    fillDisc(ren_, rc.x + CELL / 2.f, rc.y + CELL / 2.f, 8.f, C_DOT);
  }
  // Engine suggestion for a human to move.
  if (showSuggest_ && result_ == ONGOING && !playerIsComputer(pos_.stm())) {
    MCTS::Stats st = engine_.stats();
    if (!st.bestMove.isNull()) {
      SDL_FRect t = squareRect(st.bestMove.to);
      setColor(ren_, C_SUGGEST);
      for (int o = 0; o < 3; ++o) {
        SDL_FRect ring{t.x + o, t.y + o, t.w - 2 * o, t.h - 2 * o};
        SDL_RenderRect(ren_, &ring);
      }
      if (!st.bestMove.isDrop()) {
        SDL_FRect f = squareRect(st.bestMove.from);
        outlineRect(ren_, f.x + 1, f.y + 1, f.w - 2, f.h - 2, C_SUGGEST);
      }
    }
  }
}

void App::renderHands() {
  static const char* names[2] = {"BLACK HAND", "WHITE HAND"};
  for (int c = 0; c < 2; ++c) {
    Color col = Color(c);
    int x = ((col == WHITE) != flipped_) ? WHITE_HAND_X : BLACK_HAND_X;
    drawTextC(ren_, x + HAND_W / 2, BOARD_Y - 30, 17,
              names[c == BLACK ? 0 : 1], C_DIM);
    for (int i = 0; i < 7; ++i) {
      PieceType t = PieceType(PT_PAWN + i);
      SDL_FRect rc = handEntry(col, i);
      bool sel = (selHand_ == t && pos_.stm() == col);
      fillRect(ren_, rc.x, rc.y, rc.w, rc.h, sel ? C_BTN_HOT : C_PANEL);
      outlineRect(ren_, rc.x, rc.y, rc.w, rc.h, RGBA{20, 22, 28, 255});
      Piece pc = makePiece(col, t, false);
      drawPiece(ren_, rc.x + 6, rc.y + 2, 56, 56, pc, false, flipped_);
      int n = pos_.hand[c][t];
      RGBA tc = n ? C_TEXT : C_DIM;
      // Count centred in the space to the right of the 56px piece glyph.
      drawTextC(ren_, int(rc.x) + (62 + int(rc.w)) / 2, int(rc.y) + 17, 26,
                "x" + std::to_string(n), tc);
    }
  }
}

void App::renderBars(const MCTS::Stats& st) {
  double pb = st.blackWinProb;
  // The bottom player's bar sits on the right; default is Black on bottom.
  struct B { int x; double frac; RGBA col; const char* tag; } bars[2] = {
      {flipped_ ? BBAR_X : WBAR_X, 1.0 - pb, C_BAR_W, "W"},
      {flipped_ ? WBAR_X : BBAR_X, pb, C_BAR_B, "B"}};
  for (const B& b : bars) {
    fillRect(ren_, b.x, BOARD_Y, BAR_W, BOARD_PX, C_PANEL);
    int h = int(BOARD_PX * std::clamp(b.frac, 0.0, 1.0));
    fillRect(ren_, b.x, BOARD_Y + BOARD_PX - h, BAR_W, h, b.col);
    outlineRect(ren_, b.x, BOARD_Y, BAR_W, BOARD_PX, RGBA{20, 22, 28, 255});
    drawTextC(ren_, b.x + BAR_W / 2, BOARD_Y - 24, 16, b.tag, C_DIM);
    int pct = int(b.frac * 100.0 + 0.5);
    drawTextC(ren_, b.x + BAR_W / 2, BOARD_Y + BOARD_PX + 6, 16,
              std::to_string(pct), C_DIM);
  }
}

void App::renderStatus(const MCTS::Stats& st) {
  std::string line;
  RGBA col = C_TEXT;
  if (result_ == ONGOING) {
    bool cpu = playerIsComputer(pos_.stm());
    line = (pos_.stm() == BLACK ? "BLACK" : "WHITE");
    line += cpu ? " THINKING" : " TO MOVE";
    if (inCheck(pos_, pos_.stm())) {
      line += "   CHECK";
      col = C_SUGGEST;
    }
  } else if (result_ == DRAW) {
    line = "DRAW";
  } else {
    line = (result_ == BLACK_WIN ? "BLACK WINS" : "WHITE WINS");
    line += "  CHECKMATE";
  }
  drawTextC(ren_, WIN_W / 2, 22, 30, line, col);
  std::string info = "PLAYOUTS " + std::to_string(st.rootVisits);
  if (!st.bestMove.isNull() && result_ == ONGOING)
    info += "    BEST " + moveToString(st.bestMove);
  drawTextC(ren_, WIN_W / 2, 62, 17, info, C_DIM);
}

void App::renderPromoDialog() {
  fillRect(ren_, 0, 0, WIN_W, WIN_H, RGBA{0, 0, 0, 150});
  float bx = WIN_W / 2 - 190, by = WIN_H / 2 - 80;
  fillRect(ren_, bx, by, 380, 160, C_PANEL);
  outlineRect(ren_, bx, by, 380, 160, C_TEXT);
  drawTextC(ren_, WIN_W / 2, int(by) + 28, 26, "PROMOTE PIECE?", C_TEXT);
  promoYes_ = Button{{bx + 30, by + 84, 150, 48}, "YES"};
  promoNo_ = Button{{bx + 200, by + 84, 150, 48}, "NO"};
  float mx, my;
  mousePos(ren_, mx, my);
  drawButton(ren_, promoYes_, promoYes_.hit(mx, my));
  drawButton(ren_, promoNo_, promoNo_.hit(mx, my));
}

void App::renderResult() {
  fillRect(ren_, 0, WIN_H / 2 - 40, WIN_W, 80, RGBA{0, 0, 0, 160});
  std::string t = (result_ == DRAW)
                      ? "DRAW BY REPETITION"
                      : (result_ == BLACK_WIN ? "BLACK WINS" : "WHITE WINS");
  drawTextC(ren_, WIN_W / 2, WIN_H / 2 - 22, 40, t, C_SUGGEST);
}

void App::render() {
  // Clear the whole output (including the letterbox margin) to the background.
  setColor(ren_, C_BG);
  SDL_RenderClear(ren_);
  fillRect(ren_, 0, 0, WIN_W, WIN_H, C_BG);
  if (screen_ == SCR_MENU) {
    renderMenu();
    return;
  }
  MCTS::Stats st = engine_.stats();
  renderBoard();
  renderHands();
  renderBars(st);
  renderStatus(st);

  float mx, my;
  mousePos(ren_, mx, my);
  drawButton(ren_, newGameBtn_, newGameBtn_.hit(mx, my));
  suggestBtn_.label = showSuggest_ ? "SUGGEST  ON" : "SUGGEST  OFF";
  drawButton(ren_, suggestBtn_, suggestBtn_.hit(mx, my));

  if (result_ != ONGOING) renderResult();
  if (promoDialog_) renderPromoDialog();
}

// --- Application lifecycle --------------------------------------------------
// One-time setup: SDL, the window and renderer, and the embedded assets.
bool App::init() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
#ifdef __EMSCRIPTEN__
  win_ = SDL_CreateWindow("Shogi", WIN_W, WIN_H,
                          SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
#else
  win_ = SDL_CreateWindow("Shogi - MCTS", WIN_W, WIN_H,
                          SDL_WINDOW_HIGH_PIXEL_DENSITY);
#endif
  if (!win_) {
    SDL_Log("CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  // Window / taskbar icon: the gold-general piece (src/icon.hpp, generated by
  // tools/genicon.cpp).  SDL copies the pixels into the window, so the source
  // surface is only needed transiently.  A harmless no-op on the web build,
  // which has no OS window chrome.
  if (SDL_Surface* icon = SDL_CreateSurfaceFrom(
          ICON_W, ICON_H, SDL_PIXELFORMAT_RGBA32,
          const_cast<uint8_t*>(ICON_RGBA), ICON_W * 4)) {
    SDL_SetWindowIcon(win_, icon);
    SDL_DestroySurface(icon);
  }
  syncCanvasSize();   // web: fit the canvas to the viewport at device density
  ren_ = SDL_CreateRenderer(win_, nullptr);
  if (!ren_) {
    SDL_Log("CreateRenderer failed: %s", SDL_GetError());
    return false;
  }
  // The game is drawn in a fixed WIN_W x WIN_H coordinate space; the renderer
  // scales and letterboxes that to whatever size the window or canvas really
  // is.  This makes it render fully (never clipped) at any window size - on a
  // phone, in a resized window - and crisply on high-DPI displays.
  SDL_SetRenderLogicalPresentation(ren_, WIN_W, WIN_H,
                                   SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_SetRenderVSync(ren_, 1);
  SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
  buildGlyphs(ren_);                     // upload the embedded font atlas
  buildTileTextures(ren_);               // and the anti-aliased piece tiles

  initZobrist();

  // Static button geometry.
  const char* modeLabels[4] = {"HUMAN VS HUMAN", "HUMAN VS COMPUTER",
                               "COMPUTER VS HUMAN", "COMPUTER VS COMPUTER"};
  for (int i = 0; i < 4; ++i)
    menuButtons_.push_back(
        Button{{float(WIN_W / 2 - 230), float(280 + i * 76), 460, 56},
               modeLabels[i]});
  newGameBtn_ = Button{{float(BOARD_X), 636, 240, 46}, "MENU"};
  suggestBtn_ = Button{{float(BOARD_X + BOARD_PX - 240), 636, 240, 46},
                       "SUGGEST  ON"};
  return true;
}

// Teardown.  SDL calls SDL_Quit() itself after SDL_AppQuit returns.
App::~App() {
  engine_.stop();
  SDL_DestroyRenderer(ren_);   // both NULL-safe, in case init() failed partway
  SDL_DestroyWindow(win_);
}

// Web: keep the canvas a WIN_W:WIN_H box fitted to the viewport, with the
// backing store at device-pixel resolution (CSS size x devicePixelRatio) so
// the picture is sharp.  SDL owns the CSS size (via SDL_SetWindowSize); this
// owns the backing store (via emscripten_set_canvas_element_size).  No-op on
// native.  Cheap, and only acts when the viewport actually changed.
void App::syncCanvasSize() {
#ifdef __EMSCRIPTEN__
  int vw = EM_ASM_INT({ return window.innerWidth | 0; });
  int vh = EM_ASM_INT({ return window.innerHeight | 0; });
  if (vw < 1 || vh < 1) return;
  double s = double(vw) / WIN_W;
  if (double(vh) / WIN_H < s) s = double(vh) / WIN_H;   // fit the viewport
  int cssW = int(WIN_W * s), cssH = int(WIN_H * s);
  if (cssW == webCssW_ && cssH == webCssH_) return;     // unchanged
  webCssW_ = cssW;
  webCssH_ = cssH;
  double dpr = emscripten_get_device_pixel_ratio();
  if (dpr < 1.0) dpr = 1.0;
  emscripten_set_canvas_element_size("#canvas", int(cssW * dpr + 0.5),
                                     int(cssH * dpr + 0.5));   // backing store
  SDL_SetWindowSize(win_, cssW, cssH);                         // CSS size
  if (ren_)
    SDL_SetRenderLogicalPresentation(ren_, WIN_W, WIN_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
#endif
}

// Handle one input event.  SDL delivers these (one call each) ahead of the
// SDL_AppIterate that closes the frame.
SDL_AppResult App::onEvent(SDL_Event& e) {
  if (e.type == SDL_EVENT_QUIT) {
    running_ = false;
  } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
             e.button.button == SDL_BUTTON_LEFT) {
    SDL_ConvertEventToRenderCoordinates(ren_, &e);   // window px -> logical
    onClick(e.button.x, e.button.y);
  } else if (e.type == SDL_EVENT_KEY_DOWN) {
    if (e.key.key == SDLK_ESCAPE) {
      if (screen_ == SCR_PLAY) {
        engine_.stop();
        screen_ = SCR_MENU;
      } else {
        running_ = false;
      }
    } else if (e.key.key == SDLK_S) {
      showSuggest_ = !showSuggest_;
    }
  }
  return running_ ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

// One iteration of the main loop: search progress, computer move, render.
// SDL calls this once per frame.
SDL_AppResult App::iterate() {
  syncCanvasSize();   // track viewport resize / rotation (web)

  // Grow the search tree.  No-op on a threaded build (worker threads do the
  // searching); on a single-threaded build this is the search.
  if (screen_ == SCR_PLAY && result_ == ONGOING) engine_.pump(12);

  // Drive computer moves.
  if (screen_ == SCR_PLAY && result_ == ONGOING && !promoDialog_ &&
      playerIsComputer(pos_.stm())) {
    if (!thinking_) {
      thinking_ = true;
      thinkStart_ = SDL_GetTicks();
    } else {
      Uint64 elapsed = SDL_GetTicks() - thinkStart_;
      bool done = elapsed >= MAX_THINK_MS ||
                  (elapsed >= MIN_THINK_MS &&
                   engine_.visits() >= MIN_PLAYOUTS);
      if (done) {
        Move mv = engine_.stats().bestMove;
        if (mv.isNull() && !legal_.empty()) mv = legal_[0];
        if (!mv.isNull()) applyMove(mv);
      }
    }
  }

  render();
  SDL_RenderPresent(ren_);
  return running_ ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

}  // namespace
}  // namespace shogi

// --- SDL_MAIN_USE_CALLBACKS entry points ------------------------------------
// SDL's generated main() drives the program through these four functions, so
// there is no app-level loop.  They are global (SDL resolves them by name) and
// just forward to the App, which is carried between calls as the "appstate"
// pointer.  SDL calls SDL_Quit() itself once SDL_AppQuit returns.
SDL_AppResult SDL_AppInit(void** appstate, int /*argc*/, char** /*argv*/) {
  shogi::App* app = new shogi::App();
  *appstate = app;
  return app->init() ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}
SDL_AppResult SDL_AppIterate(void* appstate) {
  return static_cast<shogi::App*>(appstate)->iterate();
}
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
  return static_cast<shogi::App*>(appstate)->onEvent(*event);
}
void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/) {
  delete static_cast<shogi::App*>(appstate);
}
