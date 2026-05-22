// main.cpp - Program entry point.
#include "ui.hpp"

// SDL_main.h must be included by the one translation unit that defines main().
// SDL then supplies whatever entry point the target platform needs - e.g.
// WinMain for the Windows GUI subsystem - and forwards to the main() below.
#include <SDL3/SDL_main.h>

int main(int /*argc*/, char** /*argv*/) {
  return shogi::runGame();
}
