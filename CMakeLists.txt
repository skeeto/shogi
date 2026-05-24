cmake_minimum_required(VERSION 3.16...3.30)
project(shogi VERSION 1.0 LANGUAGES CXX)

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
# Default native build: prefer an installed SDL3, else fetch and build a
# shared SDL3.  Emscripten and -DSHOGI_STATIC=ON instead always fetch SDL3 and
# build it statically - a self-contained binary, no SDL3 runtime dependency.
# A static build skips find_package: an installed SDL3 is typically shared.
option(SHOGI_STATIC "Fetch and statically link SDL3" OFF)
if(EMSCRIPTEN OR SHOGI_STATIC)
  set(shogi_sdl_static ON)
else()
  set(shogi_sdl_static OFF)
endif()

if(NOT shogi_sdl_static)
  find_package(SDL3 QUIET CONFIG)
endif()
if(NOT SDL3_FOUND)
  message(STATUS "SDL3: fetching via FetchContent (static=${shogi_sdl_static})")
  include(FetchContent)
  FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.8/SDL3-3.4.8.tar.gz
    URL_HASH SHA256=e9fff7467fb60f037e6708da18b25560649e4c63edc2a69bb871b960d9cbfbba
  )
  if(shogi_sdl_static)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
  else()
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  endif()
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

# WIN32: link the Windows build for the GUI ("windows") subsystem so it opens
# with no console window.  SDL supplies the WinMain entry point (see
# src/ui.cpp).  MACOSX_BUNDLE makes the macOS build a proper Shogi.app so it
# carries its own icon (for Dock and Cmd-Tab) and version metadata rather
# than appearing as a generic terminal binary.  Both flags are silently
# ignored on platforms they don't apply to.
add_executable(shogi WIN32 MACOSX_BUNDLE
  src/board.cpp
  src/mate.cpp
  src/mcts.cpp
  src/engine.cpp
  src/ui.cpp
)
target_include_directories(shogi PRIVATE src)
target_link_libraries(shogi PRIVATE SDL3::SDL3)
target_compile_definitions(shogi PRIVATE SHOGI_VERSION="${PROJECT_VERSION}")

# Windows: compile the gold-general application icon (src/shogi.ico) and a
# VERSIONINFO block (file/product version, etc.) into the executable.  The
# .rc is configure_file()'d from src/shogi.rc.in so PROJECT_VERSION
# substitutes cleanly, avoiding the quoting headaches of passing strings
# through windres -D.  windres locates the .ico via the src/ include path.
if(WIN32)
  enable_language(RC)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/shogi.rc.in
                 ${CMAKE_CURRENT_BINARY_DIR}/shogi.rc @ONLY)
  target_sources(shogi PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shogi.rc)
endif()

