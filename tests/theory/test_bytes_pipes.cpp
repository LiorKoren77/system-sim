// Whole-simulation checks against closed-form queueing theory (spec §12).
// This file: byte-sized nodes, the room that bounds the wait, which byte-sized nodes the engine
// runs, a blob store's event notifications, and a link's bandwidth as a pipe. One of the nine files
// the theory tests are split into by topic; the scenario builders more than one of them needs live
// in tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;
import simsys.test_fixtures;
import simsys.workload;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::Direction;
using simsys::model::HitRatio;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::first_step;
using simsys::test_support::mm1;
using simsys::test_support::one_gbit;
using simsys::test_support::only_link;
using simsys::test_support::pipe_mb_per_s;
using simsys::test_support::pipe_over;
using simsys::test_support::records_of;

// ---- Byte-sized nodes count megabytes (spec §19 entry 2) ----
//
// Spec section 6: a blob store does 500 MB/s and a CDN's per-MB transfer takes 10 ms; the book's
// latency table gives a megabyte 30 ms from disk and 10 ms over the network. A node whose
// capacity is a byte rate therefore counts MEGABYTES: capacity = bytes/s / 1e6 units/s, a request
// of op o costs request_bytes(o) / 1e6 units, and its service time s_MB is the time of ONE
// megabyte, so the implied concurrency is Little's law again, c = units/s x s_MB.
//
// The queue waits for whole OBJECTS: each request is a batch of megabytes that holds one of the c
// slots for its whole transfer, size x s_MB, so Erlang C is fed the rate-weighted mean object
// service time
//   s_obj = sum_o (rate_o x MB_o x s_MB) / sum_o rate_o
// with the same c. rho is unchanged (Little's law: objects/s x s_obj / c = MB/s / capacity), only
// the wait is a whole object's. A request is then answered:
//   a write (an op in the store's write_ops)  at its LAST byte:  Wq + MB_o x s_MB
//   a read or a stream                         at its FIRST MB:   Wq + s_MB
// and a request under 1 MB still takes one megabyte's time either way (the small-object bound).
//
// A transfer of a fixed size at a fixed rate takes a fixed time, so each object's own service is
// deterministic (SCV 0) and the spread of the service times is the size mix's alone. The queue's
// Allen-Cunneen factor is (c_a^2 + c_s^2) / 2 with c_s^2 the mixture's SCV:
//   E[S]   = sum_o p_o s_o,   E[S^2] = sum_o p_o (1 + cs_o^2) s_o^2   (p_o = rate_o / sum rate)
//   c_s^2  = E[S^2] / E[S]^2 - 1
// so a single size with Poisson arrivals waits (1 + 0) / 2 = HALF the M/M/c wait (M/D/c), and the
// answer adds no variance of its own: the response's variance is the wait's. And the stochastic
// wait is bounded by the room: never longer than queue_limit / capacity, the room-drain bound.

namespace {
// u (`rate`/s of `op`, each `size` long) -> store, a zero-latency link, so the client's response
// is the store's sojourn alone. Node indices: u 0, store 1. The store is a blob store with its
// type's defaults (spec section 6: 500 MB/s, 40 ms per MB) unless one is passed.
// C++20 designated initializers: `.id = ...` names each member being set, in
// declaration order, and leaves the rest at their defaults. Replaces positional braces
// (which break silently when members are reordered) or a default object assigned field
// by field; here a node reads like the YAML it stands for.
Scenario blob_store_chain(const std::string& op, double rate, simsys::Bytes size,
                          NodeSpec store = NodeSpec{.id = "store", .type = NodeType::blob_store}) {
  Scenario s{.name = "blob-chain", .version = 1, .duration = 10s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{op, 1.0}},
                                           .sizes = {{op, size}}}},
             std::move(store)};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s}};
  return s;
}

// A default blob store that counts `op` as a write (its `write_ops`), as youtube's original_store
// counts `upload`.
NodeSpec store_writing(const std::string& op) {
  return NodeSpec{
      .id = "store", .type = NodeType::blob_store, .write_ops = std::vector<std::string>{op}};
}

// first_step(g), the first step of a fresh simulation, is shared with the queue and sink tests:
// tests/support/fixtures.cppm.
}  // namespace

// 5 uploads/s of 50 MB into a default blob store that counts `upload` as a write:
//   demand   = 5/s x 50 MB = 250 units/s of 1 MB, against 500 units/s: rho = 250 / 500 = 0.5;
//   c        = 500 units/s x 40 ms per MB = 20 (Little's law);
//   s_obj    = 50 MB x 40 ms = 2 s, the time one upload holds a slot, the same for every upload
//              (a deterministic transfer: c_s^2 = 0);
//   Erlang C(20, 0.5) = 0.0037311 (queueing::erlang_c), so the M/M/c wait would be
//   C x s_obj / (c (1 - rho)) = 0.0037311 x 2 s / (20 x 0.5) = 0.746 ms, and M/D/c's is
//   Wq       = (1 + 0) / 2 x 0.746 ms = 0.373 ms                (Allen-Cunneen, Poisson arrivals)
//   response = Wq + 50 MB x 40 ms = 2.000373 s: a write is answered at its last byte.
// Before the first review the wait was computed over 1 MB units (0.0149 ms) and the response was
// the first megabyte's 40 ms: 50 times too short a wait and an upload acknowledged before its data.
TEST_CASE("a default blob store fed 5 uploads of 50 MB per second runs at rho 0.5 with c 20",
          "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 5, simsys::Bytes{50e6}, store_writing("upload")));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(250.0, 1e-12));  // units (MB) per second
  REQUIRE_THAT(store.rho, WithinRel(0.5, 1e-12));
  REQUIRE(store.dropped_per_s == 0.0);
  const auto wait = simsys::queueing::mmc_wait(20, 0.5, 2.0).mean_s / 2.0;  // M/D/c
  REQUIRE_THAT(wait, WithinAbs(3.7311e-4, 1e-8));  // the napkin number: 0.373 ms
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 2.0, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(2.000373, 1e-6));
  // The node's own latency is its requests' sojourn: here every request is an upload.
  REQUIRE_THAT(store.latency.mean_s, WithinRel(wait + 2.0, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(5.0, 1e-9));  // requests, not MB
}

// The same queue read instead of written: 5 reads/s of 50 MB wait the same whole-object 0.373 ms
// (the queue does not care which way the bytes go), but a read is answered when its first
// megabyte arrives, Wq + 40 ms = 40.373 ms: the client reads the rest as it streams in.
TEST_CASE("a 50 MB read waits behind whole objects and is answered at its first megabyte",
          "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 5, simsys::Bytes{50e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.5, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.5, 2.0).mean_s / 2.0;  // M/D/c
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 0.040, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040373, 1e-6));
}

// The review's example: 9 reads/s of 50 MB at a default store are 450 MB/s, rho 0.9. Over 1 MB
// units the M/M/c wait would be C(20, 0.9) x 40 ms / (20 x 0.1) = 0.5508 x 0.02 s = 11 ms; whole
// 50 MB objects (s_obj = 2 s) wait C(20, 0.9) x 2 s / (20 x 0.1) = 0.5508 s in M/M/c, fifty times
// longer, and as transfers of one fixed time (M/D/c) half of that, 0.2754 s. The read is answered
// 40 ms after that wait: 0.3154 s.
TEST_CASE("at rho 0.9 a 50 MB object waits 0.28 s, half the M/M/c wait and not 11 ms", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 9, simsys::Bytes{50e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.9, 1e-12));
  const auto per_unit = simsys::queueing::mmc_wait(20, 0.9, 0.040).mean_s;
  const auto per_object = simsys::queueing::mmc_wait(20, 0.9, 2.0).mean_s;
  REQUIRE_THAT(per_unit, WithinAbs(0.011015, 1e-6));
  REQUIRE_THAT(per_object, WithinAbs(0.550769, 1e-6));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(per_object / 2.0 + 0.040, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.315385, 1e-6));
}

// Book ch. 14's 300 MB upload, counted as a write: 1 upload/s at a default store is 300 MB/s,
// rho 0.6; s_obj = 300 MB x 40 ms = 12 s, so the M/M/c wait would be C(20, 0.6) x 12 s / (20 x 0.4)
// = 0.024134 x 1.5 s = 36.2 ms and M/D/c's is half, Wq = 18.1 ms; the upload is acknowledged at its
// last byte, 12 s + 18.1 ms = 12.0181 s.
TEST_CASE("a 300 MB upload is answered at its last byte, 12 s after its wait", "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 1, simsys::Bytes{300e6}, store_writing("upload")));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.6, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.6, 12.0).mean_s / 2.0;  // M/D/c
  REQUIRE_THAT(wait, WithinAbs(0.018101, 1e-6));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(12.0 + wait, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(12.0181, 1e-4));
}

