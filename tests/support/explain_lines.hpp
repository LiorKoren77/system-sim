// Lines of `simsys explain`'s text, for the tests that pin them. A plain header rather than a
// module: it is test scaffolding over std::string alone, included before the test's imports (see
// the note on import order in each test file).
#pragma once

#include <string>

namespace simsys::test_support {

// The line of explain's text that starts, after its two-space indent, with `id` and a space, at or
// after the first `block` heading ("nodes:", "checks ("); empty when there is none.
inline std::string line_in_block(const std::string& text, const std::string& block,
                                 const std::string& id) {
  const auto at = text.find("\n  " + id + " ", text.find(block));
  if (at == std::string::npos) return {};
  return text.substr(at + 1, text.find('\n', at + 1) - at - 1);
}

// Node `id`'s line of the nodes block: its resolved numbers and where they came from.
inline std::string node_line(const std::string& text, const std::string& id) {
  return line_in_block(text, "nodes:", id);
}

// Node `id`'s (or link `a->b`'s) line of the checks block: its peak offered load and verdict.
inline std::string check_line(const std::string& text, const std::string& id) {
  return line_in_block(text, "checks (", id);
}

}  // namespace simsys::test_support
