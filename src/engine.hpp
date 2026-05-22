// engine.hpp - Background search manager.
//
// Owns the MCTS tree and the search workers.  On a normal (threaded) build a
// pool of worker threads searches continuously - so the engine also "ponders"
// while a human is thinking, which feeds move suggestions and the strength
// bar.  On a single-threaded build (Emscripten/WebAssembly, where there is no
// SharedArrayBuffer) there are no worker threads; the caller drives the search
// by calling pump() each frame instead.
#pragma once
#include "mcts.hpp"
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// A single-threaded build: Emscripten compiled without pthreads (i.e. without
// SharedArrayBuffer).  Native builds and the threaded wasm build search with a
// worker pool; this build searches inline via Engine::pump().
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define SHOGI_NO_THREADS 1
#endif

namespace shogi {

class Engine {
 public:
  explicit Engine(int threads = 0, double cpuct = 2.0);  // 0 -> auto-detect
  ~Engine();

  // Switches the search to position `p` (resets the tree).  `history` holds
  // the position hashes from the game start through `p` for repetition
  // detection; callers that do not track it may pass none.
  void setPosition(const Position& p,
                   const std::vector<uint64_t>& history = {});

  // Advances the search by move `m` to position `p`, reusing the pondered
  // subtree for `m` when possible.  `history` is as for setPosition.
  void advance(const Move& m, const Position& p,
               const std::vector<uint64_t>& history = {});

  // Stops all worker threads (no-op on a single-threaded build).
  void stop();

  // Single-threaded builds: run the search for about `ms` milliseconds on the
  // calling thread.  No-op on a threaded build (the workers do the searching).
  void pump(int ms);

  MCTS::Stats stats() { return mcts_.snapshot(); }
  int  visits()       { return mcts_.rootVisits(); }
  bool solved()       { return mcts_.solved(); }   // root game value proven
  int  threadCount() const { return nThreads_; }

 private:
  void startWorkers();

  MCTS mcts_;
  std::vector<std::thread> workers_;
  std::atomic<bool> run_{false};
  int nThreads_;
#ifdef SHOGI_NO_THREADS
  uint64_t rng_ = 0x9e3779b97f4a7c15ULL;   // search RNG for the pump() path
  Scratch  scratch_;                       // reused search buffers for pump()
#endif
};

}  // namespace shogi
