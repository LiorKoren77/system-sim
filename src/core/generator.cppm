// Module `simsys.generator`: Generator<T>, a lazy range written as a coroutine, the project's own
// stand-in for C++23 std::generator<T>.
//
// Why our own: std::generator is in GCC 14's standard library (libstdc++) but not in LLVM's
// (libc++ 19), the library Clang uses with -stdlib=libc++ and Emscripten always uses. One type of
// our own builds with every toolchain, without conditional compilation (CLAUDE.md: no macros).
// What it gives up: std::generator's nested generators (`co_yield std::ranges::elements_of(...)`)
// and its reference/value type parameters; samples() yields plain values and needs neither.
//
// How a coroutine generator works (docs/study/generator.md has the longer note). A function whose
// body contains `co_yield` is a C++20 coroutine. The compiler turns it into a state machine whose
// parameters and locals live in a heap-allocated frame, and asks the return type for a nested
// `promise_type` that says how the coroutine behaves:
//   get_return_object   builds the Generator the caller receives, holding a handle to the frame;
//   initial_suspend     suspend_always: the body does not run until the first begin() (lazy);
//   yield_value(v)      called on `co_yield v`: store v and suspend, handing control back;
//   final_suspend       suspend_always: keep the frame alive after the body ends, so the iterator
//                       can ask done() and the Generator's destructor frees it exactly once;
//   unhandled_exception an exception thrown by the body is stored and rethrown to the consumer,
//                       on the begin() or ++ that resumed it.
// The iterator resumes the frame on ++ and reads the stored value on *. Pattern: Iterator — the
// coroutine is the producer, the iterator the consumer's view of it.
//
// C++20 named module, interface unit; it imports no other module, so none of the GCC 14.2 module
// defects of CLAUDE.md §3 (which concern a unit that imports another) applies to it.
module;
#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <optional>
#include <utility>

export module simsys.generator;

export namespace simsys {

// A single-pass input range of the T values a coroutine `co_yield`s. Move-only: it owns the
// coroutine frame and destroys it. Its iterator is input-only (operator* but no operator->, as
// std::generator's).
template <class T>
class Generator {
 public:
  struct promise_type {
    std::optional<T> current{};  // the value of the last co_yield
    std::exception_ptr error{};  // what the body threw, rethrown to the consumer

    Generator get_return_object() {
      return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    // C++20 std::suspend_always: an awaitable that always suspends; the two standard ones,
    // suspend_always and suspend_never, are all a simple generator needs.
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    std::suspend_always yield_value(T value) {
      current = std::move(value);
      return {};
    }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { error = std::current_exception(); }
  };

  // The consumer's side: ++ resumes the coroutine to its next co_yield, * reads what it yielded.
  class iterator {
   public:
    using value_type = T;
    using difference_type = std::ptrdiff_t;

    iterator() = default;
    explicit iterator(std::coroutine_handle<promise_type> frame) : frame_{frame} {}

    const T& operator*() const { return *frame_.promise().current; }
    iterator& operator++() {
      resume(frame_);
      return *this;
    }
    void operator++(int) { ++*this; }
    // C++20 std::default_sentinel_t: an empty end marker; the range ends when the body has. A
    // member, not the usual hidden friend: GCC 14.2 gives a friend defined in a class template of a
    // module a strong definition in every unit that instantiates it ("multiple definition" at link
    // time, the fifth defect's family, CLAUDE.md §3), while a member of a class template is emitted
    // once. C++20 rewritten comparisons still let `std::default_sentinel == it` and `!=` work.
    bool operator==(std::default_sentinel_t) const { return !frame_ || frame_.done(); }

   private:
    std::coroutine_handle<promise_type> frame_{};
  };

  Generator(const Generator&) = delete;
  Generator& operator=(const Generator&) = delete;
  Generator(Generator&& other) noexcept : frame_{std::exchange(other.frame_, {})} {}
  Generator& operator=(Generator&& other) noexcept {
    if (this != &other) {
      destroy();
      frame_ = std::exchange(other.frame_, {});
    }
    return *this;
  }
  ~Generator() { destroy(); }

  // Runs the body to its first co_yield (or to its end, or to what it throws): the first begin()
  // is where a check at the top of the body fails. Single pass: call it once.
  iterator begin() {
    resume(frame_);
    return iterator{frame_};
  }
  std::default_sentinel_t end() const noexcept { return {}; }

 private:
  explicit Generator(std::coroutine_handle<promise_type> frame) : frame_{frame} {}

  // Resumes the body to its next co_yield; an exception it threw there reaches the consumer here.
  static void resume(std::coroutine_handle<promise_type> frame) {
    frame.resume();
    if (auto error = std::exchange(frame.promise().error, {})) std::rethrow_exception(error);
  }
  void destroy() noexcept {
    if (frame_) frame_.destroy();
    frame_ = {};
  }

  std::coroutine_handle<promise_type> frame_{};
};

}  // namespace simsys
