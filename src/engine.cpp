// engine.cpp - Background search manager.
#include "engine.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>

namespace shogi {

#ifndef __EMSCRIPTEN__
namespace {
// SplitMix64 finalizer - scrambles a counter into a well-spread 64-bit seed.
uint64_t mixSeed(uint64_t z) {
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
}  // namespace
#endif

Engine::Engine(int threads, double cpuct) {
#ifdef __EMSCRIPTEN__
  (void)threads;
  nThreads_ = 1;                       // single-threaded: no SharedArrayBuffer
#else
  int hw = int(std::thread::hardware_concurrency());
  if (hw <= 0) hw = 2;
  nThreads_ = (threads > 0) ? threads : std::max(1, hw - 1);
#endif
  mcts_.cpuct = cpuct;
}

Engine::~Engine() { stop(); }

void Engine::startWorkers() {
#ifndef __EMSCRIPTEN__
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
#endif
}

void Engine::stop() {
#ifndef __EMSCRIPTEN__
  run_ = false;
  for (std::thread& t : workers_)
    if (t.joinable()) t.join();
  workers_.clear();
#endif
}

void Engine::pump(int ms) {
#ifdef __EMSCRIPTEN__
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(ms);
  do {
    for (int i = 0; i < 128; ++i) mcts_.iterate(rng_);
  } while (std::chrono::steady_clock::now() < deadline);
#else
  (void)ms;
#endif
}

void Engine::setPosition(const Position& p) {
  stop();                 // join all workers before touching the tree
  mcts_.setRoot(p);
  startWorkers();
}

void Engine::advance(const Move& m, const Position& p) {
  stop();
  mcts_.advance(m, p);
  startWorkers();
}

}  // namespace shogi
