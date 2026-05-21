cmake_minimum_required(VERSION 3.16)
project(shogi CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# --- Emscripten: optional pthreads (multi-threaded WebAssembly) -------------
# pthreads must be consistent across every object, so it is applied
# directory-wide (covering the fetched SDL3) before anything else is set up.
# web/build.sh builds the project twice - once with SHOGI_PTHREAD and once
# without - and a loader picks the right one per browser at runtime.
if(EMSCRIPTEN)
  option(SHOGI_PTHREAD "Build the WebAssembly target with pthreads" OFF)
  if(SHOGI_PTHREAD)
    add_compile_options(-pthread)
    add_link_options(-pthread -sPTHREAD_POOL_SIZE=8)
  endif()
endif()

# --- SDL3 -------------------------------------------------------------------
# Prefer an installed SDL3; otherwise fetch and build it.  Emscripten always
# fetches and builds SDL3 statically.
find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found locally - fetching via FetchContent")
  include(FetchContent)
  FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.8/SDL3-3.4.8.tar.gz
    URL_HASH SHA256=e9fff7467fb60f037e6708da18b25560649e4c63edc2a69bb871b960d9cbfbba
  )
  if(EMSCRIPTEN)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
  else()
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  endif()
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

add_executable(shogi
  src/main.cpp
  src/board.cpp
  src/mcts.cpp
  src/engine.cpp
  src/ui.cpp
)
target_include_directories(shogi PRIVATE src)
target_link_libraries(shogi PRIVATE SDL3::SDL3)

if(MSVC)
  target_compile_options(shogi PRIVATE /W3)
else()
  target_compile_options(shogi PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(EMSCRIPTEN)
  # Emit <name>.js + <name>.wasm; web/index.html is the loader/shell.  A
  # pthreads build also runs code in Web Workers, so it needs ENVIRONMENT to
  # include "worker".
  if(SHOGI_PTHREAD)
    set_target_properties(shogi PROPERTIES OUTPUT_NAME "shogi-mt" SUFFIX ".js")
    set(SHOGI_WEB_ENV "web,worker")
  else()
    set_target_properties(shogi PROPERTIES OUTPUT_NAME "shogi-st" SUFFIX ".js")
    set(SHOGI_WEB_ENV "web")
  endif()
  target_link_options(shogi PRIVATE
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=4MB
    -sENVIRONMENT=${SHOGI_WEB_ENV})
else()
  find_package(Threads REQUIRED)
  target_link_libraries(shogi PRIVATE Threads::Threads)
endif()