// The same upload's spread: the 12 s transfer is deterministic, so the response's variance is the
// wait's alone, (1/2)^2 x Var(Wq of M/M/c) (the Allen-Cunneen factor applied to the variance, the
// engine's heuristic) = 0.25 x P (2 - P) theta^2 with P = 0.024134 and theta = 12 s / (20 x 0.4) =
// 1.5 s: 0.026824 s^2, a standard deviation of 0.16 s. The lognormal fit then puts the p99 at 12.40
// s and the p1 at 11.64 s (below the 12 s no upload can beat: an artefact of the fit, whose left
// tail knows no floor): every upload takes more than 10 s, so youtube's `timeout: 10s` would time
// out all of them, not the ~42 % an exponential transfer (CV 1: variance 144 s^2 more, a p99 of
// 58.9 s) suggested. The shipped scenario sizes that timeout per the book (resumable uploads).
TEST_CASE("a 300 MB upload's p99 is 12.4 s and its p1 is above 10 s: the transfer is deterministic",
          "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 1, simsys::Bytes{300e6}, store_writing("upload")));
  const auto response = first_step(g).responses[0].response;
  const auto mmc = simsys::queueing::mmc_wait(20, 0.6, 12.0);
  REQUIRE_THAT(response.variance_s2, WithinRel(mmc.variance_s2 / 4.0, 1e-9));
  REQUIRE_THAT(response.variance_s2, WithinAbs(0.026824, 1e-6));
  using simsys::queueing::lognormal_quantile;
  using simsys::queueing::z_p99;
  REQUIRE_THAT(lognormal_quantile(response.mean_s, response.variance_s2, z_p99),
               WithinAbs(12.404, 1e-3));
  REQUIRE(lognormal_quantile(response.mean_s, response.variance_s2, -z_p99) > 10.0);
}

// A 2 KB read costs 2e3 B / 1e6 B per unit = 0.002 units: 1000 reads/s are 2 units/s, rho =
// 2 / 500 = 0.004; s_obj = 0.002 x 40 ms = 80 us and nothing waits (Erlang C(20, 0.004) = 4e-41;
// M/D/c halves it),
// and yet each read takes the whole 40 ms of one unit: the documented upper bound for small
// objects (a request under 1 MB still pays one unit's time). Without the bound a 2 KB read would
// be answered in 80 us.
TEST_CASE("a 2 KB read at a blob store costs 0.002 units and is answered in 40 ms", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 1000, simsys::Bytes{2e3}));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(2.0, 1e-12));  // 1000/s x 0.002 units
  REQUIRE_THAT(store.rho, WithinRel(0.004, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.004, 80e-6).mean_s / 2.0;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 0.040, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040, 1e-9));
}

// The same bound for a small write: its last byte comes with its first megabyte's time, so a 2 KB
// write (the default write op, `write`) is answered in 40 ms, not in 2 KB x 40 ms per MB = 80 us.
TEST_CASE("a 2 KB write is answered in one megabyte's time, not in 80 us", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("write", 1000, simsys::Bytes{2e3}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040, 1e-9));
}

// The size is the client's: the same store fed the same 5/s at 100 MB doubles demand to 500
// units/s (rho 1.0), where 50 MB gave 250. Sizes scale the load, never the number of requests.
TEST_CASE("doubling the request size doubles a byte-sized node's demand and keeps the request rate",
          "[theory]") {
  const auto g = *Graph::build(blob_store_chain("upload", 5, simsys::Bytes{100e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].demand_per_s, WithinRel(500.0, 1e-12));
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(1.0, 1e-12));
}

namespace {
// u sends 20 requests/s, half `write`s of 50 MB and half `read`s of 10 MB, into a default blob
// store (500 MB/s, 40 ms per MB, c = 20, a waiting room of 1000 MB). Zero-latency link. Node
// indices: u 0, store 1.
Scenario mixed_size_store() {
  Scenario s{.name = "blob-mixed", .version = 1, .duration = 60s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{20}},
                                           .op_shares = {{"read", 0.5}, {"write", 0.5}},
                                           .sizes = {{"read", simsys::Bytes{10e6}},
                                                     {"write", simsys::Bytes{50e6}}}}},
             NodeSpec{.id = "store", .type = NodeType::blob_store}};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s}};
  return s;
}

// The client's sample of `op` in a step's responses.
const simsys::engine::ResponseSample& response_to(const simsys::engine::StepRecord& r,
                                                  std::string_view op) {
  // C++20 std::ranges::find with a projection: compares `op` with each sample's `op` member
  // (the projection, a pointer to member), replacing std::find_if over begin/end with a lambda
  // that reads the member.
  const auto found = std::ranges::find(r.responses, op, &simsys::engine::ResponseSample::op);
  REQUIRE(found != r.responses.end());
  return *found;
}
}  // namespace

// Two sizes at one overloaded store: 10 writes/s x 50 MB + 10 reads/s x 10 MB = 600 MB/s against
// 500, an offered rho of 1.2. The 1000 MB waiting room fills in 1000 / (600 - 500) = 10 s; from
// then on the store drains 500 units/s and drops 100. The queue is well-mixed in UNITS (see
// engine::Simulation::serve), so each op loses the same share of its megabytes, 100 / 600 = 1/6,
// and so the same share of its requests: 10 x 5/6 = 8.333 of each op are answered per second,
// whatever their sizes. Units balance at every step (demand dt = (drain + dropped) dt + change in
// backlog). Both ops wait in the same queue; the write is answered at its last byte (50 MB x
// 40 ms = 2 s), the read at its first megabyte (40 ms), so the write's response is 1.96 s longer,
// and the node's own latency is their mixture by requests, half and half.
TEST_CASE("an overloaded blob store answers the same share of large and small requests",
          "[theory]") {
  const auto g = *Graph::build(mixed_size_store());
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  const auto store_index = *g.index_of("store");
  simsys::engine::StepRecord last;
  double backlog = 0.0;
  for (int i = 0; i < 300; ++i) {  // 30 s of 100 ms steps
    last = sim->step(i * 100ms, 100ms);
    const auto& store = last.nodes[store_index];
    const double in = store.demand_per_s * 0.1;
    const double out = (store.drain_per_s + store.dropped_per_s) * 0.1 + (store.backlog - backlog);
    REQUIRE_THAT(out, WithinAbs(in, 1e-9 * in));
    backlog = store.backlog;
  }
  const auto& store = last.nodes[store_index];
  REQUIRE_THAT(store.demand_per_s, WithinRel(600.0, 1e-12));
  REQUIRE_THAT(store.drain_per_s, WithinRel(500.0, 1e-9));
  REQUIRE_THAT(store.dropped_per_s, WithinRel(100.0, 1e-6));
  const auto& writes = response_to(last, "write");
  const auto& reads = response_to(last, "read");
  REQUIRE_THAT(writes.answered.per_second, WithinRel(10.0 * 5.0 / 6.0, 1e-6));
  REQUIRE_THAT(reads.answered.per_second, WithinRel(10.0 * 5.0 / 6.0, 1e-6));
  REQUIRE_THAT(writes.response.mean_s - reads.response.mean_s, WithinAbs(1.96, 1e-9));
  REQUIRE_THAT(store.latency.mean_s,
               WithinRel((writes.response.mean_s + reads.response.mean_s) / 2.0, 1e-9));
}

// ---- The room bounds the wait, the size
// mix's spread enters Kingman ----

namespace {
// The last step of `steps` 100 ms steps of a fresh simulation of `g`.
simsys::engine::StepRecord step_until(const Graph& g, int steps) {
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  simsys::engine::StepRecord last;
  for (int i = 0; i < steps; ++i) last = sim->step(i * 100ms, 100ms);
  return last;
}
}  // namespace

// 12 writes/s of 50 MB are 600 MB/s against a default store's 500: past saturation. Erlang C at the
// 0.99 clamp with 2 s objects would add C(20, 0.99) x 2 s / (20 x 0.01) x 1/2 = 4.74 s (9.48 s with
// exponential service, what an earlier version reported: an 11.48 s response). But the store's
// waiting room holds 1000 MB and drains at 500 MB/s, so nobody admitted waits more than 1000 / 500
// = 2 s (the room-drain bound); the full room's fluid wait is exactly that. A write is then
// answered at its last byte, 2 s later: 4 s. The excess, 100 MB/s, is dropped.
TEST_CASE("twelve 50 MB writes per second wait at most the 2 s their room drains in: about 4 s",
          "[theory]") {
  const auto g = *Graph::build(blob_store_chain("write", 12, simsys::Bytes{50e6}));
  const auto last = step_until(g, 300);  // 30 s: the room is full after 10 s
  const auto& store = last.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.backlog, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(store.dropped_per_s, WithinRel(100.0, 1e-6));
  REQUIRE(last.responses[0].response.mean_s - 2.0 <= 2.0 + 1e-9);  // the wait
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinRel(4.0, 1e-9));
}

// What the room-drain bound does NOT bound: the estimated percentiles. The same twelve 50 MB
// writes per second over a run: every admitted write waits at most the room's 2 s and is answered
// 2 s after it, so none takes longer than 4 s. The report's percentiles are a lognormal fitted to
// the mean (4 s) and the variance (the wait's, capped at the room's scale, T^2 / 4 = 1 s^2), and a
// lognormal has no upper bound: its p99 is 6.88 s. The decision this pins: percentiles are
// labelled `(estimated)`, not truncated at a bound, since only a path whose every hop is
// deterministic has a finite slowest request (docs/scenario-schema.md, "How percentiles are
// estimated, and where they mislead").
TEST_CASE(
    "a saturated blob store's percentiles are labelled, not truncated: its estimated p99 is "
    "6.88 s past a 4 s bound",
    "[theory]") {
  Scenario s = blob_store_chain("write", 12, simsys::Bytes{50e6});
  s.duration = 60s;
  const auto g = *Graph::build(s);
  simsys::report::Summary summary{g, s.warmup};
  (*Simulation::create(g)).run({.duration = s.duration, .step = s.step}, [&](const auto& r) {
    summary.observe(r);
  });
  const auto op = summary.ops().front();
  REQUIRE_THAT(op.mean_s, WithinAbs(4.0, 0.01));
  REQUIRE_THAT(op.p99_s, WithinAbs(6.88, 0.005));
  REQUIRE(op.p99_s > 2.0 + 2.0);  // the room's drain time plus the write's own 2 s
}

