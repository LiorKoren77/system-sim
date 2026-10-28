// Module `simsys.behaviours`: what each node type does with flow (client: offer load
// along its routes; service: forward step demand, serve the rest) and how it composes
// response latency from its own sojourn plus link legs and downstream responses.
//
// STUB (TDD red): the tests in tests/unit/test_behaviours.cpp define the API; the
// implementation follows in the next commit.
export module simsys.behaviours;

import simsys.core;
import simsys.flow;
import simsys.graph;
import simsys.model;
