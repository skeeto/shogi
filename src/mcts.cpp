// mcts.cpp - Incremental MCTS implementation.
#include "mcts.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace shogi {

namespace {
#ifdef __EMSCRIPTEN__
constexpr size_t MAX_NODES = 300000;      // smaller ceiling for the wasm heap
#else
constexpr size_t MAX_NODES = 700000;      // tree-size ceiling
#endif

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

// --- MCTS-Solver helpers ---------------------------------------------------

// Black-perspective value of a node whose game result is proven.
double provenValueBlack(const Node* n) {
  if (n->proven == PV_DRAW) return 0.5;
  return ((n->proven == PV_WIN) == (n->pos.stm() == BLACK)) ? 1.0 : 0.0;
}

// MCTS-Solver: try to prove `n` from its children (negamax).  A child is the
// opponent to move, so a proven-loss child is a winning move for `n`; once
// every reply is resolved, `n` takes the best outcome available to it.
void tryProve(Node* n) {
  if (!n->movesGenerated) return;
  bool sawLoss = false, sawDraw = false;
  bool allResolved = n->untried.empty();
  int winDepth  = 1 << 20;        // shortest forced win among loss-children
  int holdDepth = 0;              // longest a resolved child can hold out
  for (const Node* c : n->children) {
    if      (c->proven == PV_LOSS) { sawLoss = true;
                                     if (c->provenDepth < winDepth)
                                       winDepth = c->provenDepth; }
    else if (c->proven == PV_DRAW)   sawDraw = true;
    else if (c->proven == PV_NONE)   allResolved = false;
    if (c->proven != PV_NONE && c->provenDepth > holdDepth)
      holdDepth = c->provenDepth;
  }
  if (sawLoss) {                                 // a move that mates them
    n->proven = PV_WIN;
    n->provenDepth = uint16_t(winDepth + 1);
  } else if (allResolved) {                      // every reply is resolved
    n->proven = sawDraw ? PV_DRAW : PV_LOSS;
    n->provenDepth = uint16_t(holdDepth + 1);
  }
}

// Rank of a child as a move to play: a proven loss for the child is a win for
// us (2), an unsolved or drawn child is neutral (1), a proven win for the
// child is a loss for us (0).
int playRank(const Node* c) {
  return (c->proven == PV_LOSS) ? 2 : (c->proven == PV_WIN) ? 0 : 1;
}

// Best child to play or display: prefer the highest rank; within a winning
// rank take the shortest mate, within a losing rank the longest resistance,
// otherwise the most-visited child (the robust MCTS choice).
Node* bestChild(const Node* n) {
  Node* best = nullptr;
  for (Node* c : n->children) {
    if (!best) { best = c; continue; }
    int rc = playRank(c), rb = playRank(best);
    bool better;
    if      (rc != rb) better = rc > rb;
    else if (rc == 2)  better = c->provenDepth < best->provenDepth;
    else if (rc == 0)  better = c->provenDepth > best->provenDepth;
    else               better = c->visits > best->visits;
    if (better) best = c;
  }
  return best;
}
}  // namespace

MCTS::~MCTS() = default;   // pool_'s destructor releases every node's slab

// Returns the subtree rooted at `n` to the pool's free-list.
void MCTS::freeTree(Node* n) {
  if (!n) return;
  for (Node* c : n->children) freeTree(c);
  pool_.free(n);
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
    pool_.free(root_);
    keep->parent = nullptr;
    root_ = keep;
    nodeCount_ = countNodes(root_);
  } else {
    freeTree(root_);
    root_ = pool_.alloc();
    root_->pos = newPos;
    nodeCount_ = 1;
  }
  // A reused subtree may already be solved; a fresh root never is.
  solved_.store(root_->proven != PV_NONE, std::memory_order_relaxed);
}

void MCTS::setRoot(const Position& p) {
  std::lock_guard<std::mutex> lk(mtx_);
  freeTree(root_);
  root_ = pool_.alloc();
  root_->pos = p;
  nodeCount_ = 1;
  solved_.store(false, std::memory_order_relaxed);
}

// Expand: discover the legal moves from `n` into `untried` with their policy
// priors, or flag the node terminal.  Children are materialised lazily by
// selectChild/iterate, so expand() creates no nodes - it only records the
// candidate moves and their priors.
void MCTS::expand(Node* n, Scratch& sc) {
  std::vector<Move>& moves = sc.expandMoves;       // reused scratch buffers
  generateLegalMoves(n->pos, moves, sc);
  n->movesGenerated = true;
  if (moves.empty()) {
    n->proven = PV_LOSS;          // no legal reply -> the side to move loses
    return;
  }
  size_t m = moves.size();
  int enemyKing = findKing(n->pos, opp(n->pos.stm()));
  std::vector<double>& pr = sc.expandPr;
  pr.resize(m);
  double sum = 0.0;
  for (size_t i = 0; i < m; ++i) {
    pr[i] = movePrior(n->pos, moves[i], enemyKing);
    sum += pr[i];
  }
  n->untried.resize(m);
  n->untriedPriors.resize(m);
  for (size_t i = 0; i < m; ++i) {
    n->untried[i]       = moves[i];
    n->untriedPriors[i] = float(pr[i] / sum);          // normalised to sum 1
  }
}