// The review's stop transient: 24 requests/s, half 50 MB writes and half 10 MB reads, overload a
// default store (720 MB/s against 500) for 20 s and then stop. From then on nothing arrives, and
// the store's latency is what a read arriving now would see: the wait plus one megabyte (40 ms).
// While the full room drains (1000 MB at 500 MB/s, 2 s), the wait is bounded by the room's drain
// time, 2 s, where an earlier version held it at the clamp's 5.7 s; once the room is empty nothing
// waits and the latency is the 40 ms of one megabyte. It never rises on the way down.
TEST_CASE(
    "after an overload stops a blob store's latency falls to one megabyte's time, never "
    "above the room's drain time",
    "[theory]") {
  Scenario s = blob_store_chain("write", 24, simsys::Bytes{50e6});
  auto& workload = *s.nodes[0].workload;
  workload.curve.points =
      simsys::workload::Points{.at = {{0s, Rate{24}}, {20s, Rate{24}}, {20s, Rate{0}}}};
  workload.op_shares = {{"write", 0.5}, {"read", 0.5}};
  workload.sizes = {{"write", simsys::Bytes{50e6}}, {"read", simsys::Bytes{10e6}}};
  const auto g = *Graph::build(s);
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  const auto store = *g.index_of("store");
  for (int i = 0; i < 200; ++i) (void)sim->step(i * 100ms, 100ms);  // 20 s of overload
  double previous = std::numeric_limits<double>::infinity();
  double latency = 0.0;
  for (int i = 200; i < 250; ++i) {  // 5 s after the stop
    latency = sim->step(i * 100ms, 100ms).nodes[store].latency.mean_s;
    CAPTURE(i, latency);
    REQUIRE(latency <= previous + 1e-12);
    REQUIRE(latency <= 2.0 + 0.040 + 1e-9);
    previous = latency;
  }
  REQUIRE_THAT(latency, WithinAbs(0.040, 1e-9));
}

// A ramp down: twelve 50 MB writes a second (600 MB/s) fill a default store's 1000 MB room for 20
// s, then the load falls to four (200 MB/s). The room drains at 500 − 200 = 300 MB/s, 30 MB a step:
// empty after 3.3 seconds. Every server stays busy meanwhile (drain 500 MB/s, rho 1), but the new
// writes arrive at rho 200 / 500 = 0.4, where Erlang C(20, 0.4) adds about 20 us: the stochastic
// wait reads the arrivals' utilization, min(demand, drain) / capacity. So the wait is the backlog's
// own drain time, backlog / 500 MB/s, and a write's latency, that wait plus its 2 s transfer, falls
// with the backlog: 3.94 s at 970 MB, 2.02 s at 10 MB. Read at the busy rho 1 it held at the room's
// 2 s drain time, 4 s, until the room was empty.
TEST_CASE("after a ramp down a blob store's latency falls with its backlog, 3.94 s to 2.02 s",
          "[theory]") {
  Scenario s = blob_store_chain("write", 12, simsys::Bytes{50e6});
  s.nodes[0].workload->curve.points =
      simsys::workload::Points{.at = {{0s, Rate{12}}, {20s, Rate{12}}, {20s, Rate{4}}}};
  const auto g = *Graph::build(s);
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  const auto store = *g.index_of("store");
  for (int i = 0; i < 200; ++i) (void)sim->step(i * 100ms, 100ms);  // 20 s: the room is full
  double previous = std::numeric_limits<double>::infinity();
  for (int i = 200; i < 233; ++i) {  // 3.3 s of draining, 30 MB a step
    const auto sample = sim->step(i * 100ms, 100ms).nodes[store];
    CAPTURE(i, sample.backlog, sample.latency.mean_s);
    REQUIRE_THAT(sample.backlog, WithinAbs(1000.0 - 30.0 * (i - 199), 1e-6));
    REQUIRE(sample.rho == 1.0);  // busy throughout
    REQUIRE_THAT(sample.latency.mean_s, WithinAbs(2.0 + sample.backlog / 500.0, 1e-9));
    REQUIRE(sample.latency.mean_s < previous);
    previous = sample.latency.mean_s;
  }
  REQUIRE_THAT(previous, WithinAbs(2.02, 1e-9));
}

namespace {
// The review's probe (mix.yaml): 1008 requests/s into a default blob store, 99.21 % of them 2 KB
// reads (1000/s) and 0.79 % 50 MB writes (7.9632/s). `variability` is the store's
// advanced.service_variability, the per-object c_s^2, when given (default 0: a deterministic
// transfer).
Scenario small_reads_beside_large_writes(std::optional<double> variability = {}) {
  Scenario s = blob_store_chain("write", 1008, simsys::Bytes{50e6},
                                NodeSpec{.id = "store",
                                         .type = NodeType::blob_store,
                                         .advanced = {.service_variability = variability}});
  auto& workload = *s.nodes[0].workload;
  workload.op_shares = {{"read", 0.9921}, {"write", 0.0079}};
  workload.sizes = {{"read", simsys::Bytes{2e3}}, {"write", simsys::Bytes{50e6}}};
  return s;
}

// What a 50 MB write waits: its response less its 2 s transfer (zero-latency link).
double write_wait(const Graph& g) {
  return response_to(first_step(g), "write").response.mean_s - 2.0;
}
}  // namespace

// The size mix's spread. The writes alone, 7.9632/s of 50 MB, are rho 0.79632 of a default store
// and, every one holding a slot the same 2 s (c_s^2 = 0, M/D/c), wait
//   (1 + 0) / 2 x C(20, 0.79632) x 2 s / (20 x 0.20368) = 60.9 ms.
// Add 1000 reads/s of 2 KB (80 us of a slot each): rho rises to 0.80032. A mean object time alone
// (E[S] = 0.9921 x 80 us + 0.0079 x 2 s = 15.88 ms) would cut the writes' wait to 1.02 ms (what an
// earlier version did: the reads look like they make the store faster). The mixture's SCV says how
// spread those times are:
//   E[S^2] = 0.9921 x (80 us)^2 + 0.0079 x (2 s)^2 = 0.0316 s^2   (per-object c_s^2 = 0)
//   c_s^2  = E[S^2] / E[S]^2 - 1 = 0.0316 / 0.01588^2 - 1 = 124.3
//   Wq     = (1 + 124.3) / 2 x C(20, 0.80032) x 15.88 ms / (20 x 0.19968) = 62.66 x 1.021 ms
//          = 64.0 ms
// more than the writes alone: adding load never shortens the wait.
TEST_CASE("small reads beside large writes lengthen the writes' wait: 64.0 ms against 60.9 ms",
          "[theory]") {
  const double alone =
      write_wait(*Graph::build(blob_store_chain("write", 7.9632, simsys::Bytes{50e6})));
  const double mixed = write_wait(*Graph::build(small_reads_beside_large_writes()));
  REQUIRE_THAT(alone, WithinAbs(0.060868, 1e-6));
  REQUIRE_THAT(mixed, WithinAbs(0.063980, 1e-6));
  REQUIRE(mixed > alone);
}

// advanced.service_variability still overrides the per-object c_s^2: at 1 (each transfer
// exponential, M/M/c's assumption) E[S^2] doubles, c_s^2 = 2 x 125.32 - 1 = 249.6, and the writes
// wait (1 + 249.6) / 2 x 1.021 ms = 128.0 ms, the review's ~127 ms; alone they wait the M/M/c
// 121.7 ms.
TEST_CASE("a blob store with service_variability 1 waits as exponential transfers: 128.0 ms",
          "[theory]") {
  const double mixed = write_wait(*Graph::build(small_reads_beside_large_writes(1.0)));
  REQUIRE_THAT(mixed, WithinAbs(0.127960, 1e-6));
  const double alone = write_wait(*Graph::build(blob_store_chain(
      "write", 7.9632, simsys::Bytes{50e6},
      NodeSpec{
          .id = "store", .type = NodeType::blob_store, .advanced = {.service_variability = 1.0}})));
  REQUIRE_THAT(alone, WithinAbs(0.121736, 1e-6));
  REQUIRE(mixed > alone);
}

namespace {
// u (`streams`/s of `stream`, each 50 MB) -> cdn_us (byte capacity `bytes`, every stream a hit so
// the origin sees nothing: fixed hit ratio 1) -> origin (a service). Zero-latency links. Node
// indices: u 0, cdn_us 1, origin 2.
Scenario byte_cdn_chain(double streams, simsys::ByteRate bytes) {
  Scenario s{.name = "byte-cdn", .version = 1, .duration = 10s};
  NodeSpec cdn{.id = "cdn_us",
               .type = NodeType::cdn,
               .byte_capacity = bytes,
               .hit_ratio = HitRatio{.fixed = 1.0}};
  cdn.origin = "origin";
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{streams}},
                                           .op_shares = {{"stream", 1.0}},
                                           .sizes = {{"stream", simsys::Bytes{50e6}}}}},
             std::move(cdn),
             NodeSpec{.id = "origin", .type = NodeType::service, .capacity = Rate{1000}}};
  s.links = {LinkSpec{.from = "u", .to = "cdn_us", .latency = 0s},
             LinkSpec{.from = "cdn_us", .to = "origin", .latency = 0s}};
  return s;
}
}  // namespace

