# VP8L bitstream boundary regression tests

These tests harden the lossless (VP8L) decoder against malformed bitstreams:

- fixed-width transform and color-cache parameters are length-checked **before**
  they are consumed, so a truncated stream fails at the parameter boundary
  instead of carrying unverified values into deeper decode stages;
- out-of-range color-cache bit declarations (anything outside
  `1..MAX_CACHE_BITS`) are rejected;
- color-cache indices are validated at the point of use so they can never index
  outside the declared cache table.

## Files

- `vp8l_bounds_test.c` — hand-crafts malformed/edge-case VP8L bitstreams with a
  small LSB-first bit writer and drives them through the public `webp/decode.h`
  API. Covers: no-transform control case, truncated transform type/parameter and
  palette size, truncated cache-present/cache-bits fields, out-of-range cache
  bits (12 and 15), maximum legal cache bits (11), plus decoding of real
  lossless golden files.
- `vp8l_truncation_sweep.c` — for every byte-prefix of each golden file, asserts
  decoding either succeeds (only on the full payload) or fails cleanly with no
  crash/sanitizer report.
- `golden_gen.c` — regenerates the known-good lossless samples in `golden/`
  using the public lossless encoder API.
- `golden/` — checked-in golden `.webp` samples used by both tests.
- `run_vp8l_bounds_test.sh` — builds and runs both test binaries against an
  existing CMake build directory, auto-detecting ASan/UBSan instrumentation.

## Running

The tests need a built `libwebpdecoder.a`. Build the project first, then:

```sh
# normal build
cmake -B build-normal -DWEBP_BUILD_<tools>=OFF
cmake --build build-normal
tests/run_vp8l_bounds_test.sh build-normal

# ASan/UBSan build (recommended)
cmake -B build-asan \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DWEBP_BUILD_<tools>=OFF
cmake --build build-asan
tests/run_vp8l_bounds_test.sh build-asan
```

Alternatively, integrate with CTest (default OFF, does not affect normal builds):

```sh
cmake -B build -DWEBP_BUILD_VP8L_BOUNDS_TEST=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

All malformed inputs must return `VP8_STATUS_BITSTREAM_ERROR`, and every golden
file must decode to byte-identical RGBA pixels. Under ASan/UBSan the runs must
be completely clean (no errors, no leaks).

## Regenerating golden samples

```sh
cc -I<repo-root> golden_gen.c -L<build> -lwebp -lsharpyuv -lm -o golden_gen
./golden_gen golden
```