# macOS: drop src/shogi.icns into Shogi.app/Contents/Resources/ and point
# the Info.plist at it.  CMake auto-generates a minimal Info.plist from the
# MACOSX_BUNDLE_* properties (name, version, GUI identifier, icon file).
if(APPLE)
  set(_shogi_icns "${CMAKE_CURRENT_SOURCE_DIR}/src/shogi.icns")
  target_sources(shogi PRIVATE "${_shogi_icns}")
  set_source_files_properties("${_shogi_icns}" PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")
  set_target_properties(shogi PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME           "Shogi"
    MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
    MACOSX_BUNDLE_GUI_IDENTIFIER        "com.nullprogram.shogi"
    MACOSX_BUNDLE_ICON_FILE             "shogi.icns")
endif()

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

# --- Standalone HTML tutorial ----------------------------------------------
# Builds a single-file build/tutorial.html (markdown + base64-embedded PNGs)
# as part of the normal `cmake --build` pipeline.  Default ON; opt out with
# -DSHOGI_BUILD_DOCS=OFF for builds where the doc isn't needed.
#
# Cross-builds (e.g. mingw, Emscripten) can't run the cross-compiled genhtml
# on the build machine, so we look for a separate host C++ compiler and use
# it to build genhtml.  If none is found we emit a warning and skip the doc
# rather than failing the build.
option(SHOGI_BUILD_DOCS "Build the standalone HTML tutorial" ON)

if(SHOGI_BUILD_DOCS)
  set(_shogi_genhtml_src "${CMAKE_CURRENT_SOURCE_DIR}/tools/genhtml.cpp")
  set(_shogi_tutorial_md "${CMAKE_CURRENT_SOURCE_DIR}/docs/tutorial/tutorial.md")
  set(_shogi_tutorial_html "${CMAKE_CURRENT_BINARY_DIR}/tutorial.html")
  file(GLOB _shogi_tutorial_imgs
       "${CMAKE_CURRENT_SOURCE_DIR}/docs/tutorial/img/*.png")

  if(CMAKE_CROSSCOMPILING)
    # Cross-build: the cross compiler (mingw / emscripten / ...) produces a
    # binary that can't run on the build machine.  Find a host C++ compiler
    # to compile genhtml separately.
    find_program(SHOGI_HOST_CXX NAMES c++ g++ clang++
                 DOC "Host C++ compiler for tools/genhtml.cpp (cross-build)")
    if(SHOGI_HOST_CXX)
      set(_shogi_genhtml "${CMAKE_CURRENT_BINARY_DIR}/genhtml")
      add_custom_command(
        OUTPUT "${_shogi_genhtml}"
        COMMAND "${SHOGI_HOST_CXX}" -O2 -std=c++17
                "${_shogi_genhtml_src}" -o "${_shogi_genhtml}"
        DEPENDS "${_shogi_genhtml_src}"
        COMMENT "Building genhtml (host C++)"
        VERBATIM)
      set(_shogi_genhtml_dep "${_shogi_genhtml}")
    else()
      message(WARNING
        "SHOGI_BUILD_DOCS=ON but no host C++ compiler found on PATH "
        "(looked for c++, g++, clang++).  Skipping tutorial.html.  "
        "Pass -DSHOGI_BUILD_DOCS=OFF to silence this warning, or "
        "-DSHOGI_HOST_CXX=/path/to/host/c++ to set one explicitly.")
      set(SHOGI_BUILD_DOCS OFF)
    endif()
  else()
    # Native build: use the project's own compiler.  Don't install the tool -
    # it's a build artefact only.
    add_executable(genhtml tools/genhtml.cpp)
    set(_shogi_genhtml "$<TARGET_FILE:genhtml>")
    set(_shogi_genhtml_dep genhtml)
  endif()
endif()

if(SHOGI_BUILD_DOCS)
  add_custom_command(
    OUTPUT "${_shogi_tutorial_html}"
    COMMAND "${_shogi_genhtml}" "${_shogi_tutorial_html}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS "${_shogi_genhtml_dep}"
            "${_shogi_tutorial_md}"
            ${_shogi_tutorial_imgs}
    COMMENT "Generating tutorial.html"
    VERBATIM)
  add_custom_target(tutorial_html ALL DEPENDS "${_shogi_tutorial_html}")
endif()

# --- Packaging --------------------------------------------------------------
# A simple ZIP bundling the shogi executable and tutorial.html, produced via
# `cmake --build <build> --target package` (or `cpack` directly).  Designed
# primarily for the mingw cross-build's Windows .exe + tutorial.html bundle,
# but works on any native target too.
#
# Emscripten is skipped: web/build.sh assembles build-web/ for that path,
# including tutorial.html (see the explicit copy in that script).
if(NOT EMSCRIPTEN)
  # BUNDLE handles the macOS Shogi.app; RUNTIME handles the bare binary on
  # every other platform.
  install(TARGETS shogi RUNTIME DESTINATION . BUNDLE DESTINATION .)
  if(SHOGI_BUILD_DOCS)
    install(FILES "${_shogi_tutorial_html}" DESTINATION .)
  endif()

  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_NAME "shogi")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Shogi - Japanese chess with an MCTS AI")
  set(CPACK_VERBATIM_VARIABLES YES)
  # Make Windows builds (incl. the mingw cross-build) read as "win64" rather
  # than the verbose CMake default "Windows-AMD64".
  if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CPACK_SYSTEM_NAME "win64")
  endif()
  # Flat layout: shogi(.exe) and tutorial.html at the ZIP root, no extra dir.
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

  include(CPack)
endif()
