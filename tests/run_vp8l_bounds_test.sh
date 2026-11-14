#!/usr/bin/env bash
#
# Builds and runs the VP8L boundary regression test against a given libwebp
# build (normal or ASan/UBSan). Self-contained: no network, no containers.
#
# Usage:
#   tests/run_vp8l_bounds_test.sh <build-dir> [repo-root] [golden-dir]
#
# Example:
#   tests/run_vp8l_bounds_test.sh ../builds/build-asan
#
# The golden directory holds known-good lossless WebP samples (regenerated with
# tests/golden_gen.c). If it does not exist the golden tests skip themselves.
#
set -euo pipefail

BUILD_DIR="${1:?usage: $0 <build-dir> [repo-root] [golden-dir]}"
REPO_ROOT="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
GOLDEN_DIR="${3:-${REPO_ROOT}/tests/golden}"
TEST_SRC="${REPO_ROOT}/tests/vp8l_bounds_test.c"
FIXTURE="${REPO_ROOT}/examples/test.webp"

# Locate the static decoder library produced by CMake (the exact filename/
# subdirectory varies across generators). Prefer the decoder-only library.
DECODER_LIB="$(find "${BUILD_DIR}" -name 'libwebpdecoder.a' | sed -n '1p')"
if [[ -z "${DECODER_LIB}" ]]; then
  DECODER_LIB="$(find "${BUILD_DIR}" -name 'libwebp.a' | sed -n '1p')"
fi
if [[ -z "${DECODER_LIB}" ]]; then
  echo "ERROR: no libwebpdecoder.a/libwebp.a found under ${BUILD_DIR}" >&2
  exit 2
fi

CC_BIN="${CC:-cc}"
OUT="$(mktemp -d)/vp8l_bounds_test"
SWEEP_OUT="$(mktemp -d)/vp8l_truncation_sweep"
SWEEP_SRC="${REPO_ROOT}/tests/vp8l_truncation_sweep.c"

# If the library was instrumented, compile/link the test with the matching
# sanitizers so the runtime symbols resolve and checks apply to the test too.
# Dump symbols to a temp file first: piping directly into grep under
# `set -o pipefail` can abort the script via SIGPIPE.
NM_OUTPUT="$(mktemp)"
nm -g "${DECODER_LIB}" > "${NM_OUTPUT}" 2>/dev/null || true
SANITIZER_FLAGS=""
if grep -q '__asan_init' "${NM_OUTPUT}"; then
  SANITIZER_FLAGS="address"
fi
if grep -q '__ubsan' "${NM_OUTPUT}"; then
  SANITIZER_FLAGS="${SANITIZER_FLAGS:+${SANITIZER_FLAGS},}undefined"
fi
rm -f "${NM_OUTPUT}"

echo "CC=${CC_BIN}"
echo "library=${DECODER_LIB}"
echo "sanitizer_flags=${SANITIZER_FLAGS:-none}"

# shellcheck disable=SC2086
"${CC_BIN}" -g -O1 -fno-omit-frame-pointer \
  ${SANITIZER_FLAGS:+-fsanitize="${SANITIZER_FLAGS}"} \
  -I"${REPO_ROOT}" "${TEST_SRC}" -L"$(dirname "${DECODER_LIB}")" \
  -l"$(basename "${DECODER_LIB}" | sed -e 's/^lib//' -e 's/\.a$//')" \
  -lm -o "${OUT}"

# Also build the byte-by-byte truncation sweep (same sanitizer settings).
# shellcheck disable=SC2086
"${CC_BIN}" -g -O1 -fno-omit-frame-pointer \
  ${SANITIZER_FLAGS:+-fsanitize="${SANITIZER_FLAGS}"} \
  -I"${REPO_ROOT}" "${SWEEP_SRC}" -L"$(dirname "${DECODER_LIB}")" \
  -l"$(basename "${DECODER_LIB}" | sed -e 's/^lib//' -e 's/\.a$//')" \
  -lm -o "${SWEEP_OUT}"

# ASan/UBSan, when the library was built with them, are inherited at runtime
# via the compiler driver; abort on first error so failures are unambiguous.
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  "${OUT}" "${GOLDEN_DIR}" "${FIXTURE}"

# Every truncated prefix of each golden file must fail cleanly (no crash).
SWEEP_INPUTS=()
for g in "${GOLDEN_DIR}"/golden_*.webp; do
  [[ -e "${g}" ]] && SWEEP_INPUTS+=("${g}")
done
[[ -e "${FIXTURE}" ]] && SWEEP_INPUTS+=("${FIXTURE}")
if (( ${#SWEEP_INPUTS[@]} > 0 )); then
  ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "${SWEEP_OUT}" "${SWEEP_INPUTS[@]}"
fi

echo "PASS: VP8L bounds test ran cleanly against ${BUILD_DIR}"
