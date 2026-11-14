// Golden-sample generator: losslessly encodes a few small RGBA images with
// distinct characteristics (flat, multi-color, gradient) and writes each as a
// complete RIFF/WEBP file. These are known-good VP8L bitstreams used by the
// regression test to prove legal images still decode to identical pixels.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webp/encode.h"

static int write_file(const char* path, const uint8_t* data, size_t n) {
  FILE* f = fopen(path, "wb");
  if (f == NULL) return 0;
  if (fwrite(data, 1, n, f) != n) {
    fclose(f);
    return 0;
  }
  fclose(f);
  return 1;
}

static int emit(const uint8_t* rgba, int w, int h, const char* path) {
  uint8_t* out = NULL;
  size_t n = WebPEncodeLosslessRGBA(rgba, w, h, w * 4, &out);
  int ok;
  if (n == 0) {
    fprintf(stderr, "lossless encode failed for %s\n", path);
    return 0;
  }
  ok = write_file(path, out, n);
  free(out);
  printf("wrote %s (%zu bytes, %dx%d)\n", path, n, w, h);
  return ok;
}

int main(int argc, char** argv) {
  const char* dir = (argc > 1) ? argv[1] : ".";
  char path[512];
  int x, y;

  // Flat 1x1.
  {
    uint8_t px[4] = {200, 100, 50, 255};
    snprintf(path, sizeof(path), "%s/golden_1x1.webp", dir);
    if (!emit(px, 1, 1, path)) return 1;
  }

  // 4x4 multi-color (encourages color indexing / multiple Huffman groups).
  {
    const int W = 4, H = 4;
    uint8_t img[W * H * 4];
    for (y = 0; y < H; ++y) {
      for (x = 0; x < W; ++x) {
        uint8_t* p = &img[(y * W + x) * 4];
        p[0] = (uint8_t)(x * 60);          // R
        p[1] = (uint8_t)(y * 60);          // G
        p[2] = (uint8_t)((x + y) * 30);    // B
        p[3] = 255;                        // A
      }
    }
    snprintf(path, sizeof(path), "%s/golden_4x4.webp", dir);
    if (!emit(img, W, H, path)) return 1;
  }

  // 8x1 gradient with alpha variation (exercises more literals/cache entries).
  {
    const int W = 8, H = 1;
    uint8_t img[W * H * 4];
    for (x = 0; x < W; ++x) {
      uint8_t* p = &img[x * 4];
      p[0] = (uint8_t)(x * 30);
      p[1] = (uint8_t)(255 - x * 20);
      p[2] = (uint8_t)(x * 17);
      p[3] = (uint8_t)(128 + x * 16);
    }
    snprintf(path, sizeof(path), "%s/golden_8x1.webp", dir);
    if (!emit(img, W, H, path)) return 1;
  }
  return 0;
}