// A CDN sized in bytes per second (`capacity: 100GB/s`) is a byte-sized node (spec §19 entry 2): it
// counts megabytes, every op that reaches it has a size (Graph::request_bytes), and the engine runs
// it. The numbers it runs on are pinned by the byte-sized tests below. Only the che hit ratio
// without `keys` (next test) still keeps a CDN from running. The node is called cdn_us as in
// book/youtube; its streams are sized as there.
TEST_CASE("a cdn with a byte capacity builds and the engine runs it", "[theory]") {
  const auto g = Graph::build(byte_cdn_chain(1000, simsys::ByteRate{100e9}));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// A 100 GB/s CDN is 100e9 / 1e6 = 100k units/s, and its 10 ms per MB (book: "read 1 MB
// sequentially from the network") make c = 100k/s x 10 ms = 1000 transfers in flight. 1k streams/s
// of 50 MB are 50k units/s: rho = 50k / 100k = 0.5. A stream holds a slot for its whole transfer,
// s_obj = 50 MB x 10 ms = 0.5 s, and Erlang C(1000, 0.5) = 3e-86, so Wq = 3e-86 x 0.5 s / 500:
// nothing waits. A stream is a read, answered at its first megabyte: 10 ms.
TEST_CASE("a 100 GB per second cdn at 1k streams of 50 MB per second is at rho 0.5", "[theory]") {
  const auto g = *Graph::build(byte_cdn_chain(1000, simsys::ByteRate{100e9}));
  const auto r = first_step(g);
  const auto& cdn = r.nodes[*g.index_of("cdn_us")];
  REQUIRE_THAT(cdn.demand_per_s, WithinRel(50'000.0, 1e-12));
  REQUIRE_THAT(cdn.rho, WithinRel(0.5, 1e-12));
  REQUIRE(cdn.dropped_per_s == 0.0);
  const auto wait = simsys::queueing::mmc_wait(1000, 0.5, 0.5).mean_s;
  REQUIRE_THAT(cdn.latency.mean_s, WithinRel(wait + 0.010, 1e-9));
  REQUIRE_THAT(cdn.latency.mean_s, WithinAbs(0.010, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.010, 1e-9));
}

// 2k streams/s x 50 MB = 100 GB/s: the cdn's whole capacity, 100k units/s, rho = 1.0, the
// saturation point of the napkin (100e9 / 50e6 = 2k streams/s). Beyond it the node sheds: at
// 2.2k streams/s demand is 110k units/s against a capacity of 100k (an offered rho of 1.1); the
// node drains 100k and, once its 1000-unit queue (the generic default) is full, drops the excess,
// 110k - 100k = 10k units/s, at every step (per step: demand dt = (drain + dropped) dt + change in
// backlog).
TEST_CASE("a 100 GB per second cdn saturates at 2k streams of 50 MB per second", "[theory]") {
  SECTION("2k streams per second is exactly full") {
    const auto g = *Graph::build(byte_cdn_chain(2000, simsys::ByteRate{100e9}));
    const auto r = first_step(g);
    const auto& cdn = r.nodes[*g.index_of("cdn_us")];
    REQUIRE_THAT(cdn.demand_per_s, WithinRel(100'000.0, 1e-12));
    REQUIRE_THAT(cdn.rho, WithinRel(1.0, 1e-12));
  }
  SECTION("2.2k streams per second drops the excess once the queue is full") {
    const auto g = *Graph::build(byte_cdn_chain(2200, simsys::ByteRate{100e9}));
    auto sim = Simulation::create(g);
    if (!sim) FAIL(sim.error().message);
    simsys::engine::StepRecord last;
    for (int i = 0; i < 20; ++i) last = sim->step(i * 100ms, 100ms);
    const auto& cdn = last.nodes[*g.index_of("cdn_us")];
    REQUIRE_THAT(cdn.demand_per_s, WithinRel(110'000.0, 1e-12));  // 2.2k/s x 50 MB
    REQUIRE_THAT(cdn.drain_per_s, WithinRel(100'000.0, 1e-9));    // the whole capacity, no more
    REQUIRE_THAT(cdn.dropped_per_s, WithinRel(10'000.0, 1e-6));
  }
}

// ---- Which byte-sized nodes the engine runs ----

namespace {
// u -> store (a blob store, 50 MB uploads at 1/s) -> svc, with the store's `then` step notifying
// svc: the shape a blob store's events make asynchronous. Node indices: u 0, store 1, svc 2.
Scenario blob_store_with_event() {
  auto s = blob_store_chain("upload", 1, simsys::Bytes{50e6},
                            NodeSpec{.id = "store",
                                     .type = NodeType::blob_store,
                                     .then = {Step{.to = "svc", .op = "notify"}}});
  s.nodes.push_back(NodeSpec{.id = "svc", .type = NodeType::service});
  s.links.push_back(LinkSpec{.from = "store", .to = "svc", .latency = 0s});
  return s;
}
}  // namespace

TEST_CASE("Simulation::create accepts a blob store without then steps", "[theory]") {
  const auto g = Graph::build(blob_store_chain("upload", 5, simsys::Bytes{50e6}));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

TEST_CASE("Simulation::create accepts a blob store whose writes notify through then steps",
          "[theory]") {
  const auto g = Graph::build(blob_store_with_event());
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// ---- A blob store's then steps are asynchronous event notifications (spec §19 entries 6-7)
// ----
//
// S3 event notifications (book ch. 14: "transcoding starts when the upload completes"): for every
// WRITE the store SERVED (an op in its `write_ops`, default `[write]`: S3's ObjectCreated), each
// `then` step sends its op to its target, and nobody waits for it. A read fires nothing: S3 has
// no event for a GET. So the uploader's response and answered rate end at the store, and the
// target only receives load.
//
// The numbers below come from one picture: a client sends 2 uploads/s of 100 MB to a default blob
// store (spec section 6: 500 MB/s, 40 ms per MB), over a 250 us link (0.5 ms round trip):
//   demand   = 2/s x 100 MB = 200 units (MB) per second against 500: rho = 200 / 500 = 0.4;
//   c        = 500 units/s x 40 ms = 20 slots (Little's law);
//   s_obj    = 100 MB x 40 ms = 4 s, the time one upload holds a slot;
//   Erlang C(20, 0.4) = 0.00026495 (queueing::mmc_wait takes rho per server), so the M/M/c wait
//   would be C x s_obj / (c (1 - rho)) = 0.00026495 x 4 s / (20 x 0.6) = 0.0883 ms, and every
//   upload holding a slot the same 4 s (a deterministic transfer, M/D/c) halves it:
//   Wq       = (1 + 0) / 2 x 0.0883 ms = 0.0442 ms.
// Listed in `write_ops` (as youtube's original_store lists `upload`), the upload is a write,
// answered at its last byte and notifying svc: Wq + 4 s + 0.5 ms = 4.000544 s.
// An op that is not in the store's `write_ops` is a read, answered at its first megabyte and
// notifying nobody: response = Wq + 40 ms + 0.5 ms = 40.544 ms.
// Either way `svc`, which takes 500 ms per notification, adds nothing to the client's response.

namespace {
constexpr double upload_mmc_wait_s = 8.8316e-5;  // M/M/c's wait above, to the derivation's digits
// M/D/c's wait, the store's: half of M/M/c's (the Allen-Cunneen factor (1 + 0) / 2).
[[nodiscard]] double upload_wait_s() {
  return simsys::queueing::mmc_wait(20, 0.4, 4.0).mean_s / 2.0;
}

// The service the store notifies: slow to the client's eyes if it counted (500 ms), so that a
// response that waited for it would be unmistakable.
NodeSpec notified_service() {
  return NodeSpec{.id = "svc", .type = NodeType::service, .capacity = Rate{1000}, .latency = 500ms};
}

// The store's `write_ops` in most tests below: the upload is a write, so it fires the events.
const std::optional<std::vector<std::string>> upload_is_a_write =
    std::vector<std::string>{"upload"};

// u (`rate` uploads/s of 100 MB, 250 us link) -> store (a default blob store) -> svc over a 1 ms
// link, the store's `then` being `then`. `write_ops` is the store's (std::nullopt: the default
// `[write]`, so the upload is a read). Node indices: u 0, store 1, svc 2.
Scenario upload_with_events(std::vector<Step> then,
                            std::optional<std::vector<std::string>> write_ops = upload_is_a_write,
                            NodeSpec svc = notified_service(), double rate = 2.0) {
  Scenario s{.name = "blob-events", .version = 1, .duration = 10s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{"upload", 1.0}},
                                           .sizes = {{"upload", simsys::Bytes{100e6}}}}},
             NodeSpec{.id = "store",
                      .type = NodeType::blob_store,
                      .then = std::move(then),
                      .write_ops = std::move(write_ops)},
             std::move(svc)};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 250us},
             LinkSpec{.from = "store", .to = "svc", .latency = 1ms}};
  return s;
}

const std::vector<Step> notify_step{Step{.to = "svc", .op = "notify"}};
}  // namespace

TEST_CASE("a blob store sends its then target one notification per upload it served", "[theory]") {
  const auto g = *Graph::build(upload_with_events(notify_step));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(200.0, 1e-12));  // units (MB) per second
  REQUIRE_THAT(store.rho, WithinRel(0.4, 1e-12));
  REQUIRE_THAT(simsys::queueing::mmc_wait(20, 0.4, 4.0).mean_s, WithinAbs(upload_mmc_wait_s, 1e-9));
  // 2 uploads/s served, 2 notify/s sent: svc counts requests, not megabytes.
  REQUIRE_THAT(r.nodes[*g.index_of("svc")].demand_per_s, WithinRel(2.0, 1e-12));
}

// The upload is a read to this store (`write_ops` does not list it): answered at its first
// megabyte, 0.0442 ms + 40 ms after the wait, plus the 0.5 ms round trip: 40.544 ms. A read fires
// no event (S3 has none for a GET), so svc is sent nothing.
TEST_CASE("a read-style upload at a blob store does not notify and is answered in 40.54 ms",
          "[theory]") {
  const auto g = *Graph::build(upload_with_events(notify_step, std::nullopt));
  const auto r = first_step(g);
  const double wait = upload_wait_s();
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 0.040 + 0.0005, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040544, 1e-6));
  // The store's own sojourn is the response minus the link round trip.
  REQUIRE_THAT(r.nodes[*g.index_of("store")].latency.mean_s, WithinRel(wait + 0.040, 1e-9));
  REQUIRE(r.nodes[*g.index_of("svc")].demand_per_s == 0.0);
}

// The same upload counted as a write is answered at its last byte, 4 s + Wq + 0.5 ms, and notifies
// svc 2/s. The 500 ms svc is not in the response: the hop to svc is asynchronous.
TEST_CASE("a write-listed upload notifies and is answered at its last byte, svc not included",
          "[theory]") {
  const auto g = *Graph::build(upload_with_events(notify_step));
  const auto r = first_step(g);
  const double wait = upload_wait_s();
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 4.0 + 0.0005, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(4.000544, 1e-6));
  REQUIRE_THAT(r.nodes[*g.index_of("svc")].demand_per_s, WithinRel(2.0, 1e-12));
}

// A reviewer's probe: 1000 viewers/s read 1 MB videos from the store and one uploader/s writes a
// 100 MB one; the store's `then` asks a transcoder for one job per event. The transcoder must see 1
// job/s, the uploads, not 1001/s: S3 notifies on ObjectCreated, never on a GET. (With reads
// included, the transcoder of 10 jobs/s was offered 1001/s, 100x its capacity.)
TEST_CASE("1000 reads/s and 1 upload/s at a blob store send its transcoder 1 job/s", "[theory]") {
  Scenario s{.name = "views-and-uploads", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "viewers",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}},
                                    .op_shares = {{"get", 1.0}},
                                    .sizes = {{"get", simsys::Bytes{1e6}}}}},
      NodeSpec{.id = "uploader",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1}},
                                    .op_shares = {{"upload", 1.0}},
                                    .sizes = {{"upload", simsys::Bytes{100e6}}}}},
      NodeSpec{.id = "store",
               .type = NodeType::blob_store,
               .byte_capacity = simsys::ByteRate{5e9},
               .then = {Step{.to = "transcode", .op = "job"}},
               .write_ops = std::vector<std::string>{"upload"}},
      NodeSpec{.id = "transcode", .type = NodeType::service, .capacity = Rate{10}, .latency = 1s}};
  s.links = {LinkSpec{.from = "viewers", .to = "store"},
             LinkSpec{.from = "uploader", .to = "store"},
             LinkSpec{.from = "store", .to = "transcode"}};
  const auto g = *Graph::build(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  REQUIRE_THAT(e.nodes[*g.index_of("transcode")].demand_per_s, WithinRel(1.0, 1e-9));
}

