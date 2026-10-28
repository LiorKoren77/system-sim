// Module `simsys.config`, interface unit: `load`, which reads a scenario file into the
// typed `model::Scenario` (module simsys.model) and checks it structurally: document shape,
// required keys, node types, duplicate or empty ids, node references and unit strings.
// That is enough to machine-check scenario drafts before the engine exists; behaviour
// parameters (eviction policy, quorum sizes, ...) are left to the engine. The checks
// themselves live in the implementation unit, config.cpp.
//
// Compilation firewall: interface declares, implementation unit defines; importers never
// recompile when only config.cpp changes. This is a build technique, not a GoF design
// pattern, hence no `Pattern:` tag.
//
// Why this interface is lean (verified 2026-10-28 with GCC 14.2): a module interface unit
// that `import`s another module must not include, in its global module fragment, any
// header that pulls in <format> (<chrono>, <filesystem>, <format>, <print>), even if it
// never calls std::format. GCC 14.2 then fails with "Bad file data ... failed to load
// pendings for std::formatter". Such headers go in the implementation unit instead.
//
// C++20 named modules: `module;` opens the global module fragment, where headers are
// #included so their declarations stay in the global module (shareable with other
// translation units) instead of being attached to `simsys.config`.
module;
#include <expected>
#include <string>

export module simsys.config;

// C++20 `import`: makes the exported names of `simsys.core` (SimTime, the unit
// parsers) visible here without re-parsing any header text.
import simsys.core;
// The scenario vocabulary `load` fills. The loader, the engine and the tests share these
// types, so there is no second, loader-only Scenario to keep in sync.
import simsys.model;

export namespace simsys::config {

// A mistake in a scenario file, located the way compilers report errors so editors can
// jump to it: file:line:column, both 1-based.
struct ConfigError {
  std::string file;
  int line{};  // int, like yaml-cpp's Mark; 0 when the location is unknown
  int column{};
  std::string message;
};

// Reads and validates a scenario file. Every problem, including YAML syntax errors and a
// missing file, comes back as a ConfigError; nothing throws. The path is a std::string,
// not a std::filesystem::path, because <filesystem> may not appear in this interface (see
// the GCC 14.2 note at the top of the file). A path converts to std::string implicitly
// only on POSIX, where its native string type is std::string; on Windows it is
// std::wstring, so portable callers pass path.string().
// C++23 std::expected<T, E>: a value-or-error return type. It replaces the choice between
// exceptions and out-parameters; the caller must inspect it, and the error carries
// structured data (file, line, column, message) instead of a bare code.
[[nodiscard]] std::expected<model::Scenario, ConfigError> load(const std::string& path);

}  // namespace simsys::config
