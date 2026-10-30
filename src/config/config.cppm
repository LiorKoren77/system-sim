// Module `simsys.config`, interface unit: `load`, which reads a scenario file into the
// typed `model::Scenario` (module simsys.model) and checks it structurally: document shape,
// required keys, node types, duplicate or empty ids, node references and unit strings.
// That is enough to machine-check scenario drafts before the engine exists; behaviour
// parameters (eviction policy, quorum sizes, ...) are left to the engine. The checks
// themselves live in the implementation units: config.cpp (`load` and the pipeline), one unit
// per concern (header.cpp, nodes.cpp, steps.cpp, workload.cpp, links.cpp, slos.cpp,
// overrides.cpp) and detail.hpp / detail.cpp for what they share.
//
// Compilation firewall: interface declares, implementation units define; the importers' BMI
// does not change when only an implementation unit does. With CMake 3.31 an edit there
// recompiles that unit alone; with CMake 3.28.3 (the system CMake; 3.28 is the minimum) every
// importer recompiles anyway, because 3.28 rewrites every unit's module map after any edit
// (docs/study/modules.md). This is a build technique, not a GoF design pattern, hence no
// `Pattern:` tag.
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
  // The rule broken, without the construct `message` names first ("over must fit in the run
  // after warmup (...)" where the message reads "slo: over must fit ..."): what a caller that
  // knows better what to blame puts after its own prefix (`run --duration 30m: ...`). Equal to
  // `message` when the message names no construct.
  std::string rule{};
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

// One replaced value (`simsys sweep`): the scalar at the dot path `path` (`nodes.users.rate.base`:
// map keys from the document's root down, never a list index) is replaced by the text `value`
// (`500/s`, in the file's own unit syntax) before validation. A value under a YAML anchor (`&a`) is
// one node shared with every alias (`*a`) to it: replacing it through either path replaces it
// everywhere the anchor is used.
struct Override {
  std::string path;
  std::string value;
};

// `load`, with each override applied to the parsed YAML before any check runs, so a replaced
// value is validated exactly like one written in the file and the engine never sees a value the
// loader did not check. A path that does not lead to one scalar of the file (a missing key, a
// list on the way, a map or a list at its end) is a ConfigError whose message starts with
// "--param: " and which has no line (the mistake is on the command line, not in the file).
// No overrides gives what load(path) gives.
// C++20 std::span<const T>: a non-owning view of contiguous elements, so the caller passes a
// vector, an array or `{}` without a copy and without making `load` a template.
[[nodiscard]] std::expected<model::Scenario, ConfigError> load(const std::string& path,
                                                               std::span<const Override> overrides);

// Whether `override_path` leads to one scalar of the file at `path`, without validating the
// scenario: the path's mistake once, before any value is tried (`simsys sweep`). The same errors
// as `load` with an Override of that path.
[[nodiscard]] std::expected<void, ConfigError> check_override_path(const std::string& path,
                                                                   std::string_view override_path);

// The keys of a dot path, in order: "nodes.users.rate.base" -> {nodes, users, rate, base}. Empty
// pieces are kept ("a..b" has an empty middle key, which no map holds). `simsys sweep` splits its
// metric names with it too.
[[nodiscard]] std::vector<std::string> dot_path_keys(std::string_view path);

// The mistakes a dot path makes on its own, before any file is read: an empty key, or a key of
// digits only, which could only be a list index (a dot path goes through maps only: a list entry
// has no name a path could say). Nothing when it makes neither; whether its keys exist is
// check_override_path's question.
[[nodiscard]] std::optional<std::string> dot_path_problem(std::string_view path);

// Whether the loader reads the key at `override_path` as a whole number (`version`, `seed`, a
// node's `workers`, `replicas`, `keys`, ..., an `advanced` block's `concurrency` or `queue_limit`,
// a client's `keys.space`), judged by where the key sits, not by its name alone: an op share
// called `workers` is still a fraction.
[[nodiscard]] bool counts_whole_things(std::string_view override_path);

// Compiler-style `file:line:column: error: message`, so editors can jump to it; without the
// position when the line is unknown (0: a missing file, an override's path).
[[nodiscard]] std::string describe(const ConfigError& error);

}  // namespace simsys::config