// svc is a 1/s service with a waiting room of 1, offered 2 notify/s: in the long run it drains
// 1/s and drops the other 1/s, a drop rate of 1 / 2 = 0.5. The client never hears of it: its
// uploads are answered at the store, 2/s of 2/s, at the same 4.000544 s.
TEST_CASE(
    "an undersized notified service drops half its notifications and the uploader still "
    "gets every answer",
    "[theory]") {
  const auto undersized = NodeSpec{.id = "svc",
                                   .type = NodeType::service,
                                   .capacity = Rate{1},
                                   .latency = 10ms,
                                   .advanced = {.queue_limit = 1}};
  const auto g = *Graph::build(upload_with_events(notify_step, upload_is_a_write, undersized));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  const auto& svc = e.nodes[*g.index_of("svc")];
  REQUIRE_THAT(svc.demand_per_s, WithinRel(2.0, 1e-9));
  REQUIRE_THAT(svc.dropped_per_s, WithinRel(1.0, 1e-6));
  REQUIRE_THAT(svc.dropped_per_s / svc.demand_per_s, WithinRel(0.5, 1e-6));
  REQUIRE(e.nodes[*g.index_of("store")].dropped_per_s == 0.0);
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(2.0, 1e-9));
  const double wait = upload_wait_s();
  REQUIRE_THAT(e.responses[0].response.mean_s, WithinRel(wait + 4.0 + 0.0005, 1e-9));
}

// `factor` multiplies a notification: one upload, four events (say four renditions to make).
TEST_CASE("a notification's factor multiplies the notifications sent: 4 per upload is 8 per second",
          "[theory]") {
  const auto g =
      *Graph::build(upload_with_events({Step{.to = "svc", .op = "notify", .factor = 4.0}}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("svc")].demand_per_s, WithinRel(8.0, 1e-12));
  // The store is untouched by what it sends: same rho, same answer.
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.4, 1e-12));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(4.000544, 1e-6));
}

// The size walk (spec §19 entry 1) is how the graph resolves it, and the engine must accept all
// three shapes: a step that keeps the op name carries the upload's 100 MB (1e8 B), a renamed step
// is unsized unless it states `size:`. svc counts requests, so it takes an unsized notify.
TEST_CASE(
    "a notification carries the upload's size when the op keeps its name and none when "
    "renamed",
    "[theory]") {
  SECTION("same op: the incoming 100 MB") {
    const auto g = *Graph::build(upload_with_events({Step{.to = "svc", .op = "upload"}}));
    REQUIRE(Simulation::create(g).has_value());
    REQUIRE_THAT(g.request_bytes(*g.index_of("svc"), "upload")->count, WithinRel(1e8, 1e-12));
  }
  SECTION("renamed, without size: unsized") {
    const auto g = *Graph::build(upload_with_events(notify_step));
    REQUIRE(Simulation::create(g).has_value());
    REQUIRE_FALSE(g.request_bytes(*g.index_of("svc"), "notify").has_value());
  }
  SECTION("renamed, with size: the step's own 1 KB") {
    const auto g = *Graph::build(
        upload_with_events({Step{.to = "svc", .op = "notify", .size = simsys::Bytes{1e3}}}));
    REQUIRE(Simulation::create(g).has_value());
    REQUIRE_THAT(g.request_bytes(*g.index_of("svc"), "notify")->count, WithinRel(1e3, 1e-12));
  }
}

// 10 uploads/s of 100 MB are 1000 MB/s against 500: the store serves half, 5 uploads/s, and the
// other half is dropped. A notification fires for a request the store SERVED, so svc is sent 5/s,
// not the 10/s offered.
TEST_CASE("an overloaded blob store notifies only for the uploads it served", "[theory]") {
  const auto g =
      *Graph::build(upload_with_events(notify_step, upload_is_a_write, notified_service(), 10.0));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  const auto& store = e.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(store.drain_per_s, WithinRel(500.0, 1e-9));  // 5 uploads/s of 100 MB
  REQUIRE_THAT(e.nodes[*g.index_of("svc")].demand_per_s, WithinRel(5.0, 1e-9));
}

// estimate is the long-run state of run: constant rates, so the last step of a run equals it.
TEST_CASE("estimate and run agree on a blob store with a notification", "[theory]") {
  const auto g =
      *Graph::build(upload_with_events({Step{.to = "svc", .op = "notify", .factor = 4.0}}));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  auto running = *Simulation::create(g);
  simsys::engine::StepRecord last;
  running.run({.duration = 2s, .step = 100ms},
              [&](const simsys::engine::StepRecord& r) { last = r; });
  const auto e = sim->estimate();
  for (const auto* id : {"store", "svc"}) {
    CAPTURE(id);
    const auto node = *g.index_of(id);
    REQUIRE_THAT(last.nodes[node].rho, WithinRel(e.nodes[node].rho, 1e-9));
    REQUIRE_THAT(last.nodes[node].demand_per_s, WithinRel(e.nodes[node].demand_per_s, 1e-9));
  }
  REQUIRE_THAT(e.nodes[*g.index_of("svc")].demand_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinRel(e.responses[0].response.mean_s, 1e-9));
  REQUIRE_THAT(last.responses[0].answered.per_second,
               WithinRel(e.responses[0].answered.per_second, 1e-9));
}

// ---- A link's bandwidth is a pipe that queues and drops megabytes (spec §19 entry 3)
// ----
//
// A link that states `bandwidth` is a pipe (spec section 3.4: it "caps bytes/s and queues the
// excess like a node"): ONE server (c = 1) whose unit is 1 MB, served in 1 MB / bandwidth. At
// 1 Gbit/s = 125 MB/s that is 8 ms per MB, deterministic (service SCV 0 per object; the SCV of
// the size mix when several ops share the pipe, as at a byte-sized node), with a waiting room of
// 1000 MB (the generic queue_limit default read in megabytes) beyond which it drops. The pipe is
// fed by the DEMAND that crosses the link: each request is charged its whole stated size in the
// direction it is sent, whichever way its bytes really travel, and a reply adds no bytes (so an
// `inout` link is one pipe, and `a->b` and `b->a` with a bandwidth each are two).
//
// What a hop over the pipe adds to the round trip, once: the pipe's WAIT plus the request's answer
// time on the wire, by spec §19 entry 2's rule for the node behind it:
//  - a WRITE (an op in the receiver's write_ops; a service's is `write`) its WHOLE serialisation,
//    size x 1 MB / bandwidth: the receiver acts after the last byte;
//  - any other op its FIRST megabyte, min(size, 1 MB) x 1 MB / bandwidth: the rest streams in
//    while the receiver answers at its first megabyte.
// For a 1 MB request both are 8 ms. The pipe's queue waits for whole requests either way (each
// holds the one server for its whole serialisation). The pipe's losses multiply the hop's
// delivered fraction.
//
// Interfaces these tests propose (the implementer may adjust the shape, not the numbers):
//  - `engine::StepRecord::links`: a std::vector<engine::LinkSample>, ONE entry per link that has
//    a bandwidth, in Graph::links() order (empty for a graph without any bandwidth);
//  - `engine::LinkSample{demand_per_s, drain_per_s, dropped_per_s, backlog, rho, latency, zone}`,
//    as NodeSample's fields of those names, in MEGABYTES (per second for the rates); `latency` is
//    the request-weighted mean of what a request waits in the pipe plus its answer time on the
//    wire (above).
//
// Every number below is derived on paper from one picture: client `a` sends `get`s of 1 MB at
// 100/s to service `b` (capacity 1M/s, 1 ms: no queue of its own) over a one-way 250 us link of
// 1 Gbit/s:
//   demand      = 100/s x 1 MB = 100 MB/s;  capacity = 125 MB/s (1e9 bit/s / 8 bit per byte)
//   rho         = 100 / 125 = 0.8                              (yellow: 0.70 <= rho < 0.90)
//   s_MB        = 1 MB / 125 MB/s = 8 ms;  a 1 MB object holds the pipe s_obj = 8 ms
//   Wq (M/D/1)  = (1 + 0) / 2 x rho / (1 - rho) x s_obj = 0.5 x 0.8 / 0.2 x 8 ms = 16 ms
//                 (Allen-Cunneen / Kingman with Poisson arrivals, c_a^2 = 1, and a deterministic
//                 service, c_s^2 = 0; for one server it is exactly Pollaczek-Khinchine's
//                 Wq = lambda E[S^2] / (2 (1 - rho)) = 100 x (8 ms)^2 / 0.4 = 16 ms)
//   sojourn     = Wq + s_obj = 16 ms + 8 ms = 24 ms
//   round trip  = 2 x 250 us + 24 ms + b's own sojourn (1 ms, plus a wait of ~0 at rho 1e-4)

