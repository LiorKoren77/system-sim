// Module interface partition `simsys.engine:records`: what one step of the engine produces, the
// records every observer of a run reads (report::Summary, the CSV series writers, the fault log):
// NodeSample per node, ResponseSample per (client, op), LinkSample per bandwidth link, and the
// StepRecord that holds them. Plain value types with no behaviour; the step loop that fills them is
// engine.cppm's Simulation.
//
// Models: nothing of its own; the comments on each field say which model of the book produced the
// number (the fluid queue of ch. 1's boxes, a client's response time, a pipe's megabytes).
//
// C++20 module partition, interface kind (faults.cppm has the longer note): engine.cppm writes
// `export import :records;`, so an importer of simsys.engine sees these types as if engine.cppm
// declared them. A partition of their own because two units of the module read them, the primary
// interface (Simulation fills them) and the partition :fault_log (FaultLog reads them), and a
// partition cannot import the primary interface that re-exports it (an import cycle): the records
// must live below both.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this unit imports other modules (GCC 14.2 'Bad
// file data' bug, CLAUDE.md §3).
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

export module simsys.engine:records;

// C++20 `import`: the compiled interfaces of the modules these types are built from (SimTime and
// Rate from core, a latency estimate from flow, a zone from fluid).
import simsys.core;
import simsys.flow;
import simsys.fluid;

