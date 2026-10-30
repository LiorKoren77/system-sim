// Module `simsys.config`, implementation unit: a client's workload, the traffic it offers. The
// `rate:` block (level or `points` trace, wave, ramp, spikes, noise), the op shares, the
// `keys:` distribution, the request `sizes`, the `timeout` and the `retries` with their
// `retry_after`.
module;
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.config;` (no `export`) makes this file
// part of the named module. It sees everything the interface declared, exports nothing,
// and can include heavy headers in its own global module fragment, like a .cpp behind a
// header.
module simsys.config;

import simsys.core;
// Only for defaults::retry_after: the wait a client with `retries` and no `retry_after` gets.
import simsys.defaults;
// Only for fluid::Capacity's default zone thresholds (detail.hpp): one source of truth for
// 0.7 / 0.9.
import simsys.fluid;
import simsys.model;
// The rate curve types a client's `rate:` block is read into.
import simsys.workload;

// The loader's shared vocabulary and common readers, included in the module purview after
// the imports: its declarations belong to simsys.config, like this unit's own, and can
// name ConfigError (see the header's comment for why it is no module partition).
#include "detail.hpp"

namespace simsys::config::detail {

// ---- Client workload: the traffic a client offers -------------------------------------
//
// Models the user population of the book's back-of-the-envelope estimates (ch. 2): a
// request rate (DAU x requests per user / 86 400 s) and the mix of operations those
// requests are (the read/write ratio, e.g. 10:1 for the URL shortener, ch. 8). Phase 2a
// types the whole `rate:` block (spec §8): the level (`base`, or a `points` trace), the
// multiplicative terms `wave`, `ramp` and `spikes` (simsys.workload has the formulas), and
// the seeded `noise` process. The range rules below keep every term's multiplier finite
// and non-negative, so no rate the engine computes from them can be negative or infinite.

// Unnamed namespace: the helpers in these blocks are this unit's own (internal linkage).
// Outside it a function of a module implementation unit has module linkage, so every unit
// of simsys.config could call it and the linker sees its name; only what detail.hpp declares
// is shared between the units.
namespace {

constexpr std::string_view rate_keys = "base wave spikes ramp points noise";
// The keys of each curve block; every one of them is required.
constexpr std::string_view wave_keys = "shape period amplitude peak_at";
constexpr std::string_view ramp_keys = "from to over";
constexpr std::string_view spike_keys = "at for factor";
constexpr std::string_view noise_keys = "sigma correlation";

// The tolerance of "the shares sum to 1": decimal shares are not exact in binary floating
// point (0.1 + 0.2 != 0.3), so the sum is compared within 1e-6.
constexpr double share_sum_tolerance = 1e-6;

using OpShare = std::pair<std::string, double>;
using OpShares = std::vector<OpShare>;

// A rate no larger than max_rate_per_second (see there); `label` names it ("rate.base",
// "rate.ramp.to", "rate.points[3]").
std::expected<Rate, ConfigError> check_rate_bounded(const Site& site, const YAML::Node& at,
                                                    std::string_view label, Rate rate) {
  if (rate.per_second <= max_rate_per_second) return rate;
  return site.fail(at, std::format("{} must be at most 1e15/s", label));
}

}  // namespace

std::expected<Rate, ConfigError> read_bounded_rate(const Site& site, const YAML::Node& parent,
                                                   std::string_view key, std::string_view label) {
  return parse_key(site, parent, key, label, parse_rate).and_then([&](Rate rate) {
    return check_rate_bounded(site, parent[std::string{key}], label, rate);
  });
}

namespace {

// The words of a space-separated list, in order.
std::vector<std::string> words_of(std::string_view words) {
  std::vector<std::string> parts;
  // C++20 std::views::split: a lazy view of the pieces between separators, replacing a
  // hand-written find/substr loop.
  for (const auto piece : words | std::views::split(' '))
    parts.emplace_back(piece.begin(), piece.end());
  return parts;
}

// "a, b and c" from the words "a b c": the required keys of a curve block, as its
// "needs ..." message lists them.
std::string spoken_list(std::string_view words) {
  const auto parts = words_of(words);
  std::string text = parts.front();
  for (std::size_t i = 1; i < parts.size(); ++i) {
    text += i + 1 == parts.size() ? " and " : ", ";
    text += parts[i];
  }
  return text;
}

// A curve block (`rate.wave`, `rate.spikes[2]`, ...) is a map that has exactly `keys`: none
// unknown ("unknown rate.wave key 'colour'"), none missing ("rate.wave needs shape, period,
// amplitude and peak_at"). Every key is required because no default would be a safe guess
// (how long is the period of a wave written without one?).
CheckResult check_curve_block(const Site& site, const YAML::Node& block, std::string_view label,
                              std::string_view keys) {
  if (!block.IsMap()) return site.fail(block, std::format("{} must be a map", label));
  if (auto checked = check_known_keys(site, block, keys, std::format("{} key", label)); !checked) {
    return checked;
  }
  const auto required = words_of(keys);
  // C++20 ranges algorithm: std::ranges::all_of takes the range itself, not a begin/end
  // iterator pair.
  const bool complete =
      std::ranges::all_of(required, [&](const std::string& key) { return block[key].IsDefined(); });
  if (complete) return {};
  return site.fail(block, std::format("{} needs {}", label, spoken_list(keys)));
}

// `block[key]` as a number that passes `in_range`. Otherwise the message says what is
// wrong: "<label>.<key> must not be empty" for a bare key, "must be a number" for text or
// a quoted number (a typed language rejects "0.5" where a double is expected), and
// "must be <requirement>" for a number out of range (NaN and infinity included).
std::expected<double, ConfigError> read_curve_number(const Site& site, const YAML::Node& block,
                                                     std::string_view label, std::string_view key,
                                                     bool (*in_range)(double),
                                                     std::string_view requirement) {
  const auto full = std::format("{}.{}", label, key);
  if (auto checked = check_not_null(site, block, key, full); !checked) {
    return std::unexpected(checked.error());
  }
  const auto value = block[std::string{key}];
  if (!is_written_as_number(value))
    return site.fail(value, std::format("{} must be a number", full));
  const auto number = number_value(value);
  if (number && in_range(*number)) return *number;
  return site.fail(value, std::format("{} must be {}", full, requirement));
}

// `block[key]` as a duration above zero: a period, a spike's length, a ramp's length, a
// correlation time. Zero would divide by zero in the term's formula or make it vanish.
std::expected<SimTime, ConfigError> read_positive_duration(const Site& site,
                                                           const YAML::Node& block,
                                                           std::string_view label,
                                                           std::string_view key) {
  const auto full = std::format("{}.{}", label, key);
  return parse_key(site, block, key, full, parse_duration)
      .and_then([&](SimTime t) -> std::expected<SimTime, ConfigError> {
        if (t.count() > 0) return t;
        return site.fail(block[std::string{key}], std::format("{} must be positive", full));
      });
}

// `block[key]` as a point in simulated time: a clock time ("20:00") or a duration from the start
// ("5m"), both offsets from t = 0, which is midnight (spec §17 entry 1). An unquoted 20:00 is fine:
// yaml-cpp keeps every scalar's text, and YAML 1.2 has no base-60 numbers (YAML 1.1 read 20:00 as
// the integer 1200).
std::expected<SimTime, ConfigError> read_time_point(const Site& site, const YAML::Node& block,
                                                    std::string_view label, std::string_view key) {
  return parse_key(site, block, key, std::format("{}.{}", label, key), parse_time_point);
}

std::expected<workload::WaveShape, ConfigError> read_wave_shape(const Site& site,
                                                                const YAML::Node& wave) {
  if (const auto shape = workload::parse_wave_shape(scalar_text(wave["shape"]))) return *shape;
  return site.fail(shape_site(wave, "shape"), "rate.wave.shape must be sine, square or diurnal");
}

// The amplitude stays in [0, 1] so the wave multiplier 1 + A x (shape - 1) never drops
// below zero (see workload::Wave).
std::expected<workload::Wave, ConfigError> read_wave(const Site& site, const YAML::Node& wave) {
  constexpr std::string_view label = "rate.wave";
  if (auto checked = check_curve_block(site, wave, label, wave_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Wave read;
  if (auto shape = read_wave_shape(site, wave).transform(store_in(read.shape)); !shape) {
    return std::unexpected(shape.error());
  }
  if (auto period =
          read_positive_duration(site, wave, label, "period").transform(store_in(read.period));
      !period) {
    return std::unexpected(period.error());
  }
  if (auto amplitude =
          read_curve_number(site, wave, label, "amplitude", is_ratio, "between 0 and 1")
              .transform(store_in(read.amplitude));
      !amplitude) {
    return std::unexpected(amplitude.error());
  }
  return read_time_point(site, wave, label, "peak_at").transform([&](SimTime peak_at) {
    read.peak_at = peak_at;
    return read;
  });
}

std::expected<workload::Ramp, ConfigError> read_ramp(const Site& site, const YAML::Node& ramp) {
  constexpr std::string_view label = "rate.ramp";
  if (auto checked = check_curve_block(site, ramp, label, ramp_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Ramp read;
  if (auto from =
          read_bounded_rate(site, ramp, "from", "rate.ramp.from").transform(store_in(read.from));
      !from) {
    return std::unexpected(from.error());
  }
  if (auto to = read_bounded_rate(site, ramp, "to", "rate.ramp.to").transform(store_in(read.to));
      !to) {
    return std::unexpected(to.error());
  }
  return read_positive_duration(site, ramp, label, "over").transform([&](SimTime over) {
    read.over = over;
    return read;
  });
}

// A spike's factor is above zero (a zero factor silences the client; leave the spike out)
// and at most max_step_factor, the bound of a step's fan-out factor, for the same reason:
// rate x factor must stay far from overflowing a double. Overlapping spikes multiply, so
// read_spikes bounds their product too (check_spike_product).
std::expected<workload::Spike, ConfigError> read_spike(const Site& site, const YAML::Node& spike,
                                                       std::string_view label) {
  if (auto checked = check_curve_block(site, spike, label, spike_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Spike read;
  if (auto at = read_time_point(site, spike, label, "at").transform(store_in(read.at)); !at) {
    return std::unexpected(at.error());
  }
  if (auto length =
          read_positive_duration(site, spike, label, "for").transform(store_in(read.duration));
      !length) {
    return std::unexpected(length.error());
  }
  if (auto factor = read_curve_number(site, spike, label, "factor", is_positive, "positive")
                        .transform(store_in(read.factor));
      !factor) {
    return std::unexpected(factor.error());
  }
  if (read.factor <= max_step_factor) return read;
  return site.fail(spike["factor"], std::format("{}.factor must be at most 1e6", label));
}

// Is `spike` active at `t`, i.e. at <= t < at + duration? The same rule as the curve's
// (workload::RateCurve::rate_at), written as a difference so it cannot overflow: both
// times are offsets from t = 0, never negative.
bool spike_active_at(const workload::Spike& spike, SimTime t) {
  return spike.at.count() <= t.count() && t.count() - spike.at.count() < spike.duration.count();
}

// The product of the factors of every spike active at `t`: overlapping spikes compound.
double spike_product_at(const std::vector<workload::Spike>& spikes, SimTime t) {
  double product = 1.0;
  for (const auto& spike : spikes) {
    if (spike_active_at(spike, t)) product *= spike.factor;
  }
  return product;
}

// Overlapping spikes compound, so bounding each factor by max_step_factor is not enough:
// a thousand spikes of 1e6 at the same moment multiply to infinity. The bound applies to
// the product at every moment. The product is a step function that rises only where a
// spike starts (it falls where one ends), so its maximum is reached at some spike's `at`,
// and checking every start finds it: n starts x n spikes, O(n^2), fine for the handful of
// spikes a scenario has.
CheckResult check_spike_product(const Site& site, const YAML::Node& list,
                                const std::vector<workload::Spike>& spikes) {
  const bool bounded = std::ranges::all_of(spikes, [&](const workload::Spike& spike) {
    return spike_product_at(spikes, spike.at) <= max_step_factor;
  });
  if (bounded) return {};
  return site.fail(list, "rate.spikes: overlapping factors multiply to more than 1e6");
}

// Spikes are numbered from 1 in messages, as an author counts them ("rate.spikes[2]").
std::expected<std::vector<workload::Spike>, ConfigError> read_spikes(const Site& site,
                                                                     const YAML::Node& rate) {
  std::vector<workload::Spike> spikes;
  const auto list = rate["spikes"];
  if (!list) return spikes;
  if (auto checked = check_not_null(site, rate, "spikes", "rate.spikes", empty_list); !checked) {
    return std::unexpected(checked.error());
  }
  if (!list.IsSequence()) return site.fail(list, "rate.spikes must be a list");
  for (std::size_t i = 0; i < list.size(); ++i) {
    auto spike = read_spike(site, list[i], std::format("rate.spikes[{}]", i + 1));
    if (!spike) return std::unexpected(spike.error());
    spikes.push_back(*spike);
  }
  return check_spike_product(site, list, spikes).transform([&] { return std::move(spikes); });
}

// One `[time, rate]` entry of a trace; `label` ("rate.points[2]") prefixes a unit error.
std::expected<std::pair<SimTime, Rate>, ConfigError> read_point(const Site& site,
                                                                const YAML::Node& entry,
                                                                std::string_view label) {
  if (!entry.IsSequence() || entry.size() != 2) {
    return site.fail(entry, "rate.points entries are [time, rate] pairs");
  }
  auto time = parse_scalar(site, entry[0], label, parse_time_point);
  if (!time) return std::unexpected(time.error());
  return parse_scalar(site, entry[1], label, parse_rate)
      .and_then([&](Rate rate) { return check_rate_bounded(site, entry[1], label, rate); })
      .transform([&](Rate rate) { return std::pair{*time, rate}; });
}

// Sorted by time, not necessarily strictly: two entries at the same time are a step change
// (the interpolation takes the later one from that instant on).
CheckResult check_points_sorted(const Site& site, const YAML::Node& list,
                                const workload::Points& points) {
  // C++20 std::ranges::is_sorted with a projection: compares the entries by their time
  // (the nanosecond count) without a hand-written comparator over pairs.
  const auto time_ns = [](const std::pair<SimTime, Rate>& point) { return point.first.count(); };
  if (std::ranges::is_sorted(points.at, {}, time_ns)) return {};
  return site.fail(list, "rate.points must be sorted by time");
}

// A recorded or drawn trace that replaces `base`. Its rates are non-negative because
// parse_rate accepts no sign.
std::expected<workload::Points, ConfigError> read_points(const Site& site, const YAML::Node& rate) {
  if (auto checked = check_not_null(site, rate, "points", "rate.points"); !checked) {
    return std::unexpected(checked.error());
  }
  const auto list = rate["points"];
  if (!list.IsSequence()) return site.fail(list, "rate.points must be a list");
  if (list.size() == 0) return site.fail(list, "rate.points must have at least one entry");
  workload::Points points;
  for (std::size_t i = 0; i < list.size(); ++i) {
    auto point = read_point(site, list[i], std::format("rate.points[{}]", i + 1));
    if (!point) return std::unexpected(point.error());
    points.at.push_back(*point);
  }
  return check_points_sorted(site, list, points).transform([&] { return std::move(points); });
}

// The level the terms multiply: exactly one of `base` and `points`.
CheckResult read_level(const Site& site, const YAML::Node& rate, workload::RateCurve& curve) {
  const bool has_base = rate["base"].IsDefined();
  const bool has_points = rate["points"].IsDefined();
  if (has_base && has_points) return site.fail(rate, "rate.base and rate.points are alternatives");
  if (has_points) return read_points(site, rate).transform(store_in(curve.points));
  if (!has_base) return site.fail(rate, "client needs rate.base or rate.points");
  return read_bounded_rate(site, rate, "base", "rate.base").transform(store_in(curve.base));
}

CheckResult read_wave_term(const Site& site, const YAML::Node& rate, workload::RateCurve& curve) {
  if (!rate["wave"]) return {};
  if (auto checked = check_not_null(site, rate, "wave", "rate.wave"); !checked) return checked;
  return read_wave(site, rate["wave"]).transform(store_in(curve.wave));
}

// A ramp's rate is the client's level while it is in force (workload::RateCurve: the spec's
// multiplier ramp/base times base, applied without the division). It needs no base for the
// arithmetic, but the rule "a ramp needs a positive rate.base" is kept on purpose: the file
// then always states the client's nominal rate (explain prints it; deleting the ramp leaves
// a complete curve), and next to `points`, which are a level too, a ramp has no meaning.
CheckResult read_ramp_term(const Site& site, const YAML::Node& rate, workload::RateCurve& curve) {
  if (!rate["ramp"]) return {};
  if (auto checked = check_not_null(site, rate, "ramp", "rate.ramp"); !checked) return checked;
  // One message for both. workload's rate_at throws for a ramp next to points (a
  // programmer error), so no scenario may reach it.
  if (curve.points || curve.base.per_second <= 0.0) {
    return site.fail(rate["ramp"], "rate.ramp needs rate.base");
  }
  return read_ramp(site, rate["ramp"]).transform(store_in(curve.ramp));
}

// The deterministic curve: the level, then the multiplicative terms.
std::expected<workload::RateCurve, ConfigError> read_rate_curve(const Site& site,
                                                                const YAML::Node& rate) {
  workload::RateCurve curve;
  if (auto read = read_level(site, rate, curve); !read) return std::unexpected(read.error());
  if (auto read = read_wave_term(site, rate, curve); !read) return std::unexpected(read.error());
  if (auto read = read_ramp_term(site, rate, curve); !read) return std::unexpected(read.error());
  return read_spikes(site, rate).transform([&](std::vector<workload::Spike> spikes) {
    curve.spikes = std::move(spikes);
    return std::move(curve);
  });
}

// The largest noise `sigma` a scenario may ask for. Chosen, not from the book: the noise
// multiplier is max(0, 1 + x) with x ~ N(0, sigma^2), so at sigma = 10 the client is already
// silent almost half the time (P(x < -1) = 46 %) and bursts to around 10x its curve, at
// times 20x, the rest: no longer "noise around a curve", and no real workload needs more. The cap
// also keeps a noisy rate inside the engine's arithmetic (max_rate_per_second x a multiplier of a
// few tens is still finite) and turns a typo such as `sigma: 15` for 15 % into an error instead of
// a strange run.
inline constexpr double max_noise_sigma = 10.0;

// `sigma` as read: non-negative (0 means no noise) and at most max_noise_sigma.
CheckResult check_sigma_capped(const Site& site, const YAML::Node& noise, std::string_view label,
                               double sigma) {
  if (sigma <= max_noise_sigma) return {};
  return site.fail(shape_site(noise, "sigma"),
                   std::format("{}.sigma must be at most {}", label, max_noise_sigma));
}

// The Ornstein-Uhlenbeck noise of spec §9.2: `sigma` is its stationary standard deviation
// (0 is allowed: no noise), `correlation` (tau) roughly how long a burst lasts.
std::expected<workload::Noise, ConfigError> read_noise(const Site& site, const YAML::Node& noise) {
  constexpr std::string_view label = "rate.noise";
  if (auto checked = check_curve_block(site, noise, label, noise_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Noise read;
  if (auto sigma = read_curve_number(site, noise, label, "sigma", is_non_negative, "non-negative")
                       .transform(store_in(read.sigma));
      !sigma) {
    return std::unexpected(sigma.error());
  }
  if (auto capped = check_sigma_capped(site, noise, label, read.sigma); !capped) {
    return std::unexpected(capped.error());
  }
  return read_positive_duration(site, noise, label, "correlation").transform([&](SimTime tau) {
    read.correlation = tau;
    return read;
  });
}

CheckResult read_noise_term(const Site& site, const YAML::Node& rate,
                            std::optional<workload::Noise>& noise) {
  if (!rate["noise"]) return {};
  if (auto checked = check_not_null(site, rate, "noise", "rate.noise"); !checked) return checked;
  return read_noise(site, rate["noise"]).transform(store_in(noise));
}

// The `rate:` block: a map of known keys, read into the curve and the noise.
CheckResult read_rate(const Site& site, const YAML::Node& body, model::Workload& workload) {
  if (auto checked = check_not_null(site, body, "rate", "rate"); !checked) return checked;
  const auto rate = body["rate"];
  if (!rate) return site.fail(body, "client needs rate.base or rate.points");
  // `rate: 100/s` was the phase-1 spelling of a constant rate; say how to write it now.
  if (rate.IsScalar()) {
    return site.fail(rate,
                     std::format("rate must be a map (write rate: {{base: {}}})", rate.Scalar()));
  }
  if (!rate.IsMap()) return site.fail(rate, "rate must be a map");
  if (auto checked = check_known_keys(site, rate, rate_keys, "rate key"); !checked) return checked;
  if (auto read = read_rate_curve(site, rate).transform(store_in(workload.curve)); !read) {
    return read;
  }
  return read_noise_term(site, rate, workload.noise);
}

// A share is the probability that a request is this op: above 0 (a zero share is an op
// that never happens; leave it out) and at most 1.
std::expected<OpShare, ConfigError> read_op_share(const Site& site, const YamlElement& entry) {
  auto name = read_op_name(site, entry.first, "op name");
  if (!name) return std::unexpected(name.error());
  const auto label = std::format("op share '{}'", *name);
  if (auto checked = check_entry_not_null(site, entry, label); !checked) {
    return std::unexpected(checked.error());
  }
  const auto share = number_value(entry.second);
  if (share && is_unit_fraction(*share)) return OpShare{std::move(*name), *share};
  return site.fail(entry.second, std::format("op share '{}' must be in (0, 1]", *name));
}

// `value` with 15 significant digits: every share sum outside the 1e-6 tolerance differs
// from 1 within the first 7, while the binary rounding noise of the addition (0.5 +
// 0.500002 is 1.0000019999999998) sits in the 16th and 17th and is not shown.
// std::to_chars, not std::format("{:.15g}"): with GCC 14.2, a precision in a format spec
// throws std::format_error at run time in a module unit that imports a module whose global
// module fragment includes <chrono>, as simsys.core's does: <chrono> pulls in <format>
// (verified 2026-10-28 with a three-file probe; the same GCC 14.2 module defect behind the
// lean-interface rule of CLAUDE.md section 3). Plain "{}" is unaffected.
std::string with_significant_digits(double value) {
  return to_chars_text(value, std::chars_format::general, 15);
}

// Every request is exactly one op, so the shares are a probability distribution.
CheckResult check_shares_sum_to_one(const Site& site, const YAML::Node& ops,
                                    const OpShares& shares) {
  // C++23 std::ranges::fold_left: a left fold over a range, replacing std::accumulate's
  // iterator pair (and its trap of an int initial value truncating doubles: here 0.0).
  // C++20 std::views::values: a view of each pair's second member, without a copy.
  const double sum = std::ranges::fold_left(shares | std::views::values, 0.0, std::plus<>{});
  if (std::abs(sum - 1.0) <= share_sum_tolerance) return {};
  return site.fail(ops,
                   std::format("op shares sum to {}, expected 1", with_significant_digits(sum)));
}

// Sorted by op name, so the same mix listed in another order loads as the same Workload.
std::expected<OpShares, ConfigError> read_op_shares(const Site& site, const YAML::Node& body) {
  const auto ops = body["ops"];
  if (!ops) return site.fail(body, "client needs ops");
  // No `{}` hint: a client needs at least one op.
  if (auto checked = check_not_null(site, body, "ops", "ops"); !checked) {
    return std::unexpected(checked.error());
  }
  if (!ops.IsMap()) return site.fail(ops, "ops must be a map");
  if (ops.size() == 0) return site.fail(ops, "client needs at least one op");
  auto shares =
      collect_each(ops, [&](const YamlElement& entry) { return read_op_share(site, entry); });
  if (!shares) return shares;
  std::ranges::sort(*shares);  // pairs compare by their first member (the op) first
  return check_shares_sum_to_one(site, ops, *shares).transform([&] { return std::move(*shares); });
}

}  // namespace

// A count as the scenarios write cache sizes and key spaces: a positive integer with an optional SI
// suffix k, M or G (x 1e3, 1e6, 1e9; the same letters a rate uses, without "/s"): 500, 10k, 20M,
// 10G. Nothing for anything else (a fraction, a sign, an overflow of 64 bits). A local
// helper rather than parse_rate(text + "/s"): a rate may be fractional ("1.5k/s"), a count
// of entries may not.
std::optional<std::int64_t> parse_count(std::string_view text) {
  constexpr std::array suffixes{std::pair{'k', std::int64_t{1'000}},
                                std::pair{'M', std::int64_t{1'000'000}},
                                std::pair{'G', std::int64_t{1'000'000'000}}};
  std::int64_t scale = 1;
  for (const auto& [letter, size] : suffixes) {
    if (!text.empty() && text.back() == letter) {
      scale = size;
      text.remove_suffix(1);
      break;
    }
  }
  const auto count = parse_whole<std::int64_t>(text);
  if (!count || *count <= 0 || *count > std::numeric_limits<std::int64_t>::max() / scale) {
    return std::nullopt;
  }
  return *count * scale;
}

// ---- Client keys (spec §18 entry 1) ----

namespace {

constexpr std::string_view client_keys_keys = "zipf space uniform";

// `keys.space: 100M`, `keys.uniform: 10M`: a key-space size, a count like a cache's `keys`.
std::expected<double, ConfigError> read_key_space(const Site& site, const YAML::Node& value,
                                                  std::string_view key) {
  const auto count = parse_count(number_text(value));
  if (!count) {
    return site.fail(value, std::format("keys.{} must be a positive count (a whole number, "
                                        "optionally with k, M or G: 500, 10k, 100M)",
                                        key));
  }
  return static_cast<double>(*count);
}

// `keys: {zipf: 1.0, space: 100M}`: Zipf with exponent s >= 0 over N keys (s = 0 is uniform,
// and stored as such).
std::expected<model::KeySpec, ConfigError> read_zipf_keys(const Site& site,
                                                          const YAML::Node& keys) {
  if (!keys["zipf"] || !keys["space"]) {
    return site.fail(keys, "keys needs zipf and space, or uniform");
  }
  // number_value already refuses .inf/.nan and words (parse_yaml_double): one message for all.
  const auto exponent = number_value(keys["zipf"]);
  if (!exponent || *exponent < 0.0)
    return site.fail(keys["zipf"], "keys.zipf must be a finite non-negative number");
  // Canonical form (model::canonical): `zipf: 0` is stored as uniform.
  return read_key_space(site, keys["space"], "space").transform([&](double space) {
    // C++20 designated initializers: each member named where it is set, so two members of the
    // same type cannot be swapped unnoticed as with positional aggregate initialization.
    return model::canonical(model::KeySpec{.zipf = *exponent, .space = space});
  });
}

// A client's `keys`: which keys it asks for, and how popular each is (model::KeySpec). Read
// only for the `che` caches downstream (graph's agreed_keys), but checked on every client: a
// misspelt block would otherwise surface only when a che cache is added.
CheckResult read_client_keys(const Site& site, const YAML::Node& body, model::Workload& workload) {
  const auto keys = body["keys"];
  if (!keys) return {};
  if (!keys.IsMap()) return site.fail(keys, "keys must be a map");
  if (auto checked = check_known_keys(site, keys, client_keys_keys, "keys entry"); !checked) {
    return checked;
  }
  const bool zipf_form = keys["zipf"] || keys["space"];
  if (zipf_form && keys["uniform"]) {
    return site.fail(keys, "keys takes zipf+space or uniform, not both");
  }
  if (!keys["uniform"]) return read_zipf_keys(site, keys).transform(store_in(workload.keys));
  return read_key_space(site, keys["uniform"], "uniform").transform([&](double space) {
    workload.keys = model::KeySpec{.zipf = std::nullopt, .space = space};
  });
}

}  // namespace

// True when the client sends `op`, i.e. it has a share in the client's op mix.
bool sends_op(const model::Workload& workload, std::string_view op) {
  // C++20 std::views::keys: a view of each pair's first member (the op name). C++23
  // std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::ranges::find(r, v) != r.end()`, which spells the question as an iterator comparison.
  return std::ranges::contains(workload.op_shares | std::views::keys, op);
}

