// `simsys` command-line entry point. Parsing is delegated to CLI11; commands are thin
// wrappers over library calls so they stay testable without a process.
#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <print>

// C++20 `import`: brings the exported names of a named module into this translation
// unit. Unlike #include, it does not re-parse text; the compiler reads a compiled
// module interface (BMI). Imports go after the #includes because GCC 14 reports
// redefinition errors when a std header is #included after importing a module that
// included the same header in its global module fragment.
import simsys.core;
import simsys.config;

namespace {

// `simsys validate <file>`: structural check of one scenario. Errors use the
// compiler-style `file:line:column: error: message` form so editors can jump to them.
int run_validate(const std::filesystem::path& file) {
  const auto scenario = simsys::config::load(file);
  if (!scenario) {
    const auto& error = scenario.error();
    // C++23 std::println: type-safe formatted output (here to stderr), replacing
    // printf's format-string/argument mismatch bugs and iostream's verbosity. The
    // format string is checked at compile time.
    std::println(stderr, "{}:{}:{}: error: {}", error.file, error.line, error.column,
                 error.message);
    return 1;
  }
  std::println("OK: {} ({} nodes, {} links)", scenario->name, scenario->nodes.size(),
               scenario->links.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"simsys - a fluid-flow system design simulator"};
  app.set_version_flag("--version", std::string{simsys::version});
  app.require_subcommand(1);

  std::filesystem::path scenario_file;
  auto* validate = app.add_subcommand("validate", "check a scenario's types, references and units");
  validate->add_option("file", scenario_file, "scenario YAML file")
      ->required()
      ->check(CLI::ExistingFile);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);  // prints help / version / error exactly as CLI11 intends
  }

  if (validate->parsed()) return run_validate(scenario_file);
  return 0;
}
