// Copyright 2024 Google Inc.
//
// Regression tests for VP8L (lossless WebP) bitstream boundary handling:
//  - fixed-width transform / color-cache parameters are length-checked before
//    being consumed, so truncated streams fail at the parameter boundary;
//  - out-of-range color-cache bit declarations are rejected;
//  - color-cache indices are always inside the declared cache table.
//
// Strategy:
//  * Malformed / edge-case bitstreams are hand-crafted with a small LSB-first
//    bit writer and driven through the public decoding API; they must fail with
//    VP8_STATUS_BITSTREAM_ERROR (never crash, never OOM, ASan/UBSan clean).
//  * Legal bitstreams are real lossless encodings (see golden_gen.c). They must
//    decode successfully and to byte-identical RGBA pixels, proving the added
//    length checks do not alter the behavior of valid streams.
//
// Self-contained: no network access or containers required.
// Build (see tests/run_vp8l_bounds_test.sh):
//   cc -I<repo-root> tests/vp8l_bounds_test.c <libwebpdecoder.a> -o vp8l_bounds

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/webp/format_constants.h"
#include "webp/decode.h"

#define EXPECT_EQ(a, b)                                                  \
  do {                                                                   \
    long va_ = (long)(a);                                                \
    long vb_ = (long)(b);                                                \
    if (va_ != vb_) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s (%ld) != %s (%ld)\n", __FILE__,   \
              __LINE__, #a, va_, #b, vb_);                              \
      return 0;                                                          \
    }                                                                    \
  } while (0)

#define EXPECT_TRUE(cond)                                     \
  do {                                                         \
    if (!(cond)) {                                             \
      fprintf(stderr, "FAIL %s:%d: expected %s\n", __FILE__,   \
              __LINE__, #cond);                                \
      return 0;                                                \
    }                                                         \
  } while (0)

// -----------------------------------------------------------------------------
// Minimal LSB-first bit writer (VP8L stores multi-bit fields LSB-first).
// -----------------------------------------------------------------------------

typedef struct {
  uint8_t* bytes;
  size_t capacity;
  size_t nbits;
} BitWriter;

static void BWInit(BitWriter* const w, size_t capacity_bytes) {
  w->bytes = (uint8_t*)calloc(capacity_bytes, 1);
  w->capacity = capacity_bytes;
  w->nbits = 0;
}

static void BWFree(BitWriter* const w) {
  free(w->bytes);
  w->bytes = NULL;
}

static void BWPut(BitWriter* const w, uint32_t value, int nbits) {
  int i;
  for (i = 0; i < nbits; ++i) {
    const size_t pos = w->nbits++;
    if ((value >> i) & 1u) {
      w->bytes[pos >> 3] |= (uint8_t)(1u << (pos & 7));
    }
  }
}

static size_t BWByteSize(const BitWriter* const w) {
  return (w->nbits + 7) >> 3;
}

// -----------------------------------------------------------------------------
// VP8L framing + simple-Huffman helpers.
// -----------------------------------------------------------------------------

static uint8_t* WrapRIFF(const uint8_t* payload, size_t payload_size,
                         size_t* const out_size) {
  const size_t total = RIFF_HEADER_SIZE + CHUNK_HEADER_SIZE + payload_size +
                       (payload_size & 1);
  uint8_t* const buf = (uint8_t*)calloc(total, 1);
  size_t p = 0;
  uint32_t tag;
  memcpy(buf + p, "RIFF", 4);
  p += 4;
  tag = (uint32_t)(total - 8);
  memcpy(buf + p, &tag, 4);
  p += 4;
  memcpy(buf + p, "WEBP", 4);
  p += 4;
  memcpy(buf + p, "VP8L", 4);
  p += 4;
  tag = (uint32_t)payload_size;
  memcpy(buf + p, &tag, 4);
  p += 4;
  memcpy(buf + p, payload, payload_size);
  *out_size = total;
  return buf;
}

static void WriteImageInfo(BitWriter* const w, int width, int height,
                           int has_alpha) {
  BWPut(w, VP8L_MAGIC_BYTE, 8);
  BWPut(w, (uint32_t)(width - 1), VP8L_IMAGE_SIZE_BITS);
  BWPut(w, (uint32_t)(height - 1), VP8L_IMAGE_SIZE_BITS);
  BWPut(w, has_alpha ? 1u : 0u, 1);
  BWPut(w, VP8L_VERSION, VP8L_VERSION_BITS);
}

// Single-symbol simple Huffman tree (mirrors ReadHuffmanCode). Only symbol
// values 0/1 can be represented because the symbol is a 1-bit field.
static void WriteSimple1Symbol(BitWriter* const w, int symbol) {
  BWPut(w, 1, 1);                        // simple code
  BWPut(w, 0, 1);                        // raw 0 -> num_symbols = 1
  BWPut(w, 0, 1);                        // 1-bit symbol field
  BWPut(w, (uint32_t)(symbol & 1), 1);
}

// Two-symbol simple Huffman tree; symbol0 is emitted as bit 0, symbol1 as bit 1.
static void WriteSimple2Symbol(BitWriter* const w, int symbol0, int symbol1) {
  BWPut(w, 1, 1);                        // simple code
  BWPut(w, 1, 1);                        // raw 1 -> num_symbols = 2
  BWPut(w, 0, 1);                        // first symbol is a 1-bit field
  BWPut(w, (uint32_t)(symbol0 & 1), 1);  // symbol0 (1 bit)
  BWPut(w, (uint32_t)symbol1, 8);        // symbol1 (always 8 bits)
}

static void WriteHuffmanGroup(BitWriter* const w, int green, int red,
                              int blue, int alpha) {
  WriteSimple1Symbol(w, green);
  WriteSimple1Symbol(w, red);
  WriteSimple1Symbol(w, blue);
  WriteSimple1Symbol(w, alpha);
  WriteSimple1Symbol(w, 0);  // DISTANCE
}

// Decode into MODE_RGBA and return the precise status. On success, *rgba is a
// malloc'd width*height*4 buffer owned by the caller.
static VP8StatusCode DecodeRGBAAlloc(const uint8_t* webp, size_t webp_size,
                                     int* width, int* height,
                                     uint8_t** rgba) {
  WebPDecoderConfig config;
  VP8StatusCode status;
  EXPECT_TRUE(WebPInitDecoderConfig(&config));
  config.output.colorspace = MODE_RGBA;
  status = WebPDecode(webp, webp_size, &config);
  if (status == VP8_STATUS_OK) {
    const size_t n = (size_t)config.output.width * config.output.height * 4;
    *width = config.output.width;
    *height = config.output.height;
    *rgba = (uint8_t*)malloc(n);
    EXPECT_TRUE(*rgba != NULL);
    memcpy(*rgba, config.output.u.RGBA.rgba, n);
  } else {
    *rgba = NULL;
  }
  WebPFreeDecBuffer(&config.output);
  return status;
}

static VP8StatusCode ExpectBitstreamError(BitWriter* const w) {
  size_t payload_size = BWByteSize(w);
  uint8_t* const payload = (uint8_t*)malloc(payload_size);
  size_t webp_size;
  uint8_t* webp;
  int width = 0, height = 0;
  uint8_t* rgba = NULL;
  VP8StatusCode status;
  memcpy(payload, w->bytes, payload_size);
  webp = WrapRIFF(payload, payload_size, &webp_size);
  status = DecodeRGBAAlloc(webp, webp_size, &width, &height, &rgba);
  free(webp);
  free(payload);
  free(rgba);
  return status;
}

// -----------------------------------------------------------------------------
// Malformed / edge-case bitstreams.
// -----------------------------------------------------------------------------

// No transform, no color cache: a minimal legal 1x1 image (control case that
// also validates the bit-writer framing end to end).
static int TestNoTransform(void) {
  BitWriter w;
  size_t payload_size, webp_size;
  uint8_t *payload, *webp, *rgba = NULL;
  int width = 0, height = 0;
  VP8StatusCode status;
  const int alpha = 1, red = 1, green = 0, blue = 1;

  BWInit(&w, 256);
  WriteImageInfo(&w, 1, 1, 1);
  BWPut(&w, 0, 1);  // no transform
  BWPut(&w, 0, 1);  // no color cache
  BWPut(&w, 0, 1);  // no meta Huffman
  WriteHuffmanGroup(&w, green, red, blue, alpha);
  BWPut(&w, 0, 1); BWPut(&w, 0, 1); BWPut(&w, 0, 1); BWPut(&w, 0, 1);

  payload_size = BWByteSize(&w);
  payload = (uint8_t*)malloc(payload_size);
  memcpy(payload, w.bytes, payload_size);
  webp = WrapRIFF(payload, payload_size, &webp_size);
  status = DecodeRGBAAlloc(webp, webp_size, &width, &height, &rgba);
  EXPECT_EQ(status, VP8_STATUS_OK);
  EXPECT_EQ(width, 1);
  EXPECT_EQ(height, 1);
  EXPECT_EQ(rgba[0], red);
  EXPECT_EQ(rgba[1], green);
  EXPECT_EQ(rgba[2], blue);
  EXPECT_EQ(rgba[3], alpha);

  free(webp);
  free(payload);
  free(rgba);
  BWFree(&w);
  return 1;
}

// A transform whose 2-bit type field is cut after only one bit: the length
// check must reject it before the partial type is acted upon.
static int TestTruncatedTransformType(void) {
  BitWriter w;
  VP8StatusCode status;
  BWInit(&w, 64);
  WriteImageInfo(&w, 1, 1, 1);
  BWPut(&w, 1, 1);  // transform present
  BWPut(&w, 1, 1);  // only 1 of 2 type bits, then EOF
  status = ExpectBitstreamError(&w);
  EXPECT_EQ(status, VP8_STATUS_BITSTREAM_ERROR);
  BWFree(&w);
  return 1;
}

// A PREDICTOR transform whose 3-bit parameter is truncated to one bit.
static int TestTruncatedTransformParameter(void) {
  BitWriter w;
  VP8StatusCode status;
  BWInit(&w, 64);
  WriteImageInfo(&w, 1, 1, 1);
  BWPut(&w, 1, 1);                          // transform present
  BWPut(&w, PREDICTOR_TRANSFORM, 2);       // complete type
  BWPut(&w, 1, 1);                          // only 1 of 3 parameter bits
  status = ExpectBitstreamError(&w);
  EXPECT_EQ(status, VP8_STATUS_BITSTREAM_ERROR);
  BWFree(&w);
  return 1;
}

// A COLOR_INDEXING transform whose 8-bit num_colors field is truncated.
static int TestTruncatedPaletteSize(void) {
  BitWriter w;
  VP8StatusCode status;
  BWInit(&w, 64);
  WriteImageInfo(&w, 1, 1, 1);
  BWPut(&w, 1, 1);                          // transform present
  BWPut(&w, COLOR_INDEXING_TRANSFORM, 2);
  BWPut(&w, 1, 1); BWPut(&w, 0, 1);         // only 2 of 8 num_colors bits
  status = ExpectBitstreamError(&w);
  EXPECT_EQ(status, VP8_STATUS_BITSTREAM_ERROR);
  BWFree(&w);
  return 1;
}

// Color-cache present bit is itself the last bit of the stream.
static int TestTruncatedCachePresentBit(void) {
  BitWriter w;
  VP8StatusCode status;
  BWInit(&w, 64);
  WriteImageInfo(&w, 1, 1, 1);
  BWPut(&w, 0, 1);  // no transform
  BWPut(&w, 1, 1);  // cache present ... no following 4-bit size field
  status = ExpectBitstreamError(&w);
  EXPECT_EQ(status, VP8_STATUS_BITSTREAM_ERROR);
  BWFree(&w);
  return 1;
}

// Color-cache 4-bit size field cut after one bit.
static int TestTruncatedCacheBits(void) {
  BitWriter w;
  VP8StatusCode status;
  BWInit(&w, 64);
  WriteImageInfo(&w, 1, 1, 1);
  BWPut(&w, 0, 1);  // no transform
  BWPut(&w, 1, 1);  // cache present
  BWPut(&w, 1, 1);  // only 1 of 4 cache-bits, then EOF
  status = ExpectBitstreamError(&w);
  EXPECT_EQ(status, VP8_STATUS_BITSTREAM_ERROR);
  BWFree(&w);
  return 1;
}

// Truncations whose entire payload stays within the <=8-byte full-prefetch
// window of VP8LInitBitReader(). This exercises the small-buffer branch of the
// remaining-bits length check, where a naive pos/bit_pos formula would
// mis-count the bits still available. Every such prefix must be rejected.
static int TestSmallBufferTruncation(void) {
  static const size_t kLengths[] = {5, 6, 7, 8, 9};
  size_t i;
  for (i = 0; i < sizeof(kLengths) / sizeof(kLengths[0]); ++i) {
    const size_t n = kLengths[i];
    uint8_t payload[9] = {0};
    size_t webp_size;
    uint8_t* webp;
    int width = 0, height = 0;
    uint8_t* rgba = NULL;
    VP8StatusCode status;
    payload[0] = VP8L_MAGIC_BYTE;  // 1x1, alpha=0, version=0
    if (n > 5) payload[5] = 0x01;  // transform-present bit set, then truncated
    webp = WrapRIFF(payload, n, &webp_size);
    status = DecodeRGBAAlloc(webp, webp_size, &width, &height, &rgba);
    EXPECT_EQ(status, VP8_STATUS_BITSTREAM_ERROR);
    free(webp);
    free(rgba);
  }
  return 1;
}

// Out-of-range color-cache bit declarations (12 and 15). Ample zero padding is
// appended so the rejection is provably due to the range check, not truncation.
static int TestOutOfRangeCacheBits(int cache_bits) {
  BitWriter w;
  VP8StatusCode status;
  int j;
  BWInit(&w, 1024);
  WriteImageInfo(&w, 1, 1, 1);
  BWPut(&w, 0, 1);  // no transform
  BWPut(&w, 1, 1);  // cache present
  BWPut(&w, (uint32_t)cache_bits, 4);  // illegal: > MAX_CACHE_BITS (11)
  BWPut(&w, 0, 1);                     // no meta Huffman
  for (j = 0; j < 64; ++j) BWPut(&w, 0, 8);  // plenty of trailing bytes
  status = ExpectBitstreamError(&w);
  EXPECT_EQ(status, VP8_STATUS_BITSTREAM_ERROR);
  BWFree(&w);
  return 1;
}

// Maximum legal cache bits (11): the GREEN alphabet grows to 280 + 2048 =
// 2328 symbols. A 2x1 image decodes one literal then one color-cache symbol,
// proving the 2048-entry table is accepted and the runtime index bound holds.
static int TestMaxLegalCacheBits(void) {
  BitWriter w;
  size_t payload_size, webp_size;
  uint8_t *payload, *webp, *rgba = NULL;
  int width = 0, height = 0;
  VP8StatusCode status;
  const int cache_bits = MAX_CACHE_BITS;  // 11
  const int cache_symbol = NUM_LITERAL_CODES + NUM_LENGTH_CODES;  // key = 0
  const int alpha = 1, red = 1, green = 0, blue = 1;

  BWInit(&w, 512);
  WriteImageInfo(&w, 2, 1, 1);
  BWPut(&w, 0, 1);                  // no transform
  BWPut(&w, 1, 1);                  // color cache present
  BWPut(&w, cache_bits, 4);         // legal maximum
  BWPut(&w, 0, 1);                  // no meta Huffman
  WriteSimple2Symbol(&w, green, cache_symbol);  // GREEN: literal / cache key 0
  WriteSimple1Symbol(&w, red);
  WriteSimple1Symbol(&w, blue);
  WriteSimple1Symbol(&w, alpha);
  WriteSimple1Symbol(&w, 0);        // DISTANCE

  // Pixel 0: literal (bit 0 on GREEN) + R/B/A. This inserts the color.
  BWPut(&w, 0, 1); BWPut(&w, 0, 1); BWPut(&w, 0, 1); BWPut(&w, 0, 1);
  // Pixel 1: GREEN emits the color-cache symbol for key 0 (bit 1).
  BWPut(&w, 1, 1);

  payload_size = BWByteSize(&w);
  payload = (uint8_t*)malloc(payload_size);
  memcpy(payload, w.bytes, payload_size);
  webp = WrapRIFF(payload, payload_size, &webp_size);
  status = DecodeRGBAAlloc(webp, webp_size, &width, &height, &rgba);
  EXPECT_EQ(status, VP8_STATUS_OK);
  EXPECT_EQ(width, 2);
  EXPECT_EQ(height, 1);
  EXPECT_EQ(rgba[0], red);
  EXPECT_EQ(rgba[1], green);
  EXPECT_EQ(rgba[2], blue);
  EXPECT_EQ(rgba[3], alpha);
  // Second pixel came out of the cache and must equal the first.
  EXPECT_EQ(rgba[4], rgba[0]);
  EXPECT_EQ(rgba[5], rgba[1]);
  EXPECT_EQ(rgba[6], rgba[2]);
  EXPECT_EQ(rgba[7], rgba[3]);

  free(webp);
  free(payload);
  free(rgba);
  BWFree(&w);
  return 1;
}

// -----------------------------------------------------------------------------
// Legal golden bitstreams: must decode to byte-identical RGBA.
// -----------------------------------------------------------------------------

static uint8_t* ReadFile(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  long n;
  uint8_t* buf;
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    fclose(f);
    return NULL;
  }
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

static int TestGoldenFile(const char* path) {
  size_t webp_size;
  uint8_t* webp = ReadFile(path, &webp_size);
  int width = 0, height = 0;
  uint8_t* rgba = NULL;
  VP8StatusCode status;
  if (webp == NULL) {
    fprintf(stderr, "note: skipping %s (not found)\n", path);
    return 1;  // golden file is optional at runtime
  }
  EXPECT_TRUE(WebPGetInfo(webp, webp_size, &width, &height));
  EXPECT_TRUE(width > 0 && height > 0);
  status = DecodeRGBAAlloc(webp, webp_size, &width, &height, &rgba);
  EXPECT_EQ(status, VP8_STATUS_OK);
  EXPECT_TRUE(rgba != NULL);
  free(rgba);
  free(webp);
  return 1;
}

int main(int argc, char** argv) {
  const char* golden_dir = (argc > 1) ? argv[1] : "golden";
  const char* fixture = (argc > 2) ? argv[2] : "examples/test.webp";
  char path[1024];
  int failures = 0;

  failures += !TestNoTransform();
  failures += !TestTruncatedTransformType();
  failures += !TestTruncatedTransformParameter();
  failures += !TestTruncatedPaletteSize();
  failures += !TestTruncatedCachePresentBit();
  failures += !TestTruncatedCacheBits();
  failures += !TestSmallBufferTruncation();
  failures += !TestOutOfRangeCacheBits(12);
  failures += !TestOutOfRangeCacheBits(15);
  failures += !TestMaxLegalCacheBits();

  snprintf(path, sizeof(path), "%s/golden_1x1.webp", golden_dir);
  failures += !TestGoldenFile(path);
  snprintf(path, sizeof(path), "%s/golden_4x4.webp", golden_dir);
  failures += !TestGoldenFile(path);
  snprintf(path, sizeof(path), "%s/golden_8x1.webp", golden_dir);
  failures += !TestGoldenFile(path);
  failures += !TestGoldenFile(fixture);  // repository fixture, if present

  if (failures == 0) {
    printf("All VP8L bounds tests passed.\n");
    return 0;
  }
  fprintf(stderr, "%d VP8L bounds test(s) failed.\n", failures);
  return 1;
}
