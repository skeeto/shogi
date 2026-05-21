// engine.h - Background search manager.
//
// Owns the MCTS tree and a pool of worker threads that search continuously.
// The search runs on whatever position is current, so it also "ponders" while
// a human is thinking - which is what feeds the move suggestions and the
// position-strength bar.
#pragma once
#include "mcts.hpp"
#include <atomic>
#include <thread>
#include <vector>

namespace shogi {

class Engine {
 public:
  explicit Engine(int threads = 0, double cpuct = 2.0);  // 0 -> auto-detect
  ~Engine();

  // Switches the search to position `p` (stops, resets the tree, restarts).
  void setPosition(const Position& p);

  // Advances the search by move `m` to position `p`, reusing the pondered
  // subtree for `m` when possible.
  void advance(const Move& m, const Position& p);

  // Stops all worker threads.
  void stop();

  MCTS::Stats stats() { return mcts_.snapshot(); }
  int  visits()       { return mcts_.rootVisits(); }
  int  threadCount() const { return nThreads_; }

 private:
  void startWorkers();

  MCTS mcts_;
  std::vector<std::thread> workers_;
  std::atomic<bool> run_{false};
  int nThreads_;
};

}  // namespace shogi
