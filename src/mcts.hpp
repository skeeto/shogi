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
  Node*  root_      = nullptr;
  size_t nodeCount_ = 0;
};

}  // namespace shogi