namespace {
// one_gbit, pipe_over(rate, size, bandwidth, dir), only_link(r) and records_of(g, steps), with
// pipe_mb_per_s below, are shared with the rho-1 and pool tests: tests/support/fixtures.cppm.

constexpr double room_mb = 1000.0;  // the pipe's waiting room: the generic queue_limit in MB
constexpr double step_s = 0.1;      // the scenario's step in seconds
}  // namespace

TEST_CASE("1 MB gets at 100 per second over 1 Gbit/s run the pipe at rho 0.8 and wait 16 ms",
          "[theory]") {
  const auto g = *Graph::build(pipe_over(100, simsys::Bytes{1e6}));
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(100.0, 1e-12));  // MB per second, not requests
  REQUIRE_THAT(pipe.drain_per_s, WithinRel(100.0, 1e-12));
  REQUIRE(pipe.dropped_per_s == 0.0);
  REQUIRE(pipe.backlog == 0.0);
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));
  REQUIRE(pipe.zone == simsys::fluid::Zone::yellow);
  // M/D/1: half of the M/M/1 wait 0.8 / 0.2 x 8 ms = 32 ms (queueing::variability_factor(1, 0)).
  const double wait = simsys::queueing::variability_factor(1.0, 0.0) *
                      simsys::queueing::mmc_wait(1, 0.8, 0.008).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.016, 1e-12));
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(wait + 0.008, 1e-9));
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.024, 1e-9));  // the napkin number: 24 ms
}

// The hop adds the pipe's 24 ms once to the round trip, beside the link's own 2 x 250 us: the
// same scenario with the bandwidth removed differs by exactly that, and b sees the same 100/s.
TEST_CASE("the pipe adds its 24 ms sojourn once to the round trip beside the link's 0.5 ms",
          "[theory]") {
  const auto with = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6})));
  const auto without = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, std::nullopt)));
  const double b_sojourn = with.nodes[1].latency.mean_s;
  REQUIRE_THAT(b_sojourn, WithinAbs(0.001, 1e-9));  // 1 ms, no wait at rho 1e-4
  REQUIRE_THAT(without.nodes[1].latency.mean_s, WithinRel(b_sojourn, 1e-12));
  REQUIRE_THAT(without.responses[0].response.mean_s, WithinRel(0.0005 + b_sojourn, 1e-9));
  REQUIRE_THAT(with.responses[0].response.mean_s, WithinRel(0.0005 + 0.024 + b_sojourn, 1e-9));
  REQUIRE_THAT(with.responses[0].response.mean_s - without.responses[0].response.mean_s,
               WithinAbs(0.024, 1e-9));
  REQUIRE_THAT(with.nodes[1].demand_per_s, WithinRel(100.0, 1e-12));  // requests at b
}

// A 10 MB request at 10/s fills the pipe as much as 1 MB at 100/s (100 MB/s, rho 0.8 again) but
// holds it ten times longer: s_obj = 10 x 8 ms = 80 ms, one whole object at a time.
//   Wq (M/D/1) = 0.5 x 0.8 / 0.2 x 80 ms = 160 ms     (= 10 x (80 ms)^2 / 0.4 by
//                                                     Pollaczek-Khinchine)
// What the hop adds after that wait depends on what b does with the request:
//   a read  (`get`)   = 160 ms + its first megabyte, 8 ms = 168 ms: b answers at its first MB
//                       while the other 9 MB stream in behind it;
//   a write (`write`) = 160 ms + the WHOLE request, 80 ms = 240 ms: b acts after the last byte.
TEST_CASE("a 10 MB read waits 160 ms and its first megabyte takes 8 ms: a 168 ms hop", "[theory]") {
  const auto g = *Graph::build(pipe_over(10, simsys::Bytes{10e6}));
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(100.0, 1e-12));  // 10 requests/s x 10 MB
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.8, 0.080).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.160, 1e-12));
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(wait + 0.008, 1e-9));
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.168, 1e-9));
  const double b_sojourn = r.nodes[1].latency.mean_s;
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(10.0, 1e-12));  // b counts requests
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.0005 + 0.168 + b_sojourn, 1e-9));
}

TEST_CASE("a 10 MB write waits 160 ms and is serialised whole for 80 ms: a 240 ms hop",
          "[theory]") {
  auto s = pipe_over(10, simsys::Bytes{10e6});
  s.nodes[0].workload->op_shares = {{"write", 1.0}};
  s.nodes[0].workload->sizes = {{"write", simsys::Bytes{10e6}}};
  const auto g = *Graph::build(s);
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));  // the same queue as the read's
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.240, 1e-9));
  const double b_sojourn = r.nodes[1].latency.mean_s;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.0005 + 0.240 + b_sojourn, 1e-9));
}

// Whether a hop pays a request whole on the wire is decided per (link, op) at build: an op is a
// write on a link if a node it reaches under its own name lists it in `write_ops`, the receiver or
// any node behind it. A 10 MB `upload` at 5/s over 1 Gbit/s into a blob store that lists `upload`:
//   rho = 5 x 10 MB / 125 MB/s = 0.4;  s_obj = 10 x 8 ms = 80 ms
//   Wq (M/D/1) = 0.5 x 0.4 / 0.6 x 80 ms = 26.67 ms;  a write's answer: the whole 80 ms
//   pipe = 106.67 ms, whether the link ends at the store or at a load balancer in front of it.
// Before the fix the balancer (whose own `write_ops` is the default `[write]`) made it a read on
// the wire: 26.67 + 8 = 34.67 ms, 72 ms less than the same bytes straight into the store.
namespace {
// a (5 `upload`s/s of 10 MB) -> [lb ->] store (a default blob store whose write op is `upload`),
// the first hop over 1 Gbit/s.
Scenario upload_over_pipe(bool behind_balancer) {
  auto s = pipe_over(5, simsys::Bytes{10e6});
  s.nodes[0].workload->op_shares = {{"upload", 1.0}};
  s.nodes[0].workload->sizes = {{"upload", simsys::Bytes{10e6}}};
  NodeSpec store{.id = "store", .type = NodeType::blob_store};
  store.write_ops = std::vector<std::string>{"upload"};
  if (!behind_balancer) {
    s.nodes[1] = std::move(store);
    s.links[0].to = "store";
    return s;
  }
  s.nodes[1] = NodeSpec{.id = "lb", .type = NodeType::load_balancer};
  s.nodes.push_back(std::move(store));
  s.links[0].to = "lb";
  s.links.push_back(LinkSpec{.from = "lb", .to = "store", .latency = 250us});
  return s;
}
}  // namespace

TEST_CASE("a 10 MB upload behind a load balancer pays the wire as a write, as straight in",
          "[theory]") {
  const auto straight = first_step(*Graph::build(upload_over_pipe(false)));
  const auto balanced = first_step(*Graph::build(upload_over_pipe(true)));
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.4, 0.080).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.026667, 1e-6));
  REQUIRE_THAT(only_link(straight).rho, WithinRel(0.4, 1e-12));
  REQUIRE_THAT(only_link(straight).latency.mean_s, WithinRel(wait + 0.080, 1e-9));
  REQUIRE_THAT(only_link(balanced).latency.mean_s,
               WithinRel(only_link(straight).latency.mean_s, 1e-12));
}

