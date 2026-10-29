// Module implementation partition `simsys.report:shared`: the helpers both implementation units of
// simsys.report need, and nothing else: number formatting, the zone names, the aligned text table,
// and the declarations of the few helpers that are defined in one of the two units and called from
// the other (the run's clock, the SimTime overload of format_duration). report.cpp (summaries,
// SLOs, table, JSON, CSV) and explain.cpp (`simsys explain`) both `import :shared;`.
//
// Why a partition and not a header (src/report/detail.hpp, `inline` functions included in both
// global module fragments): a header is pasted into each unit and compiled twice, and its helpers
// would belong to no module (the global module), so anything that included it could call them. A
// partition is compiled once, its names are attached to simsys.report and have module linkage:
// visible in every unit of simsys.report that imports it, invisible outside, without `export`.
// GCC 14.2 builds and links it as written in both presets, with none of the CLAUDE.md §3
// workarounds (verified 2026-10-29), so the header fallback was not needed.
//
// Every number is formatted with std::to_chars (simsys::to_chars_text), never with a std::format
// precision such as "{:.3f}": with GCC 14.2, a precision in a format spec throws std::format_error
// at run time in a unit that imports simsys.core (CLAUDE.md §3, second <format> defect).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this unit imports other modules and the
// importers read its compiled interface (GCC 14.2 'Bad file data' bug, CLAUDE.md §3).
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 module partition: `module simsys.report:shared;` names a piece of the module
// simsys.report. Without `export` it is an implementation partition: a part of the module
// that other units of simsys.report bring in with `import :shared;` (the module name is
// implied) and that nobody outside the module can import. It replaces a private header shared
// by several .cpp files, compiled once instead of once per includer. docs/study/modules.md.
module simsys.report:shared;

// C++20 `import`: the compiled interfaces of the modules this partition reads from (SimTime and
// the number formatters from core, Zone from fluid).
import simsys.core;
import simsys.fluid;

