// Every scenario shipped under scenarios/ must validate. The directory is injected by
// CMake as SIMSYS_SCENARIOS_DIR: a compile definition is the one macro we accept, because
// it carries a build-time path that no language feature can.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion. It comes after the #includes because GCC 14 reports redefinitions
// when a std header is included after importing a module that included it in its
// global module fragment.
import simsys.config;

TEST_CASE("every shipped scenario validates") {
  namespace fs = std::filesystem;
  int count = 0;
  for (const auto& entry : fs::recursive_directory_iterator(SIMSYS_SCENARIOS_DIR)) {
    if (entry.path().extension() != ".yaml") continue;
    ++count;
    DYNAMIC_SECTION(entry.path().lexically_relative(SIMSYS_SCENARIOS_DIR).string()) {
      auto scenario = simsys::config::load(entry.path().string());
      if (!scenario) {
        const auto& e = scenario.error();
        FAIL(e.file << ":" << e.line << ":" << e.column << ": " << e.message);
      }
      CHECK(scenario->nodes.size() >= 2);
      CHECK(!scenario->links.empty());
    }
  }
  CHECK(count >= 1);
}