export namespace simsys::engine {

// One node's numbers for one step. Rates and backlog are summed over replicas (spec §16 entry 4);
// rho and zone are per copy, which all copies share — or, behind a consistent-hash balancer, those
// of the hottest copy, the one that breaks an SLO first, and for a SQL database those of the busier
// of its primary and its replicas — and latency is the sojourn a request sees: one copy's, or
// behind a hash ring the share-weighted mixture of the hot and the cool copies' (see
// Simulation::make_server), or for a database the mixture of its writes' (primary) and its reads'
// (replica) sojourns, weighted by their request rates (Simulation::make_database), and for a
// byte-sized node the mixture of its ops' sojourns, each answered at its own time, by their request
// rates (Simulation::request_weighted_megabyte_sojourn). Demand, drain, drops and backlog are in
// CAPACITY UNITS (per second for the rates), the unit `capacity` is a rate of (spec §17 entry 4): a
// request of an op with `cost` 3 counts three times. With every cost at its default of 1, units are
// requests, as in phase 1. The drop ratio dropped / demand is therefore the share of the node's
// work it shed. A kv_cluster's every request costs N units (spec §19 entry 9, defaults::op_cost):
// its demand, drain, drops and backlog are REPLICA-REQUESTS, N times the requests its clients sent
// (30k requests/s at N = 3 are 90k/s), the work its machines do; the clients' offered and answered
// rates (ResponseSample) stay in requests. `rejected_per_s` is a second way to shed (spec §17 entry
// 6): requests a rate limiter refused by POLICY, never forwarded. For the token bucket and the
// windowed algorithms they are refused after the limiter's queue drained them, so they are part of
// `drain_per_s` too; for a leaky bucket they are its queue's overflow ("bucket full"), which is
// then reported here and not in `dropped_per_s`. Drops are failures, rejections are the policy
// working: report::Summary keeps a drop rate and a reject rate apart. A limiter's every op costs 1,
// so these units are requests. Conservation, per node and per step (the fluid queue's balance,
// summed over copies):
//   demand·dt = (drain + dropped)·dt + Δbacklog
// whatever comes in is served, shed or left waiting. Rejections do not appear as a term of
// their own: a token-family limiter's (token bucket, windowed) are inside `drain` (its
// queue looked at them), and a leaky bucket's are the `dropped` term, moved to `rejected`. An
// external sink's failed requests are moved from `drain` to `dropped` (fail_share): the sum, and
// with it the balance, does not change.
// A database's replicas also replay every write the primary committed (replication); that
// work answers no client, so demand, drain, drops and backlog count the CLIENTS' queries only
// (the balance above holds for them: a replica full of replayed writes therefore drops while
// the backlog shown is below queue_limit × copies), and the replication load shows where it
// bites: in the replicas' rho, zone and the reads' sojourn (Simulation::clients_part), and in
// the replayed writes a replica drops (`replication_dropped_per_s`, one replica's, beside
// `committed_writes_per_s`, what one replica must replay).
struct NodeSample {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double rejected_per_s{};
  double backlog{};
  double rho{};
  flow::LatencyEstimate latency;  // own sojourn; zero for a client (a source has no queue)
  fluid::Zone zone{fluid::Zone::green};
  // How old the data a read may return is, in seconds (spec §18 entry 6): for a SQL database, the
  // write's sojourn at the primary (wait + service, until it is committed) + the replication lag
  // (shipping it) + its sojourn at a replica (waiting behind the reads and the other replayed
  // writes, then applied): a replica shows a write that long after the client sent it. 0 for a
  // database without replicas (every read is served by the primary, which has the latest write);
  // empty for every other node, which keeps no copies of data that could lag. Infinite when the
  // primary or the replica never answers. Finite while a replica drops replayed writes (below): the
  // model's queue is bounded, so this is the bounded-queue figure, while a real replica's lag grows
  // without bound (see replication_dropped_per_s).
  std::optional<double> staleness_s{};
  // The committed writes ONE replica failed to replay, per second, in REQUESTS (writes), not
  // capacity units: the replicas' total / ((N − 1) · S), every replica being alike. A replica
  // whose queue is full drops replayed writes with the rest: it falls behind without bound (the
  // model drops the excess at queue_limit; a real replica keeps it in its relay log/WAL until it
  // is rebuilt), and the `staleness` SLO fails on any. Per replica, so that it compares with
  // `committed_writes_per_s` (a sum over replicas could exceed every write committed). Counted
  // apart from `dropped_per_s`, which is the clients' queries. Same convention as
  // `staleness_s`: 0 for a database whose replicas keep up (or that has none), empty for every
  // other node.
  std::optional<double> replication_dropped_per_s{};
  // The writes ONE shard's primary committed, per second, in requests: the primaries' total / S,
  // and so the writes each replica of that shard must replay. Every database has it (0 when no
  // write arrives); empty for every other node.
  std::optional<double> committed_writes_per_s{};
  // A message queue's lag, in seconds (spec §19 entries 4 and 14; the queue between producers and
  // workers of ch. 10 and ch. 14): how long a message published at this step waits until its
  // slowest consumer group reads it. Kafka reports a group's lag as the records it has not read;
  // divided by the rate the group consumes them, that is this time. The broker holds nothing back
  // (it appends at 1M/s); the unread messages wait in each group's worker pool, so the lag is read
  // off the pools (Simulation::record_queue_lags):
  //   lag(group) = the pool's queueing wait this step (fluid::FluidQueue::wait)
  //              = max(min(k × Wq, T), backlog / capacity)
  //     backlog / capacity  the FIFO drain time: a message published now is read after everything
  //                         ahead of it, which the pool works off at its capacity (workers / cost)
  //                         while it is behind: 60,000 messages at 1200/s are 50 s;
  //     k × Wq              the stochastic part: Allen–Cunneen's factor k times the Erlang C wait
  //                         of an M/M/c queue (c = workers, service time = cost, rho clamped at
  //                         0.99): what a message waits with no backlog because every worker is
  //                         busy, 3.289 ms for 1k/s into 4 workers at 3.333 ms;
  //     T                   the room's drain time, = the queue's retention: a message older than
  //                         that is deleted unconsumed, so no lag passes it.
  //   The larger of the two terms is our heuristic (FluidQueue::wait), not a published result.
  //   lag(queue) = the largest over its groups: a queue is as far behind as its slowest reader.
  // It is not the age of the oldest unread message, which a dashboard may show instead: under
  // FIFO the message published now becomes the oldest one lag later, so the age trails this lag
  // by one lag and reaches the same peak that much later. The `queue_lag` SLO bounds the peak
  // after warmup, so it reads "every message published after warmup was read within `max`".
  // The wait only: the job's `cost` is not lag (a message is read once a worker takes it), so the
  // lag is the pool's latency minus its cost. Assumes the backlog is read first in, first out (a
  // Kafka partition is). Empty for every node that is not a message queue (a 0 would be a claim).
  std::optional<double> queue_lag_s{};
  // A worker pool's dead letters, per second (phase 3 spec §6.2; book ch. 10's dead-letter queue):
  // the jobs whose LAST attempt at an external sink failed this step, which a pool with `retries`
  // parks instead of trying again (behaviours::WorkerPoolBehaviour has the model). Not a drop: the
  // pool's `dropped_per_s` and its balance above do not include them (the job was drained; what
  // failed was its call), and nothing is lost from any node's books. 0 on a pool that retries
  // nothing (it has the counter, at 0); empty on every other node, which has no dead-letter queue.
  std::optional<double> dead_lettered_per_s{};
  // A message queue's jobs re-published to it this step by its consumer groups (a worker pool's
  // retries, behaviours::WorkerPoolBehaviour), per second: a part of `demand_per_s`, so that
  // demand − republished is what the producers published (report::Summary::dead_letter_rate's
  // denominator). 0 on a queue whose groups retry nothing; empty on every other node.
  std::optional<double> republished_per_s{};
  // An external sink's demand that came from worker pools with `retries`, per second, in the
  // sink's units like `demand_per_s` (demand × the share of its arrivals those calls were): the
  // calls whose failures are attempted again, so the drop rate leaves their share of the drops out
  // (report::Summary::drop_rate). Empty on every other node and on a sink no retrying pool calls.
  std::optional<double> retried_demand_per_s{};
};

// What a client sees for one op: the rate it offers, the part of it that comes back answered, and
// the response time an answered request gets. `answered` = offered × the delivered fraction the
// backward pass composes along the op's path (spec §17 entry 8,
// behaviours::NodeBehaviour::delivered): a request a limiter refused or a full queue dropped is not
// answered (one still waiting in a queue is: later, see Simulation::account_rejections), and
// reports weigh latency by `answered` so that those requests, which never saw the path's latency,
// do not dilute it. A client with a `timeout` (Simulation::give_up_after) also gives up on the
// delivered requests slower than it: those are `timed_out`, not answered, and `response` is the
// latency of the ones answered in time. So answered = offered − lost − timed_out, and timed_out is
// 0 for a client that waits for ever. `delivered_response` is the latency of every delivered
// request, answered or timed out, before the client gave up (the untruncated fit the timeout cut):
// the reports read a timed-out client's percentiles off it. Without a timeout the two are equal.
//
// A client with `retries` (behaviours::ClientBehaviour has the model) sends more than it offers:
// `attempts` is what it sent this step, `offered` (its curve) plus the retries due now, and it is
// `offered` exactly without retries or with nothing to retry. Then `answered` = attempts × the
// delivered fraction (× the share in time): an attempt answered is a request answered, once, since
// a request is not sent again after an answer. `timed_out` counts the LAST attempts that timed out
// only (Simulation::count_final_timeouts), requests given up on for good; every other failed
// attempt is sent again, not lost. Within one step `answered` may then exceed `offered` (a wave of
// retries answered as an outage ends); at a fixed point answered = offered − lost − timed_out
// still holds, `lost` being the requests whose last attempt was lost. `timed_out_attempts` counts
// EVERY attempt that timed out, the last ones and the retried ones: the client waited past its
// timeout on each, so the share of slow responses a percentile reads is
// timed_out_attempts / (answered + timed_out_attempts), the attempts delivered (report.cpp's
// percentile_seen_by_client). Counting the final ones alone would hide the timeouts a retry
// covered: 10.6 % of the attempts past a 200 ms timeout, retried twice, are 0.12 % of the
// requests, and a p99 SLO would pass on responses the client never got. Without retries the two
// counts are equal.
struct ResponseSample {
  std::size_t client{};
  std::string op{};
  Rate offered{};
  Rate attempts{};
  Rate answered{};
  Rate timed_out{};           // requests given up on for good: their last attempt timed out
  Rate timed_out_attempts{};  // every attempt that timed out, retried or not
  flow::LatencyEstimate response{};
  flow::LatencyEstimate delivered_response{};
};

// One bandwidth link's pipe for one step (spec §19 entry 3; Simulation::pass_through_pipe):
// NodeSample's fields of the same names, in MEGABYTES (per second for the rates), the pipe being a
// queue of megabytes like a byte-sized node. `demand_per_s` is what the sender put on the link,
// `drain_per_s` what the pipe passed on to the receiver, `dropped_per_s` what overflowed its 1000
// MB room; they balance as a node's do,
//   demand·dt = (drain + dropped)·dt + Δbacklog.
// `rho` and `zone` are the one server's; `latency` is what a hop over the pipe adds, its wait plus
// the request's answer time on the wire (a write's whole serialisation, size × 1 MB / bandwidth;
// any other op's first megabyte), mixed over the ops that arrived by their requests, over the ops
// it carries alike in a step when none arrived (the link's fixed one-way delay is not in it). A
// pipe refuses nothing by policy, so it has no `rejected_per_s`.
struct LinkSample {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};
  double rho{};
  flow::LatencyEstimate latency{};
  fluid::Zone zone{fluid::Zone::green};
};

// Everything one step produced: one NodeSample per node (indexed like Graph::nodes()), one
// ResponseSample per (client, op), clients in node order, ops sorted by name, and one LinkSample
// per link that has a bandwidth, in Graph::links() order (none without a bandwidth: such a link
// only delays its flow).
struct StepRecord {
  SimTime t{};
  std::vector<NodeSample> nodes{};
  std::vector<ResponseSample> responses{};
  std::vector<LinkSample> links{};
  // Only Simulation::estimate clears it: false means the settle passes hit their cap
  // before the numbers stopped changing, so the record is the last pass, not a steady
  // state. A loop that returns as much as it receives (gain >= 1) and that no capacity
  // stops within reach climbs for ever: it has no steady state. So do delivered fractions
  // that never settle (a step after a failed step, see estimate). Every step of `run` leaves it
  // true.
  bool converged{true};
};

}  // namespace simsys::engine
