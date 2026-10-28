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

# GCC 14.2 + named modules: each import maps the imported module's whole line table (its
# global-module-fragment headers included) into this translation unit's source-location
# space. A unit at the end of a deep import chain (simsys.engine imports behaviours,
# graph, fluid, flow, model, core ...) runs out of it ("unable to represent further
# imported source locations") and the compiler then crashes in write_location while
# writing the module. -flarge-source-files gives up some column precision in diagnostics
# for a much larger line range. Verified 2026-10-28; every module unit needs the same
# setting, so it applies to all of our targets.
add_compile_options(-flarge-source-files)

# One warning set for all of our targets (never applied to third-party code).
add_library(simsys_warnings INTERFACE)
target_compile_options(simsys_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

# --- library: one named module per layer ------------------------------------
add_library(simsys)
target_sources(simsys
  PUBLIC FILE_SET CXX_MODULES FILES     # FILE_SET CXX_MODULES is how CMake 3.28 declares module interface units
    src/core/core.cppm
    src/config/config.cppm
    src/queueing/queueing.cppm
    src/flow/flow.cppm
    src/fluid/fluid.cppm
    src/model/model.cppm
    src/defaults/defaults.cppm
    src/engine/graph.cppm
    src/engine/behaviours.cppm
    src/engine/engine.cppm
  PRIVATE                               # module implementation units are ordinary sources
    src/config/config.cpp)
target_link_libraries(simsys PUBLIC yaml-cpp::yaml-cpp PRIVATE simsys_warnings)

# --- executable --------------------------------------------------------------
add_executable(simsys_cli src/cli/main.cpp)
set_target_properties(simsys_cli PROPERTIES OUTPUT_NAME simsys)
target_link_libraries(simsys_cli PRIVATE simsys CLI11::CLI11 simsys_warnings)

# --- tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
