// Module `simsys.model`, implementation unit: no model logic lives here. It exists only to
// work around a GCC 14.2 named-modules defect that breaks the release (-O2/-O3) link.
//
// The defect: when a module unit has already instantiated a compiler-generated special
// member of a standard class template over a model type (the implicit move constructor of
// std::optional<Workload>, the defaulted destructor of the NodeSpec::ops map), a unit
// that imports the module calls that member out of line but never emits it, trusting the
// module's object files to define it. Those define it only while some call to it there
// stays out of line: at -O0 one does; at -O2/-O3 every call is inlined, no definition is
// left anywhere, and the link fails with "undefined reference to
// std::optional<simsys::model::Workload@simsys.model>::optional(...&&)". User-written
// members (vector's copy constructor, map::find) are not affected; neither is Debug.
//
// The workaround: CMakeLists.txt compiles this one file with -fno-inline, so every call
// below stays a real call and GCC emits each special member as a weak (COMDAT)
// definition. The linker pulls this object out of libsimsys.a for any importer that needs
// one. Seeing a new "undefined reference" to a special member of a std template over a
// model type in the release build? Add one more `template void emit_special_members`
// line below. Verified 2026-10-28 with the gcc14-release preset.
module;
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.model;` (no `export`) makes this file
// part of the named module. It sees every declaration of model.cppm, exports nothing, and
// is compiled to an ordinary object file in the library, like a .cpp behind a header.
module simsys.model;

namespace simsys::model::detail {

// Calls each of T's five special members once, so the object file defines all of them.
// `dst` is raw storage for one T; `src` is a live T that is copied and moved from.
template <typename T>
void emit_special_members(T* dst, T& src) {
  // C++20 std::construct_at: placement-new as a (constexpr) library function, replacing
  // `::new (static_cast<void*>(dst)) T(src)`. Used here because the project bans raw
  // `new`, and because it names exactly the constructor call we want emitted.
  std::construct_at(dst, src);             // copy constructor
  *dst = src;                              // copy assignment
  *dst = std::move(src);                   // move assignment
  std::destroy_at(dst);                    // destructor
  std::construct_at(dst, std::move(src));  // move constructor
  std::destroy_at(dst);
}

// Explicit instantiation definitions: each line compiles emit_special_members for one
// type into this object file, and with it (thanks to -fno-inline) that type's special
// members. Instantiating the std templates themselves (`template class std::optional<
// Workload>;`) would not work: an explicit instantiation skips implicitly declared and
// defaulted members, which are exactly the ones GCC fails to emit.
template void emit_special_members(std::optional<Workload>*, std::optional<Workload>&);
template void emit_special_members(std::map<std::string, std::vector<Step>>*,
                                   std::map<std::string, std::vector<Step>>&);
// NodeSpec::cost (phase 2a): its move constructor went missing from the release link.
template void emit_special_members(std::map<std::string, double>*, std::map<std::string, double>&);
// NodeSpec::backend (phase 2a rate limiter): the move constructor of std::optional<std::string>
// went missing from the release link of every importer that copies a NodeSpec.
template void emit_special_members(std::optional<std::string>*, std::optional<std::string>&);
// NodeSpec::write_ops (phase 2b, absent = the type's default): the move constructor of
// std::optional<std::vector<std::string>> went missing from the release link of unit_tests.
template void emit_special_members(std::optional<std::vector<std::string>>*,
                                   std::optional<std::vector<std::string>>&);

}  // namespace simsys::model::detail
