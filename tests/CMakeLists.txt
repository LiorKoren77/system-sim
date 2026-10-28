# Tests are teaching material: name each TEST_CASE after the property it proves.
# Targets are added in Task 2; an empty source file would be rejected by -Wpedantic
# ("ISO C++ forbids an empty translation unit"), so there is nothing to build yet.
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(unit_tests unit/test_units.cpp)
target_link_libraries(unit_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
catch_discover_tests(unit_tests)
