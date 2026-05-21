// engine.cpp - Background search manager.
#include "engine.hpp"
#include <algorithm>

namespace shogi {

Engine::Engine(int threads) {
  int hw = int(std::thread::hardware_concurrency());
  if (hw <= 0) hw = 2;
  nThreads_ = (threads > 0) ? threads : std::max(1, hw - 1);
}

Engine::~Engine() { stop(); }

void Engine::startWorkers() {
  run_ = true;
  workers_.reserve(nThreads_);
  for (int i = 0; i < nThreads_; ++i) {
    workers_.emplace_back([this] {
      while (run_.load(std::memory_order_relaxed)) mcts_.iterate();
    });
  }
}

void Engine::stop() {
  run_ = false;
  for (std::thread& t : workers_)
    if (t.joinable()) t.join();
  workers_.clear();
}

void Engine::setPosition(const Position& p) {
  stop();                 // join all workers before touching the tree
  mcts_.setRoot(p);
  startWorkers();
}

}  // namespace shogi