namespace {

// One entry of `sizes: {stream: 50MB}`: an op the client sends, and its size.
std::expected<std::pair<std::string, Bytes>, ConfigError> read_op_size(
    const Site& site, const YamlElement& entry, const model::Workload& workload) {
  auto op = read_op_name(site, entry.first, "op name");
  if (!op) return std::unexpected(op.error());
  if (!sends_op(workload, *op)) {
    return site.fail(entry.first,
                     std::format("sizes names op '{}', which the client does not send", *op));
  }
  const auto label = std::format("sizes '{}'", *op);
  return check_entry_not_null(site, entry, label).and_then([&] {
    return read_size(site, entry.second, label).transform([&](Bytes size) {
      return std::pair{std::move(*op), size};
    });
  });
}

// `sizes: {stream: 50MB, upload: 300MB}` (model::Workload::sizes), optional; read after the
// op shares, since every op it sizes must be one the client sends.
CheckResult read_client_sizes(const Site& site, const YAML::Node& body, model::Workload& workload) {
  const auto sizes = body["sizes"];
  if (!sizes) return {};
  if (auto checked = check_not_null(site, body, "sizes", "sizes", empty_map); !checked) {
    return checked;
  }
  if (!sizes.IsMap()) return site.fail(sizes, "sizes must be a map");
  // C++23 std::ranges::to: collects a range into a container in one call, replacing a loop
  // of inserts (or the iterator-pair constructor) into a map declared beforehand.
  return collect_each(sizes,
                      [&](const YamlElement& entry) { return read_op_size(site, entry, workload); })
      .transform([&](const auto& entries) {
        workload.sizes = std::ranges::to<std::map<std::string, Bytes>>(entries);
      });
}

// The most retries a client may state. Chosen, not from the book: ten extra attempts already let a
// failing path multiply the client's load by 11 (1 + 10 attempts per request), the engine keeps
// one bucket per retry, and a larger number is a typo rather than a client anyone ships.
inline constexpr int max_client_retries = 10;

// `retries: 2` (model::Workload::retries): a whole number from 0 to max_client_retries. A count
// beyond an int is too many as well; a negative number, a fraction, a word or a quoted "2" is no
// count (number_text: a quoted scalar is a string).
std::expected<int, ConfigError> read_retry_count(const Site& site, const YAML::Node& value) {
  const auto text = number_text(value);
  const auto count = parse_whole<int>(text);
  const bool beyond_an_int = !count && count.error() == NumberFault::out_of_range && text[0] != '-';
  if (beyond_an_int || (count && *count > max_client_retries)) {
    return site.fail(value, std::format("retries must be at most {}", max_client_retries));
  }
  if (!count || *count < 0) return site.fail(value, "retries must be a non-negative integer");
  return *count;
}

// `retry_after: 250ms` (model::Workload::retry_after): a positive wait, and only beside `retries`:
// a backoff with nothing to retry is a mistake the author should hear about, as a `worker_pool`
// without `workers` is.
CheckResult read_retry_after(const Site& site, const YAML::Node& body, model::Workload& workload) {
  if (auto read = read_optional_duration(site, body, "retry_after", workload.retry_after); !read) {
    return read;
  }
  if (!workload.retry_after) return {};
  if (workload.retry_after->count() <= 0) {
    return site.fail(body["retry_after"], "retry_after must be positive");
  }
  if (!body["retries"]) return site.fail(body["retry_after"], "retry_after needs retries");
  return {};
}

// `retries` and `retry_after` (phase 3 spec §6.1), both optional. A client that states `retries`
// and no `retry_after` waits defaults::retry_after, filled in here so that explain prints the wait
// the engine uses.
CheckResult read_client_retries(const Site& site, const YAML::Node& body,
                                model::Workload& workload) {
  if (auto read = read_retry_after(site, body, workload); !read) return read;
  if (!body["retries"]) return {};
  if (auto checked = check_not_null(site, body, "retries", "retries"); !checked) return checked;
  return read_retry_count(site, body["retries"]).transform([&](int count) {
    workload.retries = count;
    if (!workload.retry_after) workload.retry_after = defaults::retry_after;
  });
}

}  // namespace

std::expected<model::Workload, ConfigError> read_workload(const Site& site,
                                                          const YAML::Node& body) {
  model::Workload workload;
  if (auto read = read_rate(site, body, workload); !read) return std::unexpected(read.error());
  if (auto read = read_op_shares(site, body).transform(store_in(workload.op_shares)); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_client_keys(site, body, workload); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_client_sizes(site, body, workload); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_optional_duration(site, body, "timeout", workload.timeout); !read) {
    return std::unexpected(read.error());
  }
  // A client that gives up at once answers nothing, and a latency SLO would observe the 0 s the
  // client waited: a timeout is a positive wait.
  if (workload.timeout && workload.timeout->count() <= 0) {
    return site.fail(body["timeout"], "timeout must be positive");
  }
  if (auto read = read_client_retries(site, body, workload); !read) {
    return std::unexpected(read.error());
  }
  return workload;
}

}  // namespace simsys::config::detail
