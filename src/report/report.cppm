// Module `simsys.report`: turns a run into something a person or a GUI can read.
//
// Models: nothing from the book; this is the measurement layer. It summarises the step
// records of a run (time-averaged moments, percentiles fitted to them and labelled
// "estimated", SLO verdicts as in ch. 12-style availability/latency targets), renders a
// text table, a JSON report and a CSV series, and explains a resolved scenario.
//
// STUB (TDD red): the tests in tests/unit/test_report.cpp define the API; the
// implementer fills this in.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Heavy headers go in report.cpp.

export module simsys.report;

import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.fluid;
import simsys.flow;
