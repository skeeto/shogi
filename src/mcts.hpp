// mcts.h - Incremental, thread-safe Monte-Carlo Tree Search.
//
// A single shared tree is grown by many worker threads.  Selection, expansion
// and back-propagation run under one mutex; the (much longer) random rollout
// runs lock-free.  Virtual loss steers concurrent threads onto distinct paths.
#pragma once
#include "board.hpp"
#include <mutex>
#include <vector>

namespace shogi {

struct Node {
  Move  move;                       // move from parent that produced this node
  Node* parent = nullptr;
  std::vector<Node*> children;
  std::vector<Move>  untried;       // legal moves not yet expanded
  std::vector<float> untriedPriors; // policy priors, parallel to `untried`
  Position pos;                     // position AT this node
  int    visits      = 0;
  double valueBlack   = 0.0;        // summed results, Black's perspective [0,1]
  int    virtualLoss = 0;
  double prior       = 1.0;         // policy prior of the move into this node
  bool   expanded    = false;
  bool   terminal    = false;
  double terminalBlack = 0.0;
};

// Slab allocator for Nodes.  Nodes are carved from large slabs and recycled
// through a free-list (threaded through the unused `parent` of a free node),
// so growing the tree costs no per-node malloc once warm.  Recycled nodes keep
// their vectors' capacity, so re-expanding them allocates nothing either.
//
// Memory is bounded by the peak tree size (MAX_NODES) but is held until the
// pool is destroyed rather than returned between moves.  Not thread-safe: the
// MCTS mutex serialises every pool access.
class NodePool {
 public:
  ~NodePool() {
    for (Node* slab : slabs_) delete[] slab;
  }
  Node* alloc() {
    if (free_) {                          // recycle a freed node
      Node* n = free_;
      free_ = n->parent;
      reset(n);
      return n;
    }
    if (slabIdx_ >= SLAB) {               // carve from a fresh slab
      slabs_.push_back(new Node[SLAB]);
      slabIdx_ = 0;
    }
    return &slabs_.back()[slabIdx_++];
  }
  void free(Node* n) {
    n->parent = free_;                    // `parent` doubles as the list link
    free_ = n;
  }

 private:
  static void reset(Node* n) {
    n->move = Move{};
    n->parent = nullptr;
    n->children.clear();                  // clear() keeps capacity for reuse
    n->untried.clear();
    n->untriedPriors.clear();
    n->visits = 0;
    n->valueBlack = 0.0;
    n->virtualLoss = 0;
    n->prior = 1.0;
    n->expanded = false;
    n->terminal = false;
    n->terminalBlack = 0.0;
    // `pos` is overwritten by the caller right after alloc().
  }
  static constexpr size_t SLAB = 8192;
  std::vector<Node*> slabs_;
  size_t slabIdx_ = SLAB;                 // SLAB => first alloc() makes a slab
  Node*  free_ = nullptr;
};

class MCTS {
 public:
  MCTS() = default;
  ~MCTS();

  // Resets the tree to start searching from `p`.
  void setRoot(const Position& p);

  // Advances the root by `m`, reusing the matching child's subtree if it
  // exists (so search done while pondering is not thrown away); otherwise
  // starts fresh from `newPos`.
  void advance(const Move& m, const Position& newPos);

  // Runs exactly one MCTS iteration.  Safe to call from many threads; each
  // caller passes its own Scratch (reused buffers, never shared).  `rng` is
  // unused (the quiescence leaf eval is deterministic) but kept for now.
  void iterate(uint64_t& rng, Scratch& sc);

  struct Stats {
    Move   bestMove     = nullMove();
    double blackWinProb = 0.5;       // Black's estimated win probability
    int    rootVisits   = 0;
    std::vector<Move> pv;            // principal variation
  };
  Stats snapshot();
  int   rootVisits();

  double cpuct = 2.0;               // PUCT exploration constant (tunable)

 private:
  Node* selectChild(Node* n);
  void  expand(Node* n, Scratch& sc);
  void  freeTree(Node* n);

  std::mutex mtx_;
  NodePool pool_;
  Node*  root_      = nullptr;
  size_t nodeCount_ = 0;
};

}  // namespace shogi
