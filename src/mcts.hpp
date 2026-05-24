// mcts.h - Incremental, thread-safe Monte-Carlo Tree Search.
//
// A single shared tree is grown by many worker threads.  Selection, expansion
// and back-propagation run under one mutex; the (much longer) leaf evaluation
// runs lock-free.  Virtual loss steers concurrent threads onto distinct paths.
//
// Children are materialised lazily: a node carries the full legal-move list
// with priors, and PUCT selection ranges over both the realised children and
// the not-yet-realised moves (a "virtual child" valued at the FPU estimate).
// A move only becomes a Node when PUCT picks it, so a search never spends an
// evaluation on a move it would not have explored.
#pragma once
#include "board.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace shogi {

// Game-theoretic status of a node, from its own side-to-move's perspective.
// A node with no legal move is a proven loss; the MCTS-Solver propagates
// proven values up the tree from there.
enum Proven : uint8_t { PV_NONE = 0, PV_WIN, PV_LOSS, PV_DRAW };

struct Node {
  Move  move;                       // move from parent that produced this node
  Node* parent = nullptr;
  std::vector<Node*> children;      // materialised children only
  std::vector<Move>  untried;       // legal moves not yet materialised
  std::vector<float> untriedPriors; // policy priors, parallel to `untried`
  Position pos;                     // position AT this node
  int     visits     = 0;
  double  valueBlack = 0.0;         // summed results, Black's perspective [0,1]
  int     virtualLoss = 0;
  double  prior      = 1.0;         // policy prior of the move into this node
  bool     visitedOnce    = false;  // has been an iteration's leaf at least once
  bool     movesGenerated = false;  // legal moves discovered into `untried`
  uint8_t  proven = PV_NONE;        // solved game value, or PV_NONE if unknown
  uint16_t provenDepth = 0;         // plies to the proven result (mate distance)
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
    n->visitedOnce = false;
    n->movesGenerated = false;
    n->proven = PV_NONE;
    n->provenDepth = 0;
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

  // Resets the tree to start searching from `p`.  `history` holds the Zobrist
  // hashes of every position from the game's start through `p` inclusive, so
  // the search can detect sennichite (fourfold repetition).
  void setRoot(const Position& p, const std::vector<uint64_t>& history);

  // Advances the root by `m`, reusing the matching child's subtree if it
  // exists (so search done while pondering is not thrown away); otherwise
  // starts fresh from `newPos`.  `history` is as for setRoot.
  void advance(const Move& m, const Position& newPos,
               const std::vector<uint64_t>& history);

  // Runs exactly one MCTS iteration.  Safe to call from many threads; each
  // caller passes its own Scratch (reused buffers, never shared).  `rng` is
  // unused (the quiescence leaf eval is deterministic) but kept for now.
  void iterate(uint64_t& rng, Scratch& sc);

  struct Stats {
    Move   bestMove     = nullMove();
    double blackWinProb = 0.5;       // Black's estimated win probability
    int    rootVisits   = 0;
    std::vector<Move> pv;            // principal variation
    bool   bestChildOverride = false;// value-tiebreak flipped vs visits-only
  };
  Stats snapshot();
  int   rootVisits();

  // True once the root's game value is proven by the MCTS-Solver: the search
  // can stop, there is nothing left to learn.  Lock-free, cheap to poll.
  bool  solved() const { return solved_.load(std::memory_order_relaxed); }

  double cpuct = 2.0;               // PUCT exploration constant (tunable)

  // bestChild() at root pick: when the top two children are within
  // closeRatio of each other in visit count, prefer the one with higher
  // STM-relative win prob if the gap exceeds valueMargin.  Defaults tuned
  // by A/B (200-game abprev at budget 4000) - a tighter ratio was too
  // sparse and produced no measurable effect; these values produce ~1.5
  // overrides/game and gain ~30 Elo + ~6 plies shorter won games.
  // Set bestChildUseValue = false to fall back to the visit-only
  // AlphaZero default.
  double closeRatio        = 0.50;
  double valueMargin       = 0.01;
  bool   bestChildUseValue = true;

 private:
  // PUCT pick: either an existing child (untriedIdx < 0) or an index into
  // n->untried to be materialised (child == nullptr).
  struct Pick { Node* child = nullptr; int untriedIdx = -1; };
  Pick  selectChild(Node* n, bool canMaterialize);
  void  expand(Node* n, Scratch& sc);
  void  freeTree(Node* n);
  bool  isRepetition(const Node* n, const Scratch& sc) const;   // fourfold
  void  applyRootMate(const Move& m, int mateLen);  // inject a df-pn mate

  std::mutex mtx_;
  NodePool pool_;
  Node*  root_      = nullptr;
  size_t nodeCount_ = 0;
  std::atomic<bool> solved_{false};
  std::atomic<bool> dfpnStarted_{false};            // df-pn pass claimed
  std::unordered_map<uint64_t, int> historyCount_;  // pre-search hash counts
};

}  // namespace shogi
