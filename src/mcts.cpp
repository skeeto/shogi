// mcts.cpp - Incremental MCTS implementation.
#include "mcts.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace shogi {

namespace {
constexpr double EXPLORATION = 1.3;       // UCT constant: explore vs. exploit
constexpr size_t MAX_NODES   = 700000;    // tree-size ceiling
}  // namespace

MCTS::~MCTS() { freeTree(root_); }

void MCTS::freeTree(Node* n) {
  if (!n) return;
  for (Node* c : n->children) freeTree(c);
  delete n;
}

void MCTS::setRoot(const Position& p) {
  std::lock_guard<std::mutex> lk(mtx_);
  freeTree(root_);
  root_ = new Node();
  root_->pos = p;
  nodeCount_ = 1;
}

// Expand: discover the legal moves from `n`, or flag it terminal.
void MCTS::expand(Node* n) {
  std::vector<Move> moves;
  generateLegalMoves(n->pos, moves);
  n->expanded = true;
  if (moves.empty()) {
    // Side to move has no legal reply -> that side loses.
    n->terminal = true;
    n->terminalBlack = (n->pos.stm() == BLACK) ? 0.0 : 1.0;
  } else {
    n->untried = std::move(moves);
  }
}

// UCT child selection from the perspective of the side to move at `n`.
Node* MCTS::selectChild(Node* n) {
  const bool blackToMove = (n->pos.stm() == BLACK);
  const double logParent = std::log(double(n->visits + n->virtualLoss + 1));
  Node* best = nullptr;
  double bestScore = -1e18;
  for (Node* c : n->children) {
    int vis = c->visits + c->virtualLoss;
    if (vis == 0) return c;                       // never-tried child first
    // Each pending virtual loss counts as a loss for the side choosing here.
    double vb = c->valueBlack + (blackToMove ? 0.0 : double(c->virtualLoss));
    double meanBlack = vb / vis;
    double exploit = blackToMove ? meanBlack : (1.0 - meanBlack);
    double explore = EXPLORATION * std::sqrt(logParent / double(vis));
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
        Node* child = new Node();
        ++nodeCount_;
        child->parent = n;
        child->move = mv;
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
