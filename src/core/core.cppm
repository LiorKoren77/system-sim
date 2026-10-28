// Module `simsys.core`: the vocabulary types every other layer uses.
//
// C++20 named modules. This file is a *module interface unit*. Everything after
// `export module simsys.core;` is the module's purview; only declarations marked
// `export` are visible to importers. Compared with headers: no include guards, no
// macro leakage, one compilation of the interface instead of one per includer.
// The `module;` line opens the *global module fragment*, the only place a module
// unit may #include traditional headers (the standard library is still header-based
// in GCC 14).
module;
#include <string_view>

export module simsys.core;

export namespace simsys {

// C++20 `constexpr` std::string_view construction from a literal is a compile-time
// constant; `inline constexpr` gives one definition across all importers.
inline constexpr std::string_view version = "0.0.1";

}  // namespace simsys
