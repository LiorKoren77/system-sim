cmake_minimum_required(VERSION 3.28)   # 3.28 is the first release with stable named-module support
project(system-sim VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++23, not gnu++23
set(CMAKE_CXX_SCAN_FOR_MODULES ON)     # let CMake discover `export module` / `import` dependencies
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "system-sim needs GCC >= 14 (std::expected, std::generator, named modules). "
                      "Clang 18 cannot build it; use it for clang-format/clang-tidy only.")
endif()

include(cmake/Dependencies.cmake)

# One warning set for all of our targets (never applied to third-party code).
add_library(simsys_warnings INTERFACE)
target_compile_options(simsys_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
# GCC 14.2 internal compiler error "in ipa_comdats, at ipa-comdats.cc:355" at -O2/-O3 (the
# Release preset): a translation unit that imports simsys.engine and gets Graph::build,
# Simulation::create and step inlined into main() crashes the compiler (7-line
# reproducer: see the Lessons of the phase 1 plan). -O1 and -O0 are fine. Of the IPA
# passes turned off one at a time (-fno-ipa-icf, -fno-ipa-cp, -fno-inline-functions,
# -fno-ipa-sra) only IPA-SRA, interprocedural scalar replacement of aggregates, avoids
# it, at -O2 and -O3. The cost: some by-reference struct parameters stay unsplit.
# Verified 2026-10-28; the gcc14-release CI job guards it.
target_compile_options(simsys_warnings INTERFACE -fno-ipa-sra)

# --- library: one named module per layer ------------------------------------
add_library(simsys)
target_sources(simsys
  PUBLIC FILE_SET CXX_MODULES FILES     # FILE_SET CXX_MODULES is how CMake 3.28 declares module interface units
    src/core/core.cppm
    src/config/config.cppm
    src/queueing/queueing.cppm
    src/flow/flow.cppm
    src/fluid/fluid.cppm
    src/workload/workload.cppm
    src/caching/caching.cppm
    src/model/model.cppm
    src/defaults/defaults.cppm
    src/engine/graph.cppm
    src/engine/behaviours.cppm
    src/engine/faults.cppm               # interface partition simsys.engine:faults (engine.cppm re-exports it)
    src/engine/engine.cppm
    src/report/report.cppm
    src/report/shared.cppm               # implementation partition simsys.report:shared: a BMI too
    src/session/session.cppm
    src/sweep/sweep.cppm
  PRIVATE                               # module implementation units are ordinary sources
    src/config/config.cpp
    src/config/detail.cpp
    src/config/header.cpp
    src/config/nodes.cpp
    src/config/steps.cpp
    src/config/workload.cpp
    src/config/links.cpp
    src/config/slos.cpp
    src/config/faults.cpp
    src/config/overrides.cpp
    src/engine/graph.cpp
    src/model/model.cpp
    src/report/explain.cpp
    src/report/report.cpp
    src/session/session.cpp
    src/sweep/sweep.cpp)
# std::jthread (simsys.sweep's workers) needs the platform's thread library; Threads::Threads is
# CMake's portable name for it (-pthread with GCC on Linux).
find_package(Threads REQUIRED)
target_link_libraries(simsys PUBLIC yaml-cpp::yaml-cpp Threads::Threads PRIVATE simsys_warnings)
# GCC 14.2 + named modules: each import maps the imported module's whole line table (its
# global-module-fragment headers included) into this translation unit's source-location
# space. The module units at the end of the import chain, from simsys.behaviours on
# (engine and report import even more), run out of it ("unable to represent further
# imported source locations") and the compiler then crashes in write_location while
# writing the module. -flarge-source-files raises the number of source lines GCC can
# track before it stops tracking columns, at a small compile-time and memory cost
# (graph.cppm: 1.91 s -> 1.98 s, 332 MB -> 366 MB). Only the library's module units need
# it: the tests and the CLI import the modules but export none, and compile without it.
# Verified 2026-10-28.
target_compile_options(simsys PRIVATE -flarge-source-files)
# GCC 14.2 at -O2/-O3: a unit importing simsys.model calls compiler-generated special
# members of std templates over model types (std::optional<Workload>'s move constructor,
# the NodeSpec::ops map's destructor) out of line without emitting them, and once the
# optimiser has inlined every call in the module's own objects no object defines them:
# undefined references at link time. model.cpp calls each of them; -fno-inline keeps those
# calls out of line, so that object file defines them (see its header comment). -fno-inline
# for the whole build is not an option: it crashes GCC in ipa_comdats on report.cpp, and
# -fkeep-inline-functions crashes it in the gimplifier. Verified 2026-10-28.
set_source_files_properties(src/model/model.cpp PROPERTIES COMPILE_OPTIONS -fno-inline)

# --- executable --------------------------------------------------------------
add_executable(simsys_cli src/cli/main.cpp)
set_target_properties(simsys_cli PROPERTIES OUTPUT_NAME simsys)
target_link_libraries(simsys_cli PRIVATE simsys CLI11::CLI11 simsys_warnings)

# --- tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
