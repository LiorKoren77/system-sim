// Module `simsys.caching`: how much of the read traffic a cache absorbs (spec §5).
//
// Will model: the cache-hit-ratio estimate behind System Design Interview ch. 1 and ch. 2
// ("a cache holding the hottest 20 % of keys serves ~80 % of reads"), computed from a
// Zipf popularity law with the Che approximation for an LRU cache instead of by hand.
// This file is the TDD-red stub: the tests in tests/unit/test_caching.cpp define the API.
export module simsys.caching;
