// engine.cpp - Background search manager.
#include "engine.hpp"
#include <algorithm>
#include <cstdint>
#include <random>

namespace shogi {

namespace {
// SplitMix64 finalizer - scrambles a counter into a well-spread 64-bit seed.
uint64_t mixSeed(uint64_t z) {
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
}  // namespace

Engine::Engine(int threads) {
  int hw = int(std::thread::hardware_concurrency());
  if (hw <= 0) hw = 2;
  nThreads_ = (threads > 0) ? threads : std::max(1, hw - 1);
}

Engine::~Engine() { stop(); }

void Engine::startWorkers() {
  run_ = true;
  workers_.reserve(nThreads_);
  std::random_device rd;
  uint64_t base = (uint64_t(rd()) << 32) ^ uint64_t(rd());
  for (int i = 0; i < nThreads_; ++i) {
    uint64_t seed = mixSeed(base + uint64_t(i));
    workers_.emplace_back([this, seed] {
      uint64_t rng = seed;            // each worker owns its RNG state
      while (run_.load(std::memory_order_relaxed)) mcts_.iterate(rng);
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
