// mcts.cpp - Incremental MCTS implementation.
#include "mcts.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace shogi {

namespace {
constexpr double CPUCT     = 3.0;         // PUCT exploration constant
constexpr size_t MAX_NODES = 700000;      // tree-size ceiling

// Material value used only for capture-ordering in the policy prior.
int pieceVal(Piece q) {
  switch (typeOf(q)) {
    case PT_PAWN:   return 90;
    case PT_LANCE:  return 230;
    case PT_KNIGHT: return 250;
    case PT_SILVER: return 360;
    case PT_GOLD:   return 440;
    case PT_BISHOP: return 560;
    case PT_ROOK:   return 640;
    case PT_KING:   return 4000;
    default:        return 0;
  }
}
int findKing(const Position& p, Color c) {
  Piece want = makePiece(c, PT_KING, false);
  for (int s = 0; s < N_SQ; ++s)
    if (p.board[s] == want) return s;
  return -1;
}

// Heuristic policy prior (un-normalised, positive) for move `m` in `p`.
// Captures and promotions are favoured; a king stepping off its home two
// ranks is heavily penalised - the search-level brake on the king-walk.
double movePrior(const Position& p, const Move& m, int enemyKing) {
  Color me = p.stm();
  double pr = 1.0;
  if (!m.isDrop()) {
    Piece mover  = p.board[m.from];
    Piece victim = p.board[m.to];
    if (victim) pr += pieceVal(victim) / 80.0;          // MVV: favour big victims
    if (m.promo)
      pr += (typeOf(mover) == PT_ROOK || typeOf(mover) == PT_BISHOP) ? 2.5 : 1.2;
    if (typeOf(mover) == PT_KING) {
      int row = rowOf(m.to);
      bool home = (me == BLACK) ? (row >= 7) : (row <= 1);
      if (!home) pr *= 0.05;                            // king-walk brake
    }
  } else {
    pr = 0.7;                                           // drops: modest default
    if (enemyKing >= 0) {
      int cheb = std::max(std::abs(rowOf(m.to) - rowOf(enemyKing)),
                          std::abs(colOf(m.to) - colOf(enemyKing)));
      if (cheb <= 2) pr += 1.5;                         // drop near enemy king
    }
  }
  return pr < 0.02 ? 0.02 : pr;
}
}  // namespace

MCTS::~MCTS() { freeTree(root_); }

void MCTS::freeTree(Node* n) {
  if (!n) return;
  for (Node* c : n->children) freeTree(c);
  delete n;
}

static size_t countNodes(const Node* n) {
  size_t c = 1;
  for (const Node* k : n->children) c += countNodes(k);
  return c;
}

void MCTS::advance(const Move& m, const Position& newPos) {
  std::lock_guard<std::mutex> lk(mtx_);
  Node* keep = nullptr;
  if (root_)
    for (Node* c : root_->children)
      if (c->move == m) { keep = c; break; }

  if (keep) {
    // Detach the subtree for `m`; free the old root and the other branches.
    for (Node* c : root_->children)
      if (c != keep) freeTree(c);
    root_->children.clear();
    delete root_;
    keep->parent = nullptr;
    root_ = keep;
    nodeCount_ = countNodes(root_);
  } else {
    freeTree(root_);
    root_ = new Node();
    root_->pos = newPos;
    nodeCount_ = 1;
  }
}

void MCTS::setRoot(const Position& p) {
  std::lock_guard<std::mutex> lk(mtx_);
  freeTree(root_);
  root_ = new Node();
  root_->pos = p;
  nodeCount_ = 1;
}

