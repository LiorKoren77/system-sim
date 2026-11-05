# Sanitizers: ASan, UBSan and TSan

Not a language feature: a compiler feature (GCC and Clang) that every C++ programmer should know,
because it turns silent undefined behaviour into a loud, located failure.

## Used in

| Where | What |
|---|---|
| `CMakePresets.json` | the `gcc14-asan` and `gcc14-tsan` configure, build and test presets |
| `.github/workflows/ci.yml` | the `build-test-asan` and `build-test-tsan` jobs |

```bash
cmake --preset gcc14-asan && cmake --build --preset gcc14-asan && ctest --preset gcc14-asan
cmake --preset gcc14-tsan && cmake --build --preset gcc14-tsan && ctest --preset gcc14-tsan
```

## What

A sanitizer is instrumentation the compiler adds at build time (`-fsanitize=...`): extra code
around memory accesses, arithmetic, casts or synchronisation, and a runtime library linked in.
When the program does something the standard leaves undefined, the runtime stops it there and
prints what happened, where, and (for memory) where the memory was allocated and freed.

Unlike a static analyser, a sanitizer has almost no false positives: it reports what a run
actually did. The price is that it sees only the code a run reaches, which is why the test suite
is what runs under it.

| Sanitizer | Flag | Finds | Cost |
|---|---|---|---|
| AddressSanitizer (ASan) | `-fsanitize=address` | out-of-bounds reads and writes (heap, stack, globals), use after free, use after return or scope (a dangling reference or `std::string_view`), double free, leaks (LeakSanitizer) | ~2x time, 2-3x memory |
| UndefinedBehaviorSanitizer (UBSan) | `-fsanitize=undefined` | signed overflow, a shift by a negative or too large count, a float-to-int conversion out of range (`static_cast<int>(inf)`), a null or misaligned pointer, a bad `bool` or enum value, reaching `std::unreachable()`, a non-void function that falls off its end | ~1.2-1.5x |
| ThreadSanitizer (TSan) | `-fsanitize=thread` | data races (two threads touch the same memory, one writes, nothing orders them), lock-order inversions | 5-15x time, 5-10x memory |

ASan and UBSan combine in one binary; TSan cannot share one with ASan (both take over the
address-space layout and the allocator), hence two presets and two CI jobs.

## The flags of each preset

`gcc14-asan`:

- `-fsanitize=address,undefined`: the two sanitizers.
- `-fno-sanitize-recover=all`: by default UBSan prints and carries on; this makes its first
  report end the process with a non-zero status, so the test (and CI) fails.
- `-fno-omit-frame-pointer`: keeps the frame pointer so the reports' stack traces are complete.
- `-D_GLIBCXX_ASSERTIONS`: not a sanitizer but its cousin: libstdc++ checks its preconditions
  (`operator[]` in range, `front()` on a non-empty container, ...) and aborts when one fails.
  ASan does not see an `operator[]` one past the end of a `std::vector` whose capacity is larger
  than its size; this does.

`gcc14-tsan`:

- `-fsanitize=thread`, `-fno-omit-frame-pointer`.
- `-Wno-error=tsan`: GCC warns (`-Wtsan`) that TSan does not model `std::atomic_thread_fence`,
  which libstdc++'s headers use; under the project's `-Werror` that warning would stop the build.

Both inherit `gcc14-debug` (`-O0 -g`): readable reports, and clear of the `-O2` defects of
CLAUDE.md §3.

## Why the flags go in `CMAKE_CXX_FLAGS`, for every target

A named module's BMI (the `.gcm` file GCC writes for `simsys.engine`, say) records the options it
was compiled with, and an importer compiled with different ones may refuse it or miscompile
against it. With headers each `.cpp` re-reads the header under its own flags; with modules the
library, the CLI and the tests must agree. `CMAKE_CXX_FLAGS` reaches every target (third-party
code too, which ASan prefers: an uninstrumented library is a blind spot), and CMake passes it to
the link step as well, which links the runtime library.

## Why TSan runs only some tests

The preset's test filter (`workers|jobs|replication`) selects the tests that start threads:
`run_in_parallel` behind `simsys sweep` and `run --replications` ([threads.md](threads.md)), from
the CLI and from the unit tests ("the summaries do not depend on how many workers ran the
replications"). Every other test is single-threaded, where TSan has nothing to find and costs
5-15x. A new test that starts threads should have one of those words in its name.

## Pitfalls

- **TSan on recent Linux kernels**: GCC 14's TSan runtime expects at most 28 bits of `mmap`
  randomisation; Ubuntu 24.04 uses 32, and every instrumented binary dies at start with
  "FATAL: ThreadSanitizer: unexpected memory mapping". Fix: `sudo sysctl vm.mmap_rnd_bits=28`
  (the CI job does it; on WSL too).
- **A sanitizer sees only what runs.** A clean ASan run says the tests do nothing undefined, not
  that the program cannot.
- **Do not ship it.** The instrumented binary is slower and its runtime changes behaviour (ASan
  delays reusing freed memory, on purpose). The presets build into their own `build/` directory.

## The pre-sanitizer way

Valgrind (memcheck, helgrind): no rebuild, but 20-50x slower and blind to stack and global
overflows; or a crash, much later, far from the bug.

## Further reading

- GCC manual, "Program Instrumentation Options" (`-fsanitize=`).
- The Google sanitizers wiki: AddressSanitizer, ThreadSanitizer, their algorithms and flags
  (`ASAN_OPTIONS`, `TSAN_OPTIONS`).
- Clang's UndefinedBehaviorSanitizer page: the list of checks `-fsanitize=undefined` enables.