namespace simsys::report {

// ---- number formatting ----

// Every number is written by simsys::to_chars_text (simsys.core), mostly through
// simsys::significant (a few significant digits), never by a std::format precision (see the top of
// the file).

// A unit and its size in the base unit (seconds, or one request).
struct Unit {
  std::string_view name;
  double size;
};

// Pattern: Table-driven method — the units are data searched by one function each, the
// same grammar the scenario files use ("250us", "1.5k/s"), largest first.
constexpr std::array duration_units{Unit{"h", 3600.0}, Unit{"m", 60.0},  Unit{"s", 1.0},
                                    Unit{"ms", 1e-3},  Unit{"us", 1e-6}, Unit{"ns", 1e-9}};
constexpr std::array si_prefixes{Unit{"G", 1e9}, Unit{"M", 1e6}, Unit{"k", 1e3}};

// 0.9995 of a unit already prints as "1" at three significant digits, so it is shown in
// that unit: 999.7 us reads "1ms", not "1e+03us".
constexpr double rounds_to_one = 0.9995;

// From here on the largest prefix, G, would need an exponent itself ("1e+04G").
constexpr double beyond_prefixes = 1e4 * si_prefixes.front().size;

// A count with an SI prefix and four significant digits: 500, 1.5k, 2M. Past 9999G it is
// written in plain scientific notation (1e+15), which parse_rate reads back too; G stays
// the largest prefix, as in the scenario grammar (core's rate_prefixes stop at G).
[[nodiscard]] std::string with_si_prefix(double value) {
  if (!std::isfinite(value)) return "unavailable";
  if (std::abs(value) >= beyond_prefixes) return significant(value);
  for (const auto& prefix : si_prefixes) {
    if (std::abs(value) >= prefix.size) {
      return significant(value / prefix.size) + std::string{prefix.name};
    }
  }
  return significant(value);
}

// A count of things (keys) with an SI prefix, as with_si_prefix — 500, 1.5k, 2M, 9999G — but
// past the largest prefix in plain digits (20000000000000) rather than scientific notation: a
// count, unlike a rate, is a whole number a reader compares digit by digit, and the scenario
// grammar writes counts with k/M/G or digits only (config's parse_count).
[[nodiscard]] std::string fmt_count(double value) {
  if (!std::isfinite(value) || std::abs(value) < beyond_prefixes) return with_si_prefix(value);
  // No std::format precision: see the top.
  return to_chars_text(value, std::chars_format::fixed, 0);
}

// A queue's waiting room: as written up to six digits (1000, 12000, 500000), and from a million on
// with an SI prefix, four digits (a pool's room is retention × its rate: 7 days of 6.667/s is
// 4.032M, of 1.2k/s 725.8M), where six digits would print "4.032e+06". Six digits round [999,999.5,
// 1e6) up to that million too, so such a room is printed as the million it rounds to, "1M".
[[nodiscard]] std::string fmt_room(double units) {
  constexpr double million = 1e6;
  constexpr double rounds_to_a_million = million - 0.5;
  if (std::abs(units) < rounds_to_a_million) return significant(units, 6);
  return fmt_count(std::abs(units) < million ? std::copysign(million, units) : units);
}

[[nodiscard]] std::string fmt_rate(double per_second) {
  return with_si_prefix(per_second) + "/s";
}

// A ratio as a percentage, three significant digits (12.3%, 0.048%), and from 1000 % on in whole
// percents (10000%), where three significant digits would need an exponent, "1e+04%". 999.5 and up
// rounds to four digits, so that is where it switches.
[[nodiscard]] std::string percent(double ratio) {
  const double value = ratio * 100.0;
  if (value >= 999.5) return to_chars_text(value, std::chars_format::fixed, 0) + "%";
  return significant(value, 3) + "%";
}

// ---- enum names ----

[[nodiscard]] std::string_view zone_name(fluid::Zone zone) {
  // C++20 `using enum`: brings the enumerators into this function's scope, so the cases
  // read `green` instead of `fluid::Zone::green`; nothing leaks outside the function.
  using enum fluid::Zone;
  switch (zone) {
    case green:
      return "green";
    case yellow:
      return "yellow";
    case red:
      return "red";
  }
  // C++23 std::unreachable(): every enumerator returned above (-Wswitch, an error under
  // -Werror, proves it), so this point is unreachable. Replaces a dummy return value
  // that would hide a missing case, and silences -Wreturn-type.
  std::unreachable();
}

// ---- small text helpers ----

// C++23 std::views::join_with + std::ranges::to: joins the parts with a separator and
// collects the characters into a std::string in one pipeline. Replaces a hand-written
// loop that appends a separator before every part but the first.
[[nodiscard]] std::string join(std::span<const std::string> parts, std::string_view separator) {
  return parts | std::views::join_with(separator) | std::ranges::to<std::string>();
}

// One cell of an aligned text table; `colour` is an ANSI escape, or empty.
struct Cell {
  std::string text{};
  std::string_view colour{};
};
using Row = std::vector<Cell>;

enum class Align { first_left_rest_right, all_left };

constexpr std::string_view ansi_reset = "\x1b[0m";

[[nodiscard]] std::vector<std::size_t> column_widths(std::span<const Row> rows) {
  std::vector<std::size_t> widths;
  for (const auto& row : rows) {
    if (widths.size() < row.size()) widths.resize(row.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].text.size());
    }
  }
  return widths;
}

[[nodiscard]] std::string padded(const std::string& text, std::size_t width, bool right) {
  const std::string fill(width - text.size(), ' ');
  return right ? fill + text : text + fill;
}

// Every column padded to its widest cell (names left, numbers right by default), two
// spaces between columns, colour codes wrapped around the padded text so they do not
// count towards the width. Every line starts with `indent` (the tables' two spaces under their
// heading); trailing blanks are trimmed.
[[nodiscard]] std::string aligned(std::span<const Row> rows,
                                  Align align = Align::first_left_rest_right,
                                  std::string_view indent = "  ") {
  const auto widths = column_widths(rows);
  std::string out;
  for (const auto& row : rows) {
    std::string line{indent};
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) line += "  ";
      const bool right = align == Align::first_left_rest_right && i > 0;
      const auto cell = padded(row[i].text, widths[i], right);
      line += row[i].colour.empty() ? cell
                                    : std::string{row[i].colour} + cell + std::string{ansi_reset};
    }
    line.erase(line.find_last_not_of(' ') + 1);
    out += line + '\n';
  }
  return out;
}

// ---- defined in one unit, called from the other ----

// A partition can only declare what needs the module's exported interface (format_duration(double)
// is exported from report.cppm, which no partition can import), so these are defined beside their
// callers, each with its own comment: format_duration(SimTime) in report.cpp, the run's clock in
// explain.cpp (with the rate curve whose peak it prints) and called from report.cpp's SLO windows.
[[nodiscard]] std::string format_duration(SimTime t);
[[nodiscard]] std::string clock_or_duration(SimTime t);
[[nodiscard]] std::string window_clock(SimTime t);

}  // namespace simsys::report
