// Module `simsys.flow`: the per-op flow map (how much traffic of each operation type
// crosses a node, in requests per second) and the composition rules for latency moments
// (means and variances add along a path, the slowest branch dominates in parallel).
// Stub for the TDD red step: the implementation follows the tests in tests/unit/test_flow.cpp.
export module simsys.flow;

import simsys.core;
