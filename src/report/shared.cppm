// Module implementation partition `simsys.report:shared`: the helpers both implementation units of
// simsys.report need, and nothing else: number formatting (durations included), the run's clock,
// the zone names and the aligned text table. report.cpp (summaries, SLOs, table, JSON, CSV) and
// explain.cpp (`simsys explain`) both `import :shared;`.
//
// Why a partition and not a header (src/report/detail.hpp, `inline` functions included in both
// global module fragments): a header is pasted into each unit and compiled twice, and its helpers
// would belong to no module (the global module), so anything that included it could call them. A
// partition is compiled once, its names are attached to simsys.report and have module linkage:
// visible in every unit of simsys.report that imports it, invisible outside, without `export`.
// GCC 14.2 builds and links it as written in both presets, with none of the CLAUDE.md §3
// workarounds (verified 2026-10-29), so the header fallback was not needed. It imports no part of
// simsys.report: the standard allows a partition to import the primary interface, but GCC 14.2
// then refuses every unit that imports the partition ("cannot import module in its own purview";
// Clang 18 accepts it, verified 2026-10-30), so what the exported format_duration(double) prints
// is computed here, by duration_text, and report.cpp's exported overload returns it.
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
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
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
// the number formatters from core, Zone from fluid, a node's spec and its ops' costs from model and
// defaults: the unit a node counts its work in).
import simsys.core;
import simsys.defaults;
import simsys.fluid;
import simsys.model;

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

// Seconds in the largest unit that keeps the number >= 1, three significant digits:
// 0.0005 -> "500us", 120 -> "2m". Infinity means the node never answers: "unavailable". The body
// of the exported format_duration(double) (report.cppm), which returns it: this partition does not
// import the interface (GCC 14.2; see the top of the file), so it cannot see the exported
// declaration, and the shared overloads below call this.
[[nodiscard]] std::string duration_text(double seconds_value) {
  if (!std::isfinite(seconds_value)) return "unavailable";
  if (seconds_value == 0.0) return "0s";
  for (const auto& unit : duration_units) {
    if (std::abs(seconds_value) / unit.size >= rounds_to_one) {
      return significant(seconds_value / unit.size, 3) + std::string{unit.name};
    }
  }
  return significant(seconds_value / 1e-9, 3) + "ns";  // below one nanosecond
}

// The same for a SimTime (module linkage: not exported, but an overload of the exported name, so
// one name serves both units).
[[nodiscard]] std::string format_duration(SimTime t) {
  return duration_text(seconds(t));
}

// From here on the largest prefix, G, would need an exponent itself ("1e+04G").
constexpr double beyond_prefixes = 1e4 * si_prefixes.front().size;

// 0.99995 of a prefix already prints as "1" at four significant digits, so it is shown with that
// prefix: 999.99 reads "1k", not "1000" (rounds_to_one's rule for durations, one digit more). A
// rate summed over steps of 0.1 s lands there: 20 steps of 1000/s average 999.9999999999998/s.
constexpr double rounds_to_one_at_four_digits = 0.99995;

