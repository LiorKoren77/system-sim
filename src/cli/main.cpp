// `simsys` command-line entry point. Parsing is delegated to CLI11; commands are thin
// wrappers over library calls so they stay testable without a process.
#include <CLI/CLI.hpp>

#include <print>

// C++20 `import`: brings the exported names of a named module into this translation
// unit. Unlike #include, it does not re-parse text; the compiler reads a compiled
// module interface (BMI). Imports go after the #includes because GCC 14 reports
// redefinition errors when a std header is #included after importing a module that
// included the same header in its global module fragment.
import simsys.core;

int main(int argc, char** argv) {
  CLI::App app{"simsys - a fluid-flow system design simulator"};
  app.set_version_flag("--version", std::string{simsys::version});
  app.require_subcommand(0, 1);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);  // prints help / version / error exactly as CLI11 intends
  }

  if (app.get_subcommands().empty()) {
    // C++23 std::println: type-safe formatted output to stdout, replacing printf's
    // format-string/argument mismatch bugs and iostream's verbosity. The format
    // string is checked at compile time.
    std::println("simsys {} - run `simsys --help`", simsys::version);
  }
  return 0;
}