// A renamed step that states its own `size:` forwards a payload: the service had the client's whole
// body before it sent it on, so when the store behind it writes the renamed op, the client's hop
// into the service is a write on the wire as well. 10 MB `upload`s at 5/s over 1 Gbit/s into a
// service whose step is `{to: store, op: put, size: 10MB}`, in front of a blob store whose write op
// is `put`, pay the same 26.67 + 80 = 106.67 ms on the wire as `{to: store, op: upload, size:
// 10MB}` in front of one that writes `upload`, and the client gets the same response. Before the
// fix the walk stopped at the renamed step: the hop was paid as a read, 26.67 + 8 = 34.67 ms, 72 ms
// less for the same bytes.
namespace {
// a (5 `upload`s/s of 10 MB) -> b, over 1 Gbit/s; b's `upload` sends `store_op` (10 MB) on to a
// default blob store that lists `store_op` as its write.
Scenario upload_relayed_as(const std::string& store_op) {
  auto s = pipe_over(5, simsys::Bytes{10e6});
  s.nodes[0].workload->op_shares = {{"upload", 1.0}};
  s.nodes[0].workload->sizes = {{"upload", simsys::Bytes{10e6}}};
  s.nodes[1].ops = {{"upload", {Step{.to = "store", .op = store_op, .size = simsys::Bytes{10e6}}}}};
  NodeSpec store{.id = "store", .type = NodeType::blob_store};
  store.write_ops = std::vector<std::string>{store_op};
  s.nodes.push_back(std::move(store));
  s.links.push_back(LinkSpec{.from = "b", .to = "store", .latency = 250us});
  return s;
}
}  // namespace

TEST_CASE("an upload a service sends on as a sized put pays the client's wire as a write",
          "[theory]") {
  const auto renamed = first_step(*Graph::build(upload_relayed_as("put")));
  const auto same_op = first_step(*Graph::build(upload_relayed_as("upload")));
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.4, 0.080).mean_s;  // 26.67 ms
  REQUIRE_THAT(only_link(same_op).latency.mean_s, WithinRel(wait + 0.080, 1e-9));
  REQUIRE_THAT(only_link(renamed).latency.mean_s, WithinRel(wait + 0.080, 1e-9));
  REQUIRE_THAT(response_to(renamed, "upload").response.mean_s,
               WithinRel(response_to(same_op, "upload").response.mean_s, 1e-12));
}

// Only the body itself, relabelled, makes the client's hop a write: a step of another size is a new
// request. The same 10 MB uploads, but the service writes a 1 KB `put_meta` (the object's metadata)
// to a store that lists `put_meta`: the store writes 1 KB, not the client's 10 MB, so the client's
// hop stays a read on the wire, answered at its first megabyte: 26.67 + 8 = 34.67 ms, not the
// 106.67 ms of a forwarded body. (Before the fix any sized rename counted as forwarding the body,
// 72 ms too much.)
TEST_CASE("an upload whose service writes a 1 KB put_meta stays a read on the client's wire",
          "[theory]") {
  auto s = upload_relayed_as("put_meta");
  s.nodes[1].ops.at("upload").front().size = simsys::Bytes{1e3};
  const auto g = *Graph::build(s);
  REQUIRE(g.links()[0].write_ops.empty());
  const auto r = first_step(g);
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.4, 0.080).mean_s;  // 26.67 ms
  REQUIRE_THAT(only_link(r).latency.mean_s, WithinRel(wait + 0.008, 1e-9));
  REQUIRE_THAT(only_link(r).latency.mean_s, WithinAbs(0.034667, 1e-6));
}

// The same size written two ways is the same body: the client's `2.01MB` and the step's `2010KB`
// parse to 2.01 x 1e6 = 2009999.9999999998 and 2010 x 1e3 = 2010000 bytes, two doubles one rounding
// apart, so the step carries the upload on and the client's hop is a write on the wire. Sizes are
// compared in whole bytes (std::llround).
TEST_CASE("a 2010KB step carries a 2.01MB upload on: sizes compare in whole bytes", "[theory]") {
  auto s = upload_relayed_as("put");
  const auto upload = *simsys::parse_size("2.01MB");
  const auto step = *simsys::parse_size("2010KB");
  REQUIRE(upload.count != step.count);  // one rounding apart as doubles
  s.nodes[0].workload->sizes = {{"upload", upload}};
  s.nodes[1].ops.at("upload").front().size = step;
  const auto g = *Graph::build(s);
  REQUIRE(g.links()[0].write_ops == std::vector<std::string>{"upload"});
}

// Two ops of different sizes share one pipe: 20/s of `get` (1 MB, 8 ms, a read) and 20/s of `write`
// (4 MB, 32 ms, a write at b) are 20 + 80 = 100 MB/s, rho 0.8 again, but the objects are NOT
// alike. With p = 1/2 each:
//   E[S]   = 0.5 x 8 ms + 0.5 x 32 ms = 20 ms
//   E[S^2] = 0.5 x (8 ms)^2 + 0.5 x (32 ms)^2 = 544e-6 s^2     (each object's own SCV is 0)
//   c_s^2  = E[S^2] / E[S]^2 - 1 = 544e-6 / 400e-6 - 1 = 0.36  (the mixture's SCV)
//   Wq     = (1 + 0.36) / 2 x rho / (1 - rho) x E[S] = 0.68 x 4 x 20 ms = 54.4 ms
//            = lambda E[S^2] / (2 (1 - rho)) = 40 x 544e-6 / 0.4  (Pollaczek-Khinchine, exact for
//            one server): more than the 16 ms of 1 MB alone at the same rho. Both ops wait the
//            same Wq in the one queue; the read is then answered at its first (and only)
//            megabyte, the write after its whole 4 MB:
//   get    = 0.5 ms + 54.4 ms + 8 ms + b;   write = 0.5 ms + 54.4 ms + 32 ms + b
// and the pipe's own latency is the request-weighted mean, 54.4 + 0.5 x 8 + 0.5 x 32 = 74.4 ms.
TEST_CASE("two sizes on one pipe wait 54.4 ms, more than one size at the same rho: the mixture",
          "[theory]") {
  auto s = pipe_over(40, simsys::Bytes{1e6});
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"write", 0.5}};
  s.nodes[0].workload->sizes = {{"get", simsys::Bytes{1e6}}, {"write", simsys::Bytes{4e6}}};
  const auto g = *Graph::build(s);
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(100.0, 1e-12));
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));
  const double mean_s = 0.5 * 0.008 + 0.5 * 0.032;
  const double second_moment = 0.5 * 0.008 * 0.008 + 0.5 * 0.032 * 0.032;
  const double scv = second_moment / (mean_s * mean_s) - 1.0;
  REQUIRE_THAT(scv, WithinAbs(0.36, 1e-12));
  const double wait = simsys::queueing::variability_factor(1.0, scv) *
                      simsys::queueing::mmc_wait(1, 0.8, mean_s).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.0544, 1e-12));
  REQUIRE_THAT(40.0 * second_moment / (2.0 * (1.0 - 0.8)), WithinAbs(0.0544, 1e-12));  // P-K
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.0544 + 0.020, 1e-9));
  const double b_sojourn = r.nodes[1].latency.mean_s;
  REQUIRE_THAT(response_to(r, "get").response.mean_s,
               WithinRel(0.0005 + 0.0544 + 0.008 + b_sojourn, 1e-9));
  REQUIRE_THAT(response_to(r, "write").response.mean_s,
               WithinRel(0.0005 + 0.0544 + 0.032 + b_sojourn, 1e-9));
  // The same 100 MB/s as one size waits 16 ms: the spread of sizes is what lengthens the wait.
  const auto alone = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6})));
  const double alone_wait = only_link(alone).latency.mean_s - 0.008;  // 16 ms
  REQUIRE_THAT(alone_wait, WithinAbs(0.016, 1e-9));
  REQUIRE(wait > 3.0 * alone_wait);  // 54.4 ms against 16 ms
}

// A 40 Gbit/s link (youtube's cross-datacenter one) is 5 GB/s = 5000 MB/s, 0.2 ms per MB. 100 MB/s
// of it is rho = 0.02: Wq = 0.5 x 0.02 / 0.98 x 0.2 ms = 2.04 us, and the hop adds 0.2 ms + 2.04
// us. A wide pipe is nearly free; the model must not invent delay for it.
TEST_CASE("a 40 Gbit/s pipe at 100 MB per second adds 0.2 ms: rho 0.02, a wait of 2 us",
          "[theory]") {
  const auto g = *Graph::build(pipe_over(100, simsys::Bytes{1e6}, simsys::ByteRate{5e9}));
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.rho, WithinRel(0.02, 1e-12));
  REQUIRE(pipe.zone == simsys::fluid::Zone::green);
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.02, 0.0002).mean_s;
  REQUIRE_THAT(wait, WithinAbs(2.0408e-6, 1e-10));
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(wait + 0.0002, 1e-9));
  const auto without = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, std::nullopt)));
  REQUIRE_THAT(r.responses[0].response.mean_s - without.responses[0].response.mean_s,
               WithinRel(wait + 0.0002, 1e-6));
}