// PUCT selection from the perspective of the side to move at `n`, ranging over
// both the materialised children and the not-yet-materialised moves (each a
// "virtual child" valued at the FPU estimate).  When `canMaterialize` is false
// (the node pool is full) only existing children are considered.
// score = exploit + CPUCT * prior * sqrt(parentN) / (1 + childN)
MCTS::Pick MCTS::selectChild(Node* n, bool canMaterialize) {
  const bool blackToMove = (n->pos.stm() == BLACK);
  const double sqrtParent = std::sqrt(double(n->visits + n->virtualLoss + 1));
  const double fpu = 0.5;                  // flat FPU; parent-relative later

  Pick best;
  double bestScore = -1e18;
  for (Node* c : n->children) {
    double exploit, explore;
    if (c->proven != PV_NONE) {
      // A solved child is scored by its proven value; no exploration needed.
      exploit = (c->proven == PV_DRAW) ? 0.5
              : (c->proven == PV_LOSS) ? 1.0     // child loses => we win
              :                         0.0;     // child wins  => we lose
      explore = 0.0;
    } else {
      int vis = c->visits + c->virtualLoss;
      // Each pending virtual loss counts as a loss for the side choosing here.
      double vb = c->valueBlack + (blackToMove ? 0.0 : double(c->virtualLoss));
      double meanBlack = (vis > 0) ? vb / vis : 0.5;
      exploit = blackToMove ? meanBlack : (1.0 - meanBlack);
      explore = cpuct * c->prior * sqrtParent / (1.0 + vis);
    }
    double score = exploit + explore;
    if (score > bestScore) { bestScore = score; best = Pick{c, -1}; }
  }
  if (canMaterialize) {
    for (size_t i = 0; i < n->untried.size(); ++i) {
      // A virtual child has zero visits: childN = 0.
      double explore = cpuct * n->untriedPriors[i] * sqrtParent;
      double score = fpu + explore;
      if (score > bestScore) { bestScore = score; best = Pick{nullptr, int(i)}; }
    }
  }
  return best;
}

void MCTS::iterate(uint64_t& rng, Scratch& sc) {
  (void)rng;
  std::vector<Node*>& path = sc.path;       // reused; cleared each iteration
  path.clear();
  Node*  leaf = nullptr;
  bool   leafProven = false;
  double result = 0.0;

  {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!root_ || root_->proven != PV_NONE) return;   // solved: nothing to do
    Node* n = root_;
    path.push_back(n);
    for (;;) {
      if (n->proven != PV_NONE) break;
      if (!n->movesGenerated) {
        if (!n->visitedOnce) {
          n->visitedOnce = true;            // first visit: evaluate this node
          break;
        }
        expand(n, sc);                      // second visit: discover the moves
        if (n->proven != PV_NONE) break;    // ... node turned out terminal
      }
      // Moves generated and non-terminal: PUCT-select a continuation.
      Pick pick = selectChild(n, nodeCount_ < MAX_NODES);
      if (pick.untriedIdx >= 0) {
        // Materialise the chosen move; the new child becomes the next leaf.
        size_t i = size_t(pick.untriedIdx);
        Move  mv  = n->untried[i];
        float pri = n->untriedPriors[i];
        n->untried[i]       = n->untried.back();     // swap-pop the chosen move
        n->untriedPriors[i] = n->untriedPriors.back();
        n->untried.pop_back();
        n->untriedPriors.pop_back();
        Node* child = pool_.alloc();
        ++nodeCount_;
        child->parent = n;
        child->move   = mv;
        child->prior  = pri;
        child->pos    = n->pos;
        doMove(child->pos, mv);
        n->children.push_back(child);
        ++child->virtualLoss;
        path.push_back(child);
        n = child;
        continue;
      }
      if (!pick.child) break;               // pool full, nothing to descend to
      n = pick.child;
      ++n->virtualLoss;
      path.push_back(n);
    }
    leaf = n;
    leafProven = (leaf->proven != PV_NONE);   // capture under the lock
    if (leafProven) result = provenValueBlack(leaf);
  }

  if (!leafProven) result = evalLeaf(leaf->pos, sc);

  {
    std::lock_guard<std::mutex> lk(mtx_);
    for (Node* x : path) {
      ++x->visits;
      x->valueBlack += result;
    }
    for (size_t i = 1; i < path.size(); ++i) --path[i]->virtualLoss;
    // MCTS-Solver: propagate any proven value up the path.  A node only newly
    // proves once its path-child is proven, so stop at the first unproven one.
    for (size_t i = path.size(); i-- > 0; ) {
      Node* x = path[i];
      if (x->proven == PV_NONE) tryProve(x);
      if (x->proven == PV_NONE) break;
    }
    if (root_->proven != PV_NONE)
      solved_.store(true, std::memory_order_relaxed);
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
  if (root_->proven != PV_NONE)
    s.blackWinProb = provenValueBlack(root_);
  else if (root_->visits > 0)
    s.blackWinProb = root_->valueBlack / root_->visits;
  // Walk the best child at each step to build the PV.
  Node* n = root_;
  while (n && !n->children.empty()) {
    Node* best = bestChild(n);
    if (!best || best->visits == 0) break;
    if (n == root_) s.bestMove = best->move;
    s.pv.push_back(best->move);
    n = best;
    if (s.pv.size() > 16) break;
  }
  return s;
}

}  // namespace shogi
