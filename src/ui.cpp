// ui.cpp - SDL3 user interface and game flow.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "board.hpp"
#include "engine.hpp"
#include "font.hpp"
#include "ui.hpp"

namespace shogi {
namespace {

// --- Layout -----------------------------------------------------------------
constexpr int WIN_W = 1024, WIN_H = 720;
constexpr int CELL = 58;
constexpr int BOARD_X = 251, BOARD_Y = 96;
constexpr int BOARD_PX = 9 * CELL;
constexpr int HAND_W = 188, HAND_EH = 74;
constexpr int WHITE_HAND_X = 44, BLACK_HAND_X = 786;
constexpr int WBAR_X = 18, BBAR_X = 990, BAR_W = 16;
constexpr Uint64 THINK_MS = 4000;          // computer search time per move

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
// Visible width of a string: glyphs are 5 px wide on a 6 px advance, so the
// last glyph contributes no trailing column.
int textW(const std::string& s, int sc) {
  return s.empty() ? 0 : (int(s.size()) * 6 - 1) * sc;
}

void drawText(SDL_Renderer* r, int x, int y, int sc, const std::string& s,
              RGBA c) {
  setColor(r, c);
  for (char ch : s) {
    const uint8_t* g = fontGlyph(ch);
    for (int row = 0; row < 8; ++row)
      for (int col = 0; col < 5; ++col)
        if (g[row] & (1 << col)) {
          SDL_FRect q{float(x + col * sc), float(y + row * sc),
                      float(sc), float(sc)};
          SDL_RenderFillRect(r, &q);
        }
    x += 6 * sc;
  }
}
void drawTextC(SDL_Renderer* r, int cx, int y, int sc, const std::string& s,
               RGBA c) {
  drawText(r, cx - textW(s, sc) / 2, y, sc, s, c);
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

char letterOf(Piece pc) {
  switch (typeOf(pc)) {
    case PT_PAWN:   return 'P';
    case PT_LANCE:  return 'L';
    case PT_KNIGHT: return 'N';
    case PT_SILVER: return 'S';
    case PT_GOLD:   return 'G';
    case PT_BISHOP: return 'B';
    case PT_ROOK:   return 'R';
    case PT_KING:   return 'K';
    default:        return '?';
  }
}

// Draw a shogi piece as a pentagon tile pointing toward the enemy.
void drawPiece(SDL_Renderer* r, float x, float y, float w, float h, Piece pc,
               bool selected) {
  bool up = colorOf(pc) == BLACK;          // Black sits at the bottom
  float mx = w * 0.10f, my = h * 0.07f;
  float L = x + mx, R = x + w - mx, T = y + my, B = y + h - my;
  float cx = (L + R) / 2.f, sh = (B - T) * 0.34f, in = (R - L) * 0.08f;
  SDL_FPoint pts[5];
  if (up) {
    pts[0] = {cx, T};            pts[1] = {R, T + sh};
    pts[2] = {R - in, B};        pts[3] = {L + in, B};
    pts[4] = {L, T + sh};
  } else {
    pts[0] = {cx, B};            pts[1] = {R, B - sh};
    pts[2] = {R - in, T};        pts[3] = {L + in, T};
    pts[4] = {L, B - sh};
  }
  RGBA tile = up ? RGBA{244, 212, 150, 255} : RGBA{236, 226, 200, 255};
  if (selected) tile = RGBA{252, 236, 132, 255};
  fillPoly(r, pts, 5, tile);
  setColor(r, RGBA{62, 44, 18, 255});
  for (int i = 0; i < 5; ++i)
    SDL_RenderLine(r, pts[i].x, pts[i].y, pts[(i + 1) % 5].x, pts[(i + 1) % 5].y);

  bool promo = isPromoted(pc);
  RGBA col = promo ? RGBA{176, 36, 24, 255} : RGBA{38, 26, 12, 255};
  int sc = std::max(2, int(w / 13));
  // Centre the glyph on the tile's optical centre.  A glyph is 5x7 px; its
  // mid-point is 2.5*sc across and 3.5*sc down from the top-left.  The
  // pentagon's mass sits toward its wide end, so nudge the label that way.
  float labelCY = y + h * (up ? 0.55f : 0.45f);
  drawTextC(r, int(cx), int(labelCY - 3.5f * sc), sc,
            std::string(1, letterOf(pc)), col);
  if (promo)
    drawTextC(r, int(cx), int(y + h * (up ? 0.15f : 0.62f)),
              std::max(1, sc - 1), "+", col);
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
  int sc = 3;
  drawTextC(r, int(b.rect.x + b.rect.w / 2),
            int(b.rect.y + b.rect.h / 2 - 4 * sc), sc, b.label, C_TEXT);
}

// ---------------------------------------------------------------------------
// The application / game object.
// ---------------------------------------------------------------------------
enum Screen { SCR_MENU, SCR_PLAY };
enum Mode { MODE_HVH, MODE_HVC, MODE_CVH, MODE_CVC };

class App {
 public:
  int run();

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

  void frameStep();                    // one main-loop iteration
#ifdef __EMSCRIPTEN__
  static void frameThunk(void* self);  // emscripten main-loop trampoline
#endif

  int squareAt(float mx, float my) const;       // -1 if outside board
  SDL_FRect handEntry(Color c, int idx) const;

  SDL_Renderer* ren_ = nullptr;
  SDL_Window* win_ = nullptr;
  Engine engine_;

  Screen screen_ = SCR_MENU;
  Mode mode_ = MODE_HVC;
  bool running_ = true;
  bool human_[2] = {true, true};

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
    engine_.advance(m, pos_);     // reuse the subtree pondered for this move
  else
    engine_.stop();
}

void App::startGame(Mode m) {
  mode_ = m;
  human_[BLACK] = (m == MODE_HVH || m == MODE_HVC);
  human_[WHITE] = (m == MODE_HVH || m == MODE_CVH);
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
  engine_.setPosition(pos_);
  screen_ = SCR_PLAY;
}

// --- Geometry ---------------------------------------------------------------
int App::squareAt(float mx, float my) const {
  if (mx < BOARD_X || mx >= BOARD_X + BOARD_PX || my < BOARD_Y ||
      my >= BOARD_Y + BOARD_PX)
    return -1;
  int col = int((mx - BOARD_X) / CELL);
  int row = int((my - BOARD_Y) / CELL);
  return sq(row, col);
}
SDL_FRect App::handEntry(Color c, int idx) const {
  int x = (c == WHITE) ? WHITE_HAND_X : BLACK_HAND_X;
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
  drawTextC(ren_, WIN_W / 2, 96, 9, "SHOGI", C_TEXT);
  drawTextC(ren_, WIN_W / 2, 188, 2,
            "MULTITHREADED MONTE CARLO TREE SEARCH", C_DIM);
  float mx, my;
  SDL_GetMouseState(&mx, &my);
  for (const Button& b : menuButtons_)
    drawButton(ren_, b, b.hit(mx, my));
  drawTextC(ren_, WIN_W / 2, 624, 2,
            "ENGINE THREADS  " + std::to_string(engine_.threadCount()), C_DIM);
  drawTextC(ren_, WIN_W / 2, 660, 2, "CLICK A MODE TO BEGIN", C_DIM);
}

void App::renderBoard() {
  fillRect(ren_, BOARD_X, BOARD_Y, BOARD_PX, BOARD_PX, C_WOOD);

  // Highlight last move and current selection.
  auto squareRect = [](int s) {
    return SDL_FRect{float(BOARD_X + colOf(s) * CELL),
                     float(BOARD_Y + rowOf(s) * CELL), float(CELL),
                     float(CELL)};
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
    drawPiece(ren_, rc.x, rc.y, rc.w, rc.h, pc, s == selSq_);
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
    int x = (col == WHITE) ? WHITE_HAND_X : BLACK_HAND_X;
    drawTextC(ren_, x + HAND_W / 2, BOARD_Y - 28, 2,
              names[c == BLACK ? 0 : 1], C_DIM);
    for (int i = 0; i < 7; ++i) {
      PieceType t = PieceType(PT_PAWN + i);
      SDL_FRect rc = handEntry(col, i);
      bool sel = (selHand_ == t && pos_.stm() == col);
      fillRect(ren_, rc.x, rc.y, rc.w, rc.h, sel ? C_BTN_HOT : C_PANEL);
      outlineRect(ren_, rc.x, rc.y, rc.w, rc.h, RGBA{20, 22, 28, 255});
      Piece pc = makePiece(col, t, false);
      drawPiece(ren_, rc.x + 6, rc.y + 2, 56, 56, pc, false);
      int n = pos_.hand[c][t];
      RGBA tc = n ? C_TEXT : C_DIM;
      drawText(ren_, int(rc.x) + 78, int(rc.y) + 18, 4,
               "x" + std::to_string(n), tc);
    }
  }
}

void App::renderBars(const MCTS::Stats& st) {
  double pb = st.blackWinProb;
  struct B { int x; double frac; RGBA col; const char* tag; } bars[2] = {
      {WBAR_X, 1.0 - pb, C_BAR_W, "W"},
      {BBAR_X, pb, C_BAR_B, "B"}};
  for (const B& b : bars) {
    fillRect(ren_, b.x, BOARD_Y, BAR_W, BOARD_PX, C_PANEL);
    int h = int(BOARD_PX * std::clamp(b.frac, 0.0, 1.0));
    fillRect(ren_, b.x, BOARD_Y + BOARD_PX - h, BAR_W, h, b.col);
    outlineRect(ren_, b.x, BOARD_Y, BAR_W, BOARD_PX, RGBA{20, 22, 28, 255});
    drawTextC(ren_, b.x + BAR_W / 2, BOARD_Y - 22, 2, b.tag, C_DIM);
    int pct = int(b.frac * 100.0 + 0.5);
    drawTextC(ren_, b.x + BAR_W / 2, BOARD_Y + BOARD_PX + 8, 2,
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
  drawTextC(ren_, WIN_W / 2, 26, 4, line, col);
  std::string info = "PLAYOUTS " + std::to_string(st.rootVisits);
  if (!st.bestMove.isNull() && result_ == ONGOING)
    info += "    BEST " + moveToString(st.bestMove);
  drawTextC(ren_, WIN_W / 2, 64, 2, info, C_DIM);
}

void App::renderPromoDialog() {
  fillRect(ren_, 0, 0, WIN_W, WIN_H, RGBA{0, 0, 0, 150});
  float bx = WIN_W / 2 - 190, by = WIN_H / 2 - 80;
  fillRect(ren_, bx, by, 380, 160, C_PANEL);
  outlineRect(ren_, bx, by, 380, 160, C_TEXT);
  drawTextC(ren_, WIN_W / 2, int(by) + 24, 3, "PROMOTE PIECE?", C_TEXT);
  promoYes_ = Button{{bx + 30, by + 84, 150, 48}, "YES"};
  promoNo_ = Button{{bx + 200, by + 84, 150, 48}, "NO"};
  float mx, my;
  SDL_GetMouseState(&mx, &my);
  drawButton(ren_, promoYes_, promoYes_.hit(mx, my));
  drawButton(ren_, promoNo_, promoNo_.hit(mx, my));
}

void App::renderResult() {
  fillRect(ren_, 0, WIN_H / 2 - 40, WIN_W, 80, RGBA{0, 0, 0, 160});
  std::string t = (result_ == DRAW)
                      ? "DRAW BY REPETITION"
                      : (result_ == BLACK_WIN ? "BLACK WINS" : "WHITE WINS");
  drawTextC(ren_, WIN_W / 2, WIN_H / 2 - 16, 5, t, C_SUGGEST);
}

void App::render() {
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
  SDL_GetMouseState(&mx, &my);
  drawButton(ren_, newGameBtn_, newGameBtn_.hit(mx, my));
  suggestBtn_.label = showSuggest_ ? "SUGGEST  ON" : "SUGGEST  OFF";
  drawButton(ren_, suggestBtn_, suggestBtn_.hit(mx, my));

  if (result_ != ONGOING) renderResult();
  if (promoDialog_) renderPromoDialog();
}

// --- Main loop --------------------------------------------------------------
int App::run() {
  SDL_SetMainReady();
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  win_ = SDL_CreateWindow("Shogi - MCTS", WIN_W, WIN_H, 0);
  if (!win_) {
    SDL_Log("CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  ren_ = SDL_CreateRenderer(win_, nullptr);
  if (!ren_) {
    SDL_Log("CreateRenderer failed: %s", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(ren_, 1);
  SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

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

#ifdef __EMSCRIPTEN__
  // The browser owns the event loop; it calls frameStep once per animation
  // frame.  simulate_infinite_loop = 1 means this call does not return.
  emscripten_set_main_loop_arg(&App::frameThunk, this, 0, 1);
  return 0;
#else
  while (running_) frameStep();
  engine_.stop();
  SDL_DestroyRenderer(ren_);
  SDL_DestroyWindow(win_);
  SDL_Quit();
  return 0;
#endif
}

#ifdef __EMSCRIPTEN__
void App::frameThunk(void* self) { static_cast<App*>(self)->frameStep(); }
#endif

// One iteration of the main loop: input, search progress, computer move,
// render.  Driven by a plain while loop natively, by the browser on the web.
void App::frameStep() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_QUIT) {
      running_ = false;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
               e.button.button == SDL_BUTTON_LEFT) {
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
  }

  // Grow the search tree.  No-op on a threaded build (worker threads do the
  // searching); on a single-threaded build this is the search.
  if (screen_ == SCR_PLAY && result_ == ONGOING) engine_.pump(12);

  // Drive computer moves.
  if (screen_ == SCR_PLAY && result_ == ONGOING && !promoDialog_ &&
      playerIsComputer(pos_.stm())) {
    if (!thinking_) {
      thinking_ = true;
      thinkStart_ = SDL_GetTicks();
    } else if (SDL_GetTicks() - thinkStart_ >= THINK_MS) {
      Move mv = engine_.stats().bestMove;
      if (mv.isNull() && !legal_.empty()) mv = legal_[0];
      if (!mv.isNull()) applyMove(mv);
    }
  }

  render();
  SDL_RenderPresent(ren_);
}

}  // namespace

int runGame() {
  App app;
  return app.run();
}

}  // namespace shogi
