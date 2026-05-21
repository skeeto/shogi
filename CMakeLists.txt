cmake_minimum_required(VERSION 3.16)
project(shogi CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# --- SDL3 -------------------------------------------------------------------
# Prefer an installed SDL3; otherwise fetch and build it.
find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found locally - fetching via FetchContent")
  include(FetchContent)
  FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.8/SDL3-3.4.8.tar.gz
    URL_HASH SHA256=e9fff7467fb60f037e6708da18b25560649e4c63edc2a69bb871b960d9cbfbba
  )
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
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

find_package(Threads REQUIRED)
target_link_libraries(shogi PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(shogi PRIVATE /W3)
else()
  target_compile_options(shogi PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
