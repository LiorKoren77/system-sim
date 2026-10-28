// Module `simsys.defaults`: the back-of-the-envelope table (System Design Interview,
// Alex Xu, vol. 1, ch. 2) and the per-node-type defaults resolved from it, each tagged
// with where it came from so `simsys explain` can show provenance.
//
// STUB (TDD red): the test-writer registers the module so the tests can import it;
// the implementation follows in the next commit.
export module simsys.defaults;

import simsys.core;
import simsys.fluid;
import simsys.model;