// 150 MB/s into a 125 MB/s pipe: rho is offered 1.2. The pipe drains 125/s, so the backlog grows
// by (150 - 125) x 0.1 s = 2.5 MB per step and the 1000 MB room is full after 1000 / 25 = 40 s,
// i.e. 400 steps. Until then nothing is dropped and every request is answered (a backlog is only
// a wait); from then on the pipe drops 150 - 125 = 25 MB/s. The room's drain time bounds the
// wait: 1000 MB / 125 MB/s = 8 s, so the hop's latency tops out at 8 s + 8 ms serialisation, and
// the client's delivered fraction is 125 / 150 = 0.8333 in the steady state.
TEST_CASE(
    "at 150 per second the pipe fills its 1000 MB room in 40 s and then drops 25 MB per second",
    "[theory]") {
  const auto g = *Graph::build(pipe_over(150, simsys::Bytes{1e6}));
  const auto records = records_of(g, 600);  // 60 s
  std::size_t first_drop = records.size();
  double latency_so_far = 0.0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& pipe = only_link(records[i]);
    CAPTURE(i);
    REQUIRE_THAT(pipe.drain_per_s, WithinRel(pipe_mb_per_s, 1e-9));  // the pipe is saturated
    REQUIRE(pipe.backlog <= room_mb + 1e-9);
    REQUIRE(pipe.latency.mean_s >= latency_so_far - 1e-12);                  // the wait only grows
    REQUIRE(pipe.latency.mean_s <= room_mb / pipe_mb_per_s + 0.008 + 1e-9);  // 8 s + 8 ms
    latency_so_far = pipe.latency.mean_s;
    if (first_drop == records.size() && pipe.dropped_per_s > 1e-6) first_drop = i;
  }
  // The room is full after step 399 (400 x 2.5 MB = 1000 MB, t = 40 s): the first drop is the
  // next step; one step either way absorbs the rounding of the accumulated backlog.
  REQUIRE(first_drop >= 399);
  REQUIRE(first_drop <= 401);
  // Before: 20 s in, 200 steps x 2.5 MB = 500 MB wait, nothing lost, every request answered.
  const auto& filling = records[199];
  REQUIRE_THAT(only_link(filling).backlog, WithinAbs(500.0, 1e-6));
  REQUIRE(only_link(filling).dropped_per_s == 0.0);
  REQUIRE_THAT(filling.responses[0].answered.per_second,
               WithinRel(filling.responses[0].offered.per_second, 1e-12));
  // After: the steady state at 60 s.
  const auto& last = records.back();
  const auto& pipe = only_link(last);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(150.0, 1e-12));
  REQUIRE_THAT(pipe.dropped_per_s, WithinRel(25.0, 1e-6));
  REQUIRE_THAT(pipe.backlog, WithinAbs(room_mb, 1e-6));
  REQUIRE_THAT(pipe.rho, WithinRel(1.0, 1e-9));
  REQUIRE(pipe.zone == simsys::fluid::Zone::red);
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(8.008, 1e-9));
  const auto& answer = last.responses[0];
  REQUIRE_THAT(answer.offered.per_second, WithinRel(150.0, 1e-12));
  REQUIRE_THAT(answer.answered.per_second / answer.offered.per_second,
               WithinRel(125.0 / 150.0, 1e-6));  // 0.8333
  REQUIRE_THAT(answer.answered.per_second, WithinRel(125.0, 1e-6));
  // b is sent what the pipe drained, 125/s, and loses none of it.
  REQUIRE_THAT(last.nodes[1].demand_per_s, WithinRel(125.0, 1e-6));
}

// The fluid queue's balance on the link, every step of the run above: what came in was served,
// dropped or is waiting, demand.dt = (drain + dropped).dt + (backlog - the previous backlog).
TEST_CASE(
    "the pipe conserves megabytes at every step: demand x dt = served + dropped + change in "
    "backlog",
    "[theory]") {
  const auto records = records_of(*Graph::build(pipe_over(150, simsys::Bytes{1e6})), 600);
  double previous_backlog = 0.0;
  bool grew = false;
  bool dropped = false;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& pipe = only_link(records[i]);
    CAPTURE(i);
    const double in_mb = pipe.demand_per_s * step_s;
    const double out_mb = (pipe.drain_per_s + pipe.dropped_per_s) * step_s;
    REQUIRE_THAT(in_mb, WithinAbs(out_mb + (pipe.backlog - previous_backlog), 1e-9));
    grew = grew || pipe.backlog > previous_backlog;
    dropped = dropped || pipe.dropped_per_s > 0.0;
    previous_backlog = pipe.backlog;
  }
  REQUIRE(grew);     // not vacuous: the backlog really filled ...
  REQUIRE(dropped);  // ... and the room really overflowed
}

// The pipe's losses multiply the hop's delivered fraction (plan: "its losses multiply the hop's
// delivered fraction"), as any node's drops do. Here b is the weaker one: a capacity of 100/s.
// The pipe passes 125 of 150 MB/s (5/6); b is sent 125/s, serves 100 and, once its own room of
// 1000 requests has filled, drops 25: it passes 100 / 125 = 0.8. The client is answered
// 150 x 5/6 x 0.8 = 100/s, b's capacity, a delivered fraction of 2/3. (Both rooms are full after
// 40 s and 1000 / 25 = 40 s: by 100 s the steady state holds.)
TEST_CASE("the pipe's drops and the next node's drops multiply in the client's delivered fraction",
          "[theory]") {
  auto s = pipe_over(150, simsys::Bytes{1e6});
  s.nodes[1].capacity = Rate{100};
  const auto g = *Graph::build(s);
  const auto records = records_of(g, 1000);  // 100 s
  const auto& last = records.back();
  REQUIRE_THAT(only_link(last).dropped_per_s, WithinRel(25.0, 1e-6));
  REQUIRE_THAT(last.nodes[1].dropped_per_s, WithinRel(25.0, 1e-6));
  REQUIRE_THAT(last.responses[0].answered.per_second, WithinRel(100.0, 1e-6));
  REQUIRE_THAT(last.responses[0].answered.per_second / last.responses[0].offered.per_second,
               WithinRel(2.0 / 3.0, 1e-6));
}

// estimate() is the long-run state `run` reaches: below capacity the pipe is empty and waits its
// 16 ms (the settle passes read it without the delay line, as for every node); above it the
// backlog is the full room, the pipe drops 25 MB/s and the client is answered 5/6 of its load.
TEST_CASE("estimate and run agree on the pipe, below capacity and above it", "[theory]") {
  SECTION("rho 0.8") {
    const auto g = *Graph::build(pipe_over(100, simsys::Bytes{1e6}));
    const auto e = (*Simulation::create(g)).estimate();
    const auto ran = records_of(g, 20).back();
    REQUIRE(e.converged);
    const auto& settled = only_link(e);
    const auto& stepped = only_link(ran);
    REQUIRE_THAT(settled.demand_per_s, WithinRel(100.0, 1e-9));
    REQUIRE_THAT(settled.drain_per_s, WithinRel(stepped.drain_per_s, 1e-9));
    REQUIRE_THAT(settled.rho, WithinRel(0.8, 1e-9));
    REQUIRE(settled.backlog == 0.0);
    REQUIRE_THAT(settled.latency.mean_s, WithinAbs(0.024, 1e-9));
    REQUIRE_THAT(settled.latency.mean_s, WithinRel(stepped.latency.mean_s, 1e-9));
    REQUIRE_THAT(e.responses[0].response.mean_s, WithinRel(ran.responses[0].response.mean_s, 1e-9));
  }
  SECTION("rho 1.2") {
    const auto e =
        (*Simulation::create(*Graph::build(pipe_over(150, simsys::Bytes{1e6})))).estimate();
    REQUIRE(e.converged);
    const auto& settled = only_link(e);
    REQUIRE_THAT(settled.drain_per_s, WithinRel(125.0, 1e-9));
    REQUIRE_THAT(settled.dropped_per_s, WithinRel(25.0, 1e-9));
    REQUIRE_THAT(settled.backlog, WithinAbs(room_mb, 1e-9));
    REQUIRE_THAT(settled.latency.mean_s, WithinRel(8.008, 1e-9));
    REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(125.0, 1e-9));
  }
}

// The pipe is fed by the requests alone, each charged its stated size in the direction it is
// sent: a reply adds no bytes, so an `inout` link (requests out, replies back on the same link) is
// ONE pipe with the same load as an `out` link, not two and not a doubled load. (Two links `a->b`
// and `b->a` that each state a bandwidth are two pipes, each fed by its own direction's demand.)
TEST_CASE("an inout link is one pipe fed by its requests alone: replies add no bytes", "[theory]") {
  const auto out = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6})));
  const auto both =
      first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, one_gbit, Direction::inout)));
  REQUIRE(both.links.size() == 1);
  REQUIRE_THAT(only_link(both).demand_per_s, WithinRel(100.0, 1e-12));
  REQUIRE_THAT(only_link(both).latency.mean_s, WithinRel(only_link(out).latency.mean_s, 1e-12));
  REQUIRE_THAT(both.responses[0].response.mean_s,
               WithinRel(out.responses[0].response.mean_s, 1e-12));
}

// A link without `bandwidth` behaves exactly as before pipes existed: no sample, and the round trip
// is the link's own 2 x 250 us plus b's sojourn, as in the blob store tests above (the same 0.5
// ms), whether or not the client states a size.
TEST_CASE("a link without a bandwidth has no pipe: no link sample and only its own latency",
          "[theory]") {
  SECTION("sized requests") {
    const auto r = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, std::nullopt)));
    REQUIRE(r.links.empty());
    REQUIRE_THAT(r.responses[0].response.mean_s,
                 WithinRel(0.0005 + r.nodes[1].latency.mean_s, 1e-9));
    REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0015, 1e-9));  // 0.5 ms + 1 ms
  }
  SECTION("unsized requests") {
    auto s = pipe_over(100, simsys::Bytes{1e6}, std::nullopt);
    s.nodes[0].workload->sizes.clear();
    const auto r = first_step(*Graph::build(s));
    REQUIRE(r.links.empty());
    REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0015, 1e-9));
  }
  SECTION("the long-run estimate has none either") {
    const auto e = (*Simulation::create(*Graph::build(mm1(500)))).estimate();
    REQUIRE(e.links.empty());
  }
}

// A pipe needs a size for every op it carries: an unsized one is refused at build, naming the
// link, the op and its sender, and what to add. (The graph tests pin more senders.)
TEST_CASE("an unsized op on a bandwidth link is refused with the advice to add a size",
          "[theory]") {
  Scenario s = pipe_over(100, simsys::Bytes{1e6});
  s.nodes[0].workload->op_shares = {{"ping", 1.0}};
  s.nodes[0].workload->sizes.clear();
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "link 'a'->'b' has a bandwidth, but op 'ping' from 'a' has no size: add size: to the "
          "step or sizes: to the client");
}
