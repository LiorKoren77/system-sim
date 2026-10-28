// Module `simsys.graph`: the scenario resolved into what the engine steps over — nodes
// with capacities and provenance, links with one-way latency and delay-line lengths,
// demand edges, a topological order, client routes, and feedback edges for cycles.
// Models: the arrows of a System Design Interview (Alex Xu, vol. 1) diagram.
// STUB (TDD red): the declarations arrive with the implementation.
module;

export module simsys.graph;

import simsys.core;
import simsys.defaults;
import simsys.fluid;
import simsys.model;