// A count with an SI prefix and four significant digits: 500, 1.5k, 2M. Past 9999G it is
// written in plain scientific notation (1e+15), which parse_rate reads back too; G stays
// the largest prefix, as in the scenario grammar (core's rate_prefixes stop at G).
[[nodiscard]] std::string with_si_prefix(double value) {
  if (!std::isfinite(value)) return "unavailable";
  if (std::abs(value) >= beyond_prefixes) return significant(value);
  for (const auto& prefix : si_prefixes) {
    if (std::abs(value) >= prefix.size * rounds_to_one_at_four_digits) {
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
// 1e6) up to that million too, so such a room is printed as the million it rounds to, "1M". A
// room without end (+infinity: the pool of a URL frontier, graph::Graph::room_of) is "unbounded".
[[nodiscard]] std::string fmt_room(double units) {
  if (std::isinf(units)) return "unbounded";
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
// C++20 std::span<const T>: a non-owning read-only view of contiguous elements (pointer + size),
// so a caller passes a vector or an array of parts without a copy. Replaces a
// `const std::vector<T>&` parameter, which accepts only a vector.
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

// Does `c` start a UTF-8 character? Every byte does but a continuation byte, 0b10xxxxxx.
[[nodiscard]] bool starts_a_character(char c) {
  return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
}

// How many columns `text` takes on a terminal: its UTF-8 code points, not its bytes. The en dash
// of a replication's band (`1.62ms [1.61–1.63]`) is three bytes and one column. Every character
// the simulator itself prints is one column wide, so for them code points are columns; a wide
// character in a node id (CJK, an emoji) or a combining mark still misaligns its row (a true
// width needs the Unicode East Asian Width tables, wcwidth, which this does not carry).
[[nodiscard]] std::size_t display_width(std::string_view text) {
  // C++20 std::ranges::count_if: counts over the whole range, replacing a begin/end pair.
  return static_cast<std::size_t>(std::ranges::count_if(text, starts_a_character));
}

[[nodiscard]] std::vector<std::size_t> column_widths(std::span<const Row> rows) {
  std::vector<std::size_t> widths;
  for (const auto& row : rows) {
    if (widths.size() < row.size()) widths.resize(row.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
      widths[i] = std::max(widths[i], display_width(row[i].text));
    }
  }
  return widths;
}

[[nodiscard]] std::string padded(const std::string& text, std::size_t width, bool right) {
  const std::string fill(width - display_width(text), ' ');
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

// ---- the run's clock ----

[[nodiscard]] std::string two_digits(std::int64_t value) {
  return (value < 10 ? "0" : "") + std::to_string(value);
}

// A point in simulated time as a scenario writes it: "20:00" for a whole minute of the first day (t
// = 0 is midnight, spec §17 entry 1), otherwise a duration ("36h", "90s"). Integer nanoseconds, so
// 19:59:59.9 is never rounded to "20:00". explain.cpp prints a wave's peak with it.
[[nodiscard]] std::string clock_or_duration(SimTime t) {
  constexpr auto minute_ns = (1_min).count();
  constexpr auto day_ns = (24_h).count();
  const auto ns = t.count();
  if (ns < 0 || ns >= day_ns || ns % minute_ns != 0) return format_duration(t);
  const auto minutes = ns / minute_ns;
  return two_digits(minutes / 60) + ":" + two_digits(minutes % 60);
}

// A window's start on the run's clock: explain's HH:MM for a whole minute of the first day,
// HH:MM:SS for a whole second (a warmup of 1199 s starts the first window at 00:19:59, which
// clock_or_duration would round to "20m"), else the exact duration from the start of the run
// ("1500ms"; "1502m" for 25:02, past the first day). report.cpp prints an SLO window's start with
// it.
[[nodiscard]] std::string window_clock(SimTime t) {
  constexpr auto second_ns = (1_s).count();
  constexpr auto day_ns = (24_h).count();
  const auto ns = t.count();
  if (ns < 0 || ns >= day_ns || ns % second_ns != 0) return describe_duration(t);
  if (ns % (60 * second_ns) == 0) return clock_or_duration(t);
  const auto total_s = ns / second_ns;
  return two_digits(total_s / 3600) + ":" + two_digits(total_s / 60 % 60) + ":" +
         two_digits(total_s % 60);
}

// A moment of a scripted fault on the run's clock (the report's fault block): window_clock's HH:MM
// for a whole minute and HH:MM:SS for a whole second, otherwise HH:MM:SS and a tenth, rounded to
// the nearest tenth (`00:00:20.2`, where window_clock would print the exact duration, `20200ms`): a
// fault and its recovery are moments of the day, read beside the rate curve's clock. Past the
// first day, the exact duration, as window_clock.
[[nodiscard]] std::string fault_clock(SimTime t) {
  constexpr auto tenth_ns = (100_ms).count();
  constexpr auto day_ns = (24_h).count();
  const auto ns = t.count();
  if (ns < 0 || ns >= day_ns) return describe_duration(t);
  const auto tenths = (ns + tenth_ns / 2) / tenth_ns;
  if (tenths % 10 == 0) return window_clock(SimTime{tenths * tenth_ns});
  const auto total_s = tenths / 10;
  return two_digits(total_s / 3600) + ":" + two_digits(total_s / 60 % 60) + ":" +
         two_digits(total_s % 60) + "." + std::to_string(tenths % 10);
}

// A span of seconds in the fault block's body: at most one decimal, none when it is a whole number
// (`0.2s`, `12.1s`, `10s`, `0s`): the precision of a 100 ms step, without a `.0` that says nothing.
[[nodiscard]] std::string tenths_of_seconds(double seconds_value) {
  std::string text = to_chars_text(seconds_value, std::chars_format::fixed, 1);
  // C++20 std::string::ends_with: a suffix test in one call, replacing
  // `s.size() >= n && s.compare(s.size() - n, n, suffix) == 0`.
  if (text.ends_with(".0")) text.resize(text.size() - 2);
  if (text == "-0") text = "0";  // a residue below zero rounds to "-0.0"
  return text + "s";
}

// ---- JSON (run and estimate reports, the resolved graph, the stream's lines) ----

// Quotes and backslashes are escaped, control characters become \u00XX; everything
// else (UTF-8 included) passes through, as JSON allows.
[[nodiscard]] std::string json_string(std::string_view text) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string out = "\"";
  for (const char c : text) {
    const unsigned code = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (code < 0x20) {
      out += "\\u00";
      out += hex[code >> 4];
      out += hex[code & 0xFu];
    } else {
      out += c;
    }
  }
  return out + '"';
}

// JSON has no infinity: a latency that never arrives is null.
[[nodiscard]] std::string json_number(double value) {
  return std::isfinite(value) ? to_chars_text(value) : "null";
}

[[nodiscard]] std::string json_optional_number(const std::optional<double>& value) {
  return value ? json_number(*value) : "null";
}

[[nodiscard]] std::string json_optional_string(const std::optional<std::string>& text) {
  return text ? json_string(*text) : "null";
}

[[nodiscard]] std::string json_field(std::string_view name, const std::string& value) {
  return json_string(name) + ": " + value;
}

[[nodiscard]] std::string json_object(std::span<const std::string> fields) {
  return "{" + join(fields, ", ") + "}";
}

// `"name": [ ... ]` at the top level, one object per line.
[[nodiscard]] std::string json_array(std::string_view name, std::span<const std::string> objects) {
  if (objects.empty()) return "  " + json_field(name, "[]");
  return "  " + json_field(name, "[\n    " + join(objects, ",\n    ") + "\n  ]");
}

// C++20 std::views::transform + C++23 std::ranges::to: map every element through `f` and
// collect the results into a vector, replacing a reserve + push_back loop.
template <class T, class F>
[[nodiscard]] std::vector<std::string> map_to_strings(std::span<const T> items, F f) {
  return items | std::views::transform(f) | std::ranges::to<std::vector<std::string>>();
}

// A document of simsys's JSON output: `format_version` first (the output contract's version,
// report::format_version, written here once for every document), then the given lines, one
// top-level key each. The version is passed in: this partition cannot see the interface's
// exported constant (the top of the file says why).
[[nodiscard]] std::string json_document(int version, std::span<const std::string> lines) {
  return "{\n  " + json_field("format_version", std::to_string(version)) + ",\n" +
         join(lines, ",\n") + "\n}";
}

// A JSON array of strings: `["a", "b"]`, from any range of things a string_view takes.
template <class Names>
[[nodiscard]] std::string json_strings(const Names& names) {
  std::vector<std::string> quoted;
  for (const auto& name : names) quoted.push_back(json_string(name));
  return "[" + join(quoted, ", ") + "]";
}

// ---- the unit of a node's work ----

// "delete, insert, update, write cost 5" ("a cost 2 and b cost 3" for two costs): a node's ops that
// cost something other than 1 capacity unit, grouped by their cost, in name order; "" when every op
// it prices costs 1. The ops priced are a database's write ops (their default cost of 5, spec §18
// entry 7) and every op of a stated `cost` map (spec §17 entry 4), each priced as the engine
// prices it (defaults::op_cost).
[[nodiscard]] std::string priced_ops(const model::NodeSpec& spec, double unit_per_request = 1.0) {
  // C++20 std::views::keys: the keys of a map as a lazy view, no copy of its pairs; C++23
  // std::ranges::to<std::vector>(): collects a view into a container, replacing a reserve +
  // push_back loop.
  std::vector<std::string> ops{spec.cost | std::views::keys | std::ranges::to<std::vector>()};
  if (spec.type == model::NodeType::sql_db) {
    std::ranges::copy(model::write_ops_of(spec), std::back_inserter(ops));
  }
  // C++20 std::ranges::unique: returns the tail to erase as a subrange (unpacked by a structured
  // binding), where std::unique returned only its first iterator.
  std::ranges::sort(ops);
  const auto [first_repeat, end] = std::ranges::unique(ops);
  ops.erase(first_repeat, end);
  std::map<double, std::vector<std::string>> by_cost;
  for (const auto& op : ops) {
    const double cost = defaults::op_cost(spec, op) / unit_per_request;
    if (cost != 1.0) by_cost[cost].push_back(op);
  }
  std::vector<std::string> groups;
  for (const auto& [cost, named] : by_cost) {
    groups.push_back(join(named, ", ") + " cost " + significant(cost));
  }
  return join(groups, " and ");
}

// What a node's peak_backlog and drops count, when it is not requests: megabytes at a byte-sized
// node (spec §19 entry 2), replica-requests at a kv_cluster (N of them per request, §19 entry 9),
// capacity units at a node with an op that costs other than 1 (priced_ops); empty otherwise, and
// for a client, which queues nothing.
[[nodiscard]] std::optional<std::string> unit_of(const model::NodeSpec& spec) {
  if (spec.type == model::NodeType::client) return std::nullopt;
  if (defaults::is_byte_sized(spec)) return "MB";
  if (spec.type == model::NodeType::kv_cluster) {
    // Every request is N replica-requests; a stated `cost` prices each of them on top (op_cost is
    // the product), so the line says both: "replica-requests (N 3; put cost 4 each)".
    const int n = model::kv_replication(spec);
    const auto priced = priced_ops(spec, static_cast<double>(n));
    const std::string each = priced.empty() ? "" : "; " + priced + " each";
    return "replica-requests (N " + std::to_string(n) + each + ")";
  }
  const auto priced = priced_ops(spec);
  if (priced.empty()) return std::nullopt;
  return "capacity units (" + priced + ")";
}

// The unit of a node's work as one word, for programs (the graph JSON's `unit`): "MB" at a
// byte-sized node, "replica-requests" at a kv_cluster, "units" at a node with an op that costs
// other than 1 (unit_of's capacity units), "requests" otherwise; empty for a client, which serves
// nothing. unit_of says the same in words for people, with the costs.
[[nodiscard]] std::optional<std::string_view> unit_word(const model::NodeSpec& spec) {
  if (spec.type == model::NodeType::client) return std::nullopt;
  if (defaults::is_byte_sized(spec)) return "MB";
  if (spec.type == model::NodeType::kv_cluster) return "replica-requests";
  return priced_ops(spec).empty() ? "requests" : "units";
}

}  // namespace simsys::report
