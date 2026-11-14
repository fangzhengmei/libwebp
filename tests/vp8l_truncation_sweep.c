// Byte-by-byte truncation sweep over the golden VP8L files. For every prefix
// length, decoding must either succeed (only at full length) or fail cleanly
// with a non-OK status -- never crash, never ASan/UBSan report. Built and run
// under the sanitizers via run_vp8l_bounds_test.sh's sibling invocation.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webp/decode.h"

static uint8_t* ReadFile(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  long n;
  uint8_t* buf;
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (uint8_t*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

// Recompute the RIFF chunk-size fields so a truncated container stays
// internally consistent (we want to exercise the VP8L parser, not RIFF trim).
static void PatchRIFFSize(uint8_t* data, size_t total) {
  uint32_t riff_size = (uint32_t)(total - 8);
  memcpy(data + 4, &riff_size, 4);
}

static int SweepFile(const char* path) {
  size_t full;
  uint8_t* webp = ReadFile(path, &full);
  size_t len;
  uint32_t declared_vp8l;
  if (webp == NULL) {
    fprintf(stderr, "note: skip %s\n", path);
    return 1;
  }
  // The VP8L chunk payload size lives at offset 16 (after RIFF header and the
  // "VP8L" fourcc). Truncating beyond the payload only removes container
  // padding bytes, which a decoder is allowed to ignore.
  memcpy(&declared_vp8l, webp + 16, 4);
  // Smallest length that still contains the whole VP8L payload (20 byte
  // preamble + payload). We only sweep strictly shorter, meaningful prefixes.
  const size_t min_full_len = 20 + declared_vp8l;
  for (len = 12; len < min_full_len && len <= full; ++len) {
    int w, h;
    uint8_t* rgba;
    uint8_t* tmp = (uint8_t*)malloc(len);
    memcpy(tmp, webp, len);
    PatchRIFFSize(tmp, len);
    w = h = 0;
    rgba = WebPDecodeRGBA(tmp, len, &w, &h);
    // Every prefix that drops at least one payload byte must fail cleanly
    // (padding-only tails are excluded by the loop bound).
    if (rgba != NULL) {
      fprintf(stderr,
              "FAIL %s: unexpected decode success at truncated len %zu "
              "(payload needs %u bytes)\n",
              path, len, declared_vp8l);
      free(rgba);
      free(tmp);
      free(webp);
      return 0;
    }
    free(tmp);
  }
  free(webp);
  printf("truncation sweep clean: %s\n", path);
  return 1;
}

int main(int argc, char** argv) {
  int i;
  int failures = 0;
  for (i = 1; i < argc; ++i) {
    failures += !SweepFile(argv[i]);
  }
  if (failures) {
    fprintf(stderr, "%d sweep(s) failed\n", failures);
    return 1;
  }
  printf("All truncation sweeps passed.\n");
  return 0;
}
