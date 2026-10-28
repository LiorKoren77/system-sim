// Module `simsys.engine`: the fluid simulation loop. Stub for the TDD red phase: the
// tests in tests/unit/test_engine.cpp and tests/theory/test_theory.cpp define what this
// module must export (DelayLine, Simulation); the implementation follows.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3).

export module simsys.engine;

import simsys.core;
import simsys.behaviours;
import simsys.flow;
import simsys.fluid;
import simsys.graph;
import simsys.model;