// Expand: discover the legal moves from `n`, or flag it terminal.  Moves are
// stored sorted ascending by policy prior, so iterate() pops the most
// promising candidate from the back of `untried`.
void MCTS::expand(Node* n) {
  std::vector<Move> moves;
  generateLegalMoves(n->pos, moves);
  n->expanded = true;
  if (moves.empty()) {
    // Side to move has no legal reply -> that side loses.
    n->terminal = true;
    n->terminalBlack = (n->pos.stm() == BLACK) ? 0.0 : 1.0;
    return;
  }
  size_t m = moves.size();
  int enemyKing = findKing(n->pos, opp(n->pos.stm()));
  std::vector<double> pr(m);
  double sum = 0.0;
  for (size_t i = 0; i < m; ++i) {
    pr[i] = movePrior(n->pos, moves[i], enemyKing);
    sum += pr[i];
  }
  std::vector<size_t> idx(m);
  for (size_t i = 0; i < m; ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(),
            [&](size_t a, size_t b) { return pr[a] < pr[b]; });
  n->untried.resize(m);
  n->untriedPriors.resize(m);
  for (size_t i = 0; i < m; ++i) {
    n->untried[i]       = moves[idx[i]];
    n->untriedPriors[i] = float(pr[idx[i]] / sum);     // normalised to sum 1
  }
}

// PUCT child selection from the perspective of the side to move at `n`.
// score = exploit + CPUCT * prior * sqrt(parentN) / (1 + childN)
Node* MCTS::selectChild(Node* n) {
  const bool blackToMove = (n->pos.stm() == BLACK);
  const double sqrtParent = std::sqrt(double(n->visits + n->virtualLoss + 1));
  Node* best = nullptr;
  double bestScore = -1e18;
  for (Node* c : n->children) {
    int vis = c->visits + c->virtualLoss;
    // Each pending virtual loss counts as a loss for the side choosing here.
    double vb = c->valueBlack + (blackToMove ? 0.0 : double(c->virtualLoss));
    double meanBlack = (vis > 0) ? vb / vis : 0.5;       // FPU: optimistic 0.5
    double exploit = blackToMove ? meanBlack : (1.0 - meanBlack);
    double explore = CPUCT * c->prior * sqrtParent / (1.0 + vis);
    double score = exploit + explore;
    if (score > bestScore) { bestScore = score; best = c; }
  }
  return best;
}

void MCTS::iterate(uint64_t& rng) {
  std::vector<Node*> path;
  Node* leaf = nullptr;
  double result = 0.0;

  {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!root_) return;
    Node* n = root_;
    path.push_back(n);
    // Descend through fully-expanded interior nodes.
    while (n->expanded && !n->terminal && n->untried.empty() &&
           !n->children.empty()) {
      n = selectChild(n);
      ++n->virtualLoss;
      path.push_back(n);
    }
    if (!n->terminal) {
      if (!n->expanded) expand(n);
      if (!n->terminal && !n->untried.empty() && nodeCount_ < MAX_NODES) {
        Move mv = n->untried.back();
        n->untried.pop_back();
        float pri = n->untriedPriors.back();
        n->untriedPriors.pop_back();
        Node* child = new Node();
        ++nodeCount_;
        child->parent = n;
        child->move = mv;
        child->prior = pri;
        child->pos = n->pos;
        doMove(child->pos, mv);
        n->children.push_back(child);
        ++child->virtualLoss;
        path.push_back(child);
        n = child;
      }
    }
    leaf = n;
    if (leaf->terminal) result = leaf->terminalBlack;
  }

  if (!leaf->terminal) result = evalLeaf(leaf->pos);

  {
    std::lock_guard<std::mutex> lk(mtx_);
    for (Node* x : path) {
      ++x->visits;
      x->valueBlack += result;
    }
    for (size_t i = 1; i < path.size(); ++i) --path[i]->virtualLoss;
  }
}

int MCTS::rootVisits() {
  std::lock_guard<std::mutex> lk(mtx_);
  return root_ ? root_->visits : 0;
}

MCTS::Stats MCTS::snapshot() {
  std::lock_guard<std::mutex> lk(mtx_);
  Stats s;
  if (!root_) return s;
  s.rootVisits = root_->visits;
  if (root_->visits > 0)
    s.blackWinProb = root_->valueBlack / root_->visits;
  // Walk the most-visited child at each step to build the PV.
  Node* n = root_;
  while (n && !n->children.empty()) {
    Node* best = nullptr;
    for (Node* c : n->children)
      if (!best || c->visits > best->visits) best = c;
    if (!best || best->visits == 0) break;
    if (n == root_) s.bestMove = best->move;
    s.pv.push_back(best->move);
    n = best;
    if (s.pv.size() > 16) break;
  }
  return s;
}

}  // namespace shogi
