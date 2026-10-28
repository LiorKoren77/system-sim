# All third-party code comes through CPM with exact tags, so a checkout builds the
# same bits everywhere. CPM downloads at configure time into build/_deps.
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

CPMAddPackage(
  NAME yaml-cpp
  GITHUB_REPOSITORY jbeder/yaml-cpp
  GIT_TAG 0.8.0
  OPTIONS "YAML_CPP_BUILD_TESTS OFF" "YAML_CPP_BUILD_TOOLS OFF" "YAML_CPP_BUILD_CONTRIB OFF"
  SYSTEM YES)   # SYSTEM: its headers are treated as system headers, so our -Werror does not fire on them

CPMAddPackage(
  NAME CLI11
  GITHUB_REPOSITORY CLIUtils/CLI11
  GIT_TAG v2.4.2
  SYSTEM YES)

CPMAddPackage(
  NAME Catch2
  GITHUB_REPOSITORY catchorg/Catch2
  GIT_TAG v3.7.1
  SYSTEM YES)
