// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////////
//
// Integration tests for the RIFF metadata (ICCP/EXIF/XMP) paths of the three
// read entry points: the decoder header parser (WebPGetFeatures()/WebPDecode()),
// the demuxer (WebPDemux()/WebPDemuxPartial()) and the mux reader
// (WebPMuxCreate()).
//
// The malformed samples below exercise chunk lengths that reach past the end
// of the file, duplicated singleton chunks, a missing padding byte and VP8X
// flags that disagree with the chunks actually present. Each entry point has a
// documented, distinct error classification for these cases and the tests
// assert those exact codes (rather than a generic failure) so that the
// classification stays stable:
//   - decoder header: VP8_STATUS_BITSTREAM_ERROR vs VP8_STATUS_NOT_ENOUGH_DATA
//     (note WebPGetFeatures() intentionally reports OK for truncated extended
//     files, since the VP8X canvas already provides meaningful features)
//   - demuxer:        WEBP_DEMUX_PARSE_ERROR vs WEBP_DEMUX_PARSED_HEADER
//   - mux reader:     NULL (WebPMuxCreate() validates and rejects)
// Valid samples assert that metadata bytes are preserved verbatim and that the
// decoded pixels are identical to the metadata-free bitstream.
//
// Under ASan/UBSan these tests also act as out-of-bounds read detectors for
// the chunk-walking code.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "./fuzz_utils.h"
#include "webp/decode.h"
#include "webp/demux.h"
#include "webp/encode.h"
#include "webp/mux.h"
#include "webp/mux_types.h"

namespace {

constexpr size_t kRiffHeaderSize = 12;  // "RIFFnnnnWEBP"
constexpr size_t kVp8xChunkSize = 10;   // VP8X payload
constexpr int kCanvasSize = 4;          // test image width/height

//------------------------------------------------------------------------------
// RIFF construction helpers.

void PutLE24(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(value & 0xff);
  out->push_back((value >> 8) & 0xff);
  out->push_back((value >> 16) & 0xff);
}

void PutLE32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(value & 0xff);
  out->push_back((value >> 8) & 0xff);
  out->push_back((value >> 16) & 0xff);
  out->push_back((value >> 24) & 0xff);
}

void AppendFourCC(std::vector<uint8_t>* out, const char fourcc[4]) {
  out->insert(out->end(), fourcc, fourcc + 4);
}

struct ChunkSpec {
  std::string fourcc;
  std::vector<uint8_t> payload;
  // Whether the padding byte is appended for odd-sized payloads.
  bool pad = true;
  // When set, the chunk header lies about the payload size.
  std::optional<uint32_t> declared_size;
};

void AppendChunk(std::vector<uint8_t>* out, const ChunkSpec& spec) {
  AppendFourCC(out, spec.fourcc.c_str());
  PutLE32(out, spec.declared_size.has_value() ? *spec.declared_size
                                              : spec.payload.size());
  out->insert(out->end(), spec.payload.begin(), spec.payload.end());
  if (spec.pad && (spec.payload.size() & 1)) out->push_back(0);
}

// A tiny lossless bitstream shared by all the tests.
struct TestImage {
  std::vector<uint8_t> simple_file;  // RIFF....WEBPVP8L.... (no VP8X)
  std::vector<uint8_t> image_chunk;  // the bare "VP8L" chunk, padding included
};

const TestImage& GetTestImage() {
  static const TestImage* const image = [] {
    // 4x4 opaque RGB gradient.
    std::vector<uint8_t> rgb(kCanvasSize * kCanvasSize * 3);
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = (i * 37 + 11) & 0xff;
    uint8_t* encoded = nullptr;
    const size_t encoded_size = WebPEncodeLosslessRGB(
        rgb.data(), kCanvasSize, kCanvasSize, kCanvasSize * 3, &encoded);
    if (encoded_size <= kRiffHeaderSize || encoded == nullptr) abort();
    if (std::memcmp(encoded + kRiffHeaderSize, "VP8L", 4) != 0) abort();
    TestImage* const img = new TestImage();
    img->simple_file.assign(encoded, encoded + encoded_size);
    img->image_chunk.assign(encoded + kRiffHeaderSize,
                            encoded + encoded_size);
    WebPFree(encoded);
    return img;
  }();
  return *image;
}

// Assembles "RIFF<riff_size>WEBP" + VP8X + pre_chunks + image + post_chunks.
// The RIFF size honestly covers the bytes written unless 'riff_size_override'
// is given.
std::vector<uint8_t> BuildExtendedFile(
    uint32_t flags, const std::vector<ChunkSpec>& pre_chunks,
    const std::vector<ChunkSpec>& post_chunks = {},
    std::optional<uint32_t> riff_size_override = std::nullopt) {
  const TestImage& img = GetTestImage();
  std::vector<uint8_t> out;
  AppendFourCC(&out, "RIFF");
  PutLE32(&out, 0);  // patched below
  AppendFourCC(&out, "WEBP");
  AppendFourCC(&out, "VP8X");
  PutLE32(&out, kVp8xChunkSize);
  PutLE32(&out, flags);
  PutLE24(&out, kCanvasSize - 1);
  PutLE24(&out, kCanvasSize - 1);
  for (const ChunkSpec& chunk : pre_chunks) AppendChunk(&out, chunk);
  out.insert(out.end(), img.image_chunk.begin(), img.image_chunk.end());
  for (const ChunkSpec& chunk : post_chunks) AppendChunk(&out, chunk);
  const uint32_t riff_size =
      riff_size_override.has_value() ? *riff_size_override
                                     : static_cast<uint32_t>(out.size() - 8);
  out[4] = riff_size & 0xff;
  out[5] = (riff_size >> 8) & 0xff;
  out[6] = (riff_size >> 16) & 0xff;
  out[7] = (riff_size >> 24) & 0xff;
  return out;
}

//------------------------------------------------------------------------------
// Metadata payloads (odd/even sizes on purpose).

std::vector<uint8_t> IccpPayload() {
  std::vector<uint8_t> payload(32);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i * 3 + 1);
  }
  return payload;
}

// 13 bytes: odd, requires a padding byte on disk.
std::vector<uint8_t> ExifPayload() {
  return {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', '*', 0, 8, 0, 7};
}

std::vector<uint8_t> XmpPayload() {
  const std::string xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">t</x:xmpmeta>";
  return std::vector<uint8_t>(xmp.begin(), xmp.end());
}

ChunkSpec Chunk(const char fourcc[4], const std::vector<uint8_t>& payload) {
  return ChunkSpec{std::string(fourcc, 4), payload, true, std::nullopt};
}

//------------------------------------------------------------------------------
// Entry point wrappers.

// Decoder header parse only.
VP8StatusCode DecodeHeaderStatus(const std::vector<uint8_t>& file) {
  WebPBitstreamFeatures features;
  return WebPGetFeatures(file.data(), file.size(), &features);
}

// Full decode (have_all_data = 1 path of the header parser included).
VP8StatusCode FullDecodeStatus(const std::vector<uint8_t>& file) {
  WebPDecoderConfig config;
  if (!WebPInitDecoderConfig(&config)) return VP8_STATUS_INVALID_PARAM;
  const VP8StatusCode status = WebPDecode(file.data(), file.size(), &config);
  WebPFreeDecBuffer(&config.output);
  return status;
}

WebPDemuxState DemuxPartialState(const std::vector<uint8_t>& file) {
  const WebPData data = {file.data(), file.size()};
  WebPDemuxState state;
  WebPDemuxer* const dmux = WebPDemuxPartial(&data, &state);
  WebPDemuxDelete(dmux);
  return state;
}

bool DemuxAccepts(const std::vector<uint8_t>& file) {
  const WebPData data = {file.data(), file.size()};
  WebPDemuxer* const dmux = WebPDemux(&data);
  const bool ok = (dmux != nullptr);
  WebPDemuxDelete(dmux);
  return ok;
}

bool MuxAccepts(const std::vector<uint8_t>& file) {
  const WebPData data = {file.data(), file.size()};
  WebPMux* const mux = WebPMuxCreate(&data, 0);
  const bool ok = (mux != nullptr);
  WebPMuxDelete(mux);
  return ok;
}

// Returns the payload of the 'nth' (1-based) chunk 'fourcc', or nullopt when
// the file cannot be demuxed or the chunk is absent.
std::optional<std::vector<uint8_t>> DemuxGetChunk(
    const std::vector<uint8_t>& file, const char fourcc[4], int nth) {
  const WebPData data = {file.data(), file.size()};
  const std::unique_ptr<WebPDemuxer, fuzz_utils::UniquePtrDeleter> dmux(
      WebPDemux(&data));
  if (!dmux) return std::nullopt;
  WebPChunkIterator iter;
  std::optional<std::vector<uint8_t>> result;
  if (WebPDemuxGetChunk(dmux.get(), fourcc, nth, &iter)) {
    result.emplace(iter.chunk.bytes, iter.chunk.bytes + iter.chunk.size);
  }
  WebPDemuxReleaseChunkIterator(&iter);
  return result;
}

// Returns the number of chunks 'fourcc' seen by the demuxer, or -1 when the
// file cannot be demuxed.
int DemuxChunkCount(const std::vector<uint8_t>& file, const char fourcc[4]) {
  const WebPData data = {file.data(), file.size()};
  const std::unique_ptr<WebPDemuxer, fuzz_utils::UniquePtrDeleter> dmux(
      WebPDemux(&data));
  if (!dmux) return -1;
  WebPChunkIterator iter;
  WebPDemuxGetChunk(dmux.get(), fourcc, 1, &iter);
  const int count = iter.num_chunks;
  WebPDemuxReleaseChunkIterator(&iter);
  return count;
}

std::optional<std::vector<uint8_t>> MuxGetChunk(
    const std::vector<uint8_t>& file, const char fourcc[4]) {
  const WebPData data = {file.data(), file.size()};
  const std::unique_ptr<WebPMux, fuzz_utils::UniquePtrDeleter> mux(
      WebPMuxCreate(&data, 0));
  if (!mux) return std::nullopt;
  WebPData chunk;
  WebPDataInit(&chunk);
  if (WebPMuxGetChunk(mux.get(), fourcc, &chunk) != WEBP_MUX_OK) {
    return std::nullopt;
  }
  return std::vector<uint8_t>(chunk.bytes, chunk.bytes + chunk.size);
}

// Asserts both files decode to identical RGBA pixels.
void ExpectSameDecodedPixels(const std::vector<uint8_t>& a,
                             const std::vector<uint8_t>& b) {
  int width_a, height_a, width_b, height_b;
  uint8_t* const rgba_a = WebPDecodeRGBA(a.data(), a.size(), &width_a,
                                         &height_a);
  uint8_t* const rgba_b = WebPDecodeRGBA(b.data(), b.size(), &width_b,
                                         &height_b);
  ASSERT_NE(rgba_a, nullptr);
  ASSERT_NE(rgba_b, nullptr);
  EXPECT_EQ(width_a, width_b);
  EXPECT_EQ(height_a, height_b);
  EXPECT_EQ(
      0, std::memcmp(rgba_a, rgba_b,
                     static_cast<size_t>(width_a) * height_a * 4));
  WebPFree(rgba_a);
  WebPFree(rgba_b);
}

// Asserts all three read entry points accept 'file' and return the expected
// metadata payloads byte-for-byte.
void ExpectMetadataRoundTrip(const std::vector<uint8_t>& file,
                             const std::vector<uint8_t>* iccp,
                             const std::vector<uint8_t>* exif,
                             const std::vector<uint8_t>* xmp) {
  ASSERT_EQ(DecodeHeaderStatus(file), VP8_STATUS_OK);
  ASSERT_EQ(FullDecodeStatus(file), VP8_STATUS_OK);
  ASSERT_EQ(DemuxPartialState(file), WEBP_DEMUX_DONE);
  ASSERT_TRUE(DemuxAccepts(file));
  ASSERT_TRUE(MuxAccepts(file));
  const struct {
    const char fourcc[5];
    const std::vector<uint8_t>* payload;
  } kChunks[] = {{"ICCP", iccp}, {"EXIF", exif}, {"XMP ", xmp}};
  for (const auto& chunk : kChunks) {
    const std::optional<std::vector<uint8_t>> demuxed =
        DemuxGetChunk(file, chunk.fourcc, 1);
    const std::optional<std::vector<uint8_t>> muxed =
        MuxGetChunk(file, chunk.fourcc);
    if (chunk.payload == nullptr) {
      EXPECT_FALSE(demuxed.has_value()) << chunk.fourcc;
      EXPECT_FALSE(muxed.has_value()) << chunk.fourcc;
    } else {
      ASSERT_TRUE(demuxed.has_value()) << chunk.fourcc;
      ASSERT_TRUE(muxed.has_value()) << chunk.fourcc;
      EXPECT_EQ(*demuxed, *chunk.payload) << chunk.fourcc;
      EXPECT_EQ(*muxed, *chunk.payload) << chunk.fourcc;
    }
  }
  ExpectSameDecodedPixels(file, GetTestImage().simple_file);
}

//------------------------------------------------------------------------------
// Valid samples.

TEST(MetadataApiTest, ValidSingleIccpChunk) {
  const std::vector<uint8_t> iccp = IccpPayload();
  const std::vector<uint8_t> file =
      BuildExtendedFile(ICCP_FLAG, {Chunk("ICCP", iccp)});
  ExpectMetadataRoundTrip(file, &iccp, nullptr, nullptr);
  // The decoder header must report the VP8X canvas and no alpha/animation.
  WebPBitstreamFeatures features;
  ASSERT_EQ(WebPGetFeatures(file.data(), file.size(), &features),
            VP8_STATUS_OK);
  EXPECT_EQ(features.width, kCanvasSize);
  EXPECT_EQ(features.height, kCanvasSize);
  EXPECT_FALSE(features.has_alpha);
  EXPECT_FALSE(features.has_animation);
  // The demuxer exposes the ICCP feature flag.
  const WebPData data = {file.data(), file.size()};
  const std::unique_ptr<WebPDemuxer, fuzz_utils::UniquePtrDeleter> dmux(
      WebPDemux(&data));
  ASSERT_NE(dmux, nullptr);
  const uint32_t demux_flags = WebPDemuxGetI(dmux.get(), WEBP_FF_FORMAT_FLAGS);
  EXPECT_NE(demux_flags & ICCP_FLAG, 0u);
  EXPECT_EQ(demux_flags & (EXIF_FLAG | XMP_FLAG), 0u);
}

TEST(MetadataApiTest, ValidMultipleChunks) {
  const std::vector<uint8_t> iccp = IccpPayload();
  const std::vector<uint8_t> exif = ExifPayload();
  const std::vector<uint8_t> xmp = XmpPayload();
  // Spec order: ICCP before the image, EXIF and XMP after it.
  const std::vector<uint8_t> file = BuildExtendedFile(
      ICCP_FLAG | EXIF_FLAG | XMP_FLAG, {Chunk("ICCP", iccp)},
      {Chunk("EXIF", exif), Chunk("XMP ", xmp)});
  ExpectMetadataRoundTrip(file, &iccp, &exif, &xmp);
  EXPECT_EQ(DemuxChunkCount(file, "ICCP"), 1);
  EXPECT_EQ(DemuxChunkCount(file, "EXIF"), 1);
  EXPECT_EQ(DemuxChunkCount(file, "XMP "), 1);
}

TEST(MetadataApiTest, ValidOddLengthChunkWithPadding) {
  const std::vector<uint8_t> exif = ExifPayload();
  ASSERT_EQ(exif.size() & 1, 1u);  // the padding path is what is tested
  const std::vector<uint8_t> file =
      BuildExtendedFile(EXIF_FLAG, {Chunk("EXIF", exif)});
  ExpectMetadataRoundTrip(file, nullptr, &exif, nullptr);
  // The returned payload must be the unpadded 13 bytes.
  const std::optional<std::vector<uint8_t>> demuxed =
      DemuxGetChunk(file, "EXIF", 1);
  ASSERT_TRUE(demuxed.has_value());
  EXPECT_EQ(demuxed->size(), exif.size());
}

TEST(MetadataApiTest, ValidZeroLengthChunk) {
  const std::vector<uint8_t> empty;
  const std::vector<uint8_t> file =
      BuildExtendedFile(EXIF_FLAG, {Chunk("EXIF", empty)});
  ExpectMetadataRoundTrip(file, nullptr, &empty, nullptr);
}

TEST(MetadataApiTest, ValidNoVp8xNoMetadata) {
  const std::vector<uint8_t>& file = GetTestImage().simple_file;
  ASSERT_EQ(DecodeHeaderStatus(file), VP8_STATUS_OK);
  ASSERT_EQ(FullDecodeStatus(file), VP8_STATUS_OK);
  ASSERT_EQ(DemuxPartialState(file), WEBP_DEMUX_DONE);
  ASSERT_TRUE(DemuxAccepts(file));
  ASSERT_TRUE(MuxAccepts(file));
  EXPECT_EQ(DemuxChunkCount(file, "ICCP"), 0);
  EXPECT_EQ(DemuxChunkCount(file, "EXIF"), 0);
  EXPECT_EQ(DemuxChunkCount(file, "XMP "), 0);
  const WebPData data = {file.data(), file.size()};
  const std::unique_ptr<WebPMux, fuzz_utils::UniquePtrDeleter> mux(
      WebPMuxCreate(&data, 0));
  ASSERT_NE(mux, nullptr);
  WebPData chunk;
  WebPDataInit(&chunk);
  EXPECT_EQ(WebPMuxGetChunk(mux.get(), "ICCP", &chunk), WEBP_MUX_NOT_FOUND);
  uint32_t flags = 0xffffffffu;
  EXPECT_EQ(WebPMuxGetFeatures(mux.get(), &flags), WEBP_MUX_OK);
  EXPECT_EQ(flags & (ICCP_FLAG | EXIF_FLAG | XMP_FLAG), 0u);
}

// The mux write path must produce files that all read entry points accept,
// with metadata preserved byte-for-byte.
TEST(MetadataApiTest, MuxWriteReadRoundTrip) {
  const std::vector<uint8_t> iccp = IccpPayload();
  const std::vector<uint8_t> exif = ExifPayload();
  const std::vector<uint8_t> xmp = XmpPayload();
  const TestImage& img = GetTestImage();

  const std::unique_ptr<WebPMux, fuzz_utils::UniquePtrDeleter> mux(
      WebPMuxNew());
  ASSERT_NE(mux, nullptr);
  WebPData image_data = {img.simple_file.data(), img.simple_file.size()};
  ASSERT_EQ(WebPMuxSetImage(mux.get(), &image_data, 1), WEBP_MUX_OK);
  const WebPData iccp_data = {iccp.data(), iccp.size()};
  const WebPData exif_data = {exif.data(), exif.size()};
  const WebPData xmp_data = {xmp.data(), xmp.size()};
  ASSERT_EQ(WebPMuxSetChunk(mux.get(), "ICCP", &iccp_data, 1), WEBP_MUX_OK);
  ASSERT_EQ(WebPMuxSetChunk(mux.get(), "EXIF", &exif_data, 1), WEBP_MUX_OK);
  ASSERT_EQ(WebPMuxSetChunk(mux.get(), "XMP ", &xmp_data, 1), WEBP_MUX_OK);
  WebPData assembled;
  WebPDataInit(&assembled);
  ASSERT_EQ(WebPMuxAssemble(mux.get(), &assembled), WEBP_MUX_OK);
  const std::vector<uint8_t> file(assembled.bytes,
                                  assembled.bytes + assembled.size);
  WebPDataClear(&assembled);

  ExpectMetadataRoundTrip(file, &iccp, &exif, &xmp);
}

//------------------------------------------------------------------------------
// Malformed samples: chunk length reaches past the end of the file.

// The ICCP header declares a payload far larger than what the (honest) RIFF
// size and the physical file contain.
TEST(MetadataApiTest, ChunkLengthPastEndOfFile) {
  const std::vector<uint8_t> iccp = IccpPayload();
  ChunkSpec bad_iccp = Chunk("ICCP", iccp);
  bad_iccp.declared_size = iccp.size() + 0x1000;
  const std::vector<uint8_t> file =
      BuildExtendedFile(ICCP_FLAG, {bad_iccp});
  // The declared chunk size overflows the RIFF size: a structural error.
  EXPECT_EQ(DecodeHeaderStatus(file), VP8_STATUS_BITSTREAM_ERROR);
  EXPECT_EQ(DemuxPartialState(file), WEBP_DEMUX_PARSE_ERROR);
  EXPECT_FALSE(DemuxAccepts(file));
  EXPECT_FALSE(MuxAccepts(file));
}

// Same overshoot, but the RIFF size is inflated to cover the declared chunk
// while the physical file is short: a truncation, not a structural error.
TEST(MetadataApiTest, ChunkLengthPastEndOfFileInflatedRiff) {
  const std::vector<uint8_t> iccp = IccpPayload();
  ChunkSpec bad_iccp = Chunk("ICCP", iccp);
  bad_iccp.declared_size = iccp.size() + 0x1000;
  // First build with an honest RIFF size to learn the file size, then lie.
  const std::vector<uint8_t> reference =
      BuildExtendedFile(ICCP_FLAG, {bad_iccp});
  const uint32_t inflated_riff_size =
      static_cast<uint32_t>(reference.size() - 8) + 0x2000;
  const std::vector<uint8_t> file =
      BuildExtendedFile(ICCP_FLAG, {bad_iccp}, {}, inflated_riff_size);
  ASSERT_EQ(file.size(), reference.size());
  // WebPGetFeatures() deliberately reports OK for truncated extended files:
  // the VP8X canvas is already known, so the features are still meaningful
  // (see the VP8_STATUS_NOT_ENOUGH_DATA special case in GetHeaders()).
  WebPBitstreamFeatures features;
  ASSERT_EQ(WebPGetFeatures(file.data(), file.size(), &features),
            VP8_STATUS_OK);
  EXPECT_EQ(features.width, kCanvasSize);
  EXPECT_EQ(features.height, kCanvasSize);
  // The full decode has all the data and detects the truncated RIFF.
  EXPECT_EQ(FullDecodeStatus(file), VP8_STATUS_NOT_ENOUGH_DATA);
  // The demuxer fully parsed the VP8X header and waits for more chunk data
  // instead of reading out of bounds.
  EXPECT_EQ(DemuxPartialState(file), WEBP_DEMUX_PARSED_HEADER);
  EXPECT_FALSE(DemuxAccepts(file));
  EXPECT_FALSE(MuxAccepts(file));
}

// A chunk size above MAX_CHUNK_PAYLOAD is rejected outright.
TEST(MetadataApiTest, ChunkSizeExceedsMaxPayload) {
  ChunkSpec bad_iccp = Chunk("ICCP", {});
  bad_iccp.declared_size = 0xffffffffu;
  const std::vector<uint8_t> file = BuildExtendedFile(ICCP_FLAG, {bad_iccp});
  EXPECT_EQ(DecodeHeaderStatus(file), VP8_STATUS_BITSTREAM_ERROR);
  EXPECT_EQ(DemuxPartialState(file), WEBP_DEMUX_PARSE_ERROR);
  EXPECT_FALSE(DemuxAccepts(file));
  EXPECT_FALSE(MuxAccepts(file));
}

//------------------------------------------------------------------------------
// Malformed samples: duplicated singleton chunk.

// ICCP may appear at most once. The decoder and demuxer tolerate duplicates
// (the demuxer exposes both), the mux reader rejects the file.
TEST(MetadataApiTest, DuplicateIccpChunk) {
  const std::vector<uint8_t> iccp = IccpPayload();
  std::vector<uint8_t> iccp2 = iccp;
  iccp2[0] ^= 0xff;  // distinct payloads to tell the chunks apart
  const std::vector<uint8_t> file = BuildExtendedFile(
      ICCP_FLAG, {Chunk("ICCP", iccp), Chunk("ICCP", iccp2)});
  EXPECT_EQ(DecodeHeaderStatus(file), VP8_STATUS_OK);
  EXPECT_EQ(FullDecodeStatus(file), VP8_STATUS_OK);
  EXPECT_EQ(DemuxPartialState(file), WEBP_DEMUX_DONE);
  ASSERT_TRUE(DemuxAccepts(file));
  EXPECT_EQ(DemuxChunkCount(file, "ICCP"), 2);
  const std::optional<std::vector<uint8_t>> first =
      DemuxGetChunk(file, "ICCP", 1);
  const std::optional<std::vector<uint8_t>> second =
      DemuxGetChunk(file, "ICCP", 2);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, iccp);
  EXPECT_EQ(*second, iccp2);
  EXPECT_FALSE(DemuxGetChunk(file, "ICCP", 3).has_value());
  // MuxValidate() enforces at most one ICCP chunk.
  EXPECT_FALSE(MuxAccepts(file));
}

//------------------------------------------------------------------------------
// Malformed samples: missing padding byte after an odd-sized chunk.

// The odd-sized EXIF chunk is not followed by its padding byte, so every
// parser lands one byte inside the "VP8L" tag of the image chunk. The
// misaligned "chunk size" then reads as 0x2f000000 (the VP8L signature byte
// is the high byte), which overflows the RIFF size on all three paths.
TEST(MetadataApiTest, MissingPaddingByte) {
  const std::vector<uint8_t> exif = ExifPayload();
  ASSERT_EQ(exif.size() & 1, 1u);
  ChunkSpec unpadded_exif = Chunk("EXIF", exif);
  unpadded_exif.pad = false;
  const std::vector<uint8_t> file =
      BuildExtendedFile(EXIF_FLAG, {unpadded_exif});
  EXPECT_EQ(DecodeHeaderStatus(file), VP8_STATUS_BITSTREAM_ERROR);
  EXPECT_EQ(DemuxPartialState(file), WEBP_DEMUX_PARSE_ERROR);
  EXPECT_FALSE(DemuxAccepts(file));
  EXPECT_FALSE(MuxAccepts(file));
}

//------------------------------------------------------------------------------
// Malformed samples: VP8X flags inconsistent with the chunks present.

// EXIF_FLAG set but no EXIF chunk: the decoder and demuxer tolerate it (the
// chunk is simply absent), the mux reader rejects the inconsistency.
TEST(MetadataApiTest, Vp8xFlagSetButChunkMissing) {
  const std::vector<uint8_t> file = BuildExtendedFile(EXIF_FLAG, {});
  EXPECT_EQ(DecodeHeaderStatus(file), VP8_STATUS_OK);
  EXPECT_EQ(FullDecodeStatus(file), VP8_STATUS_OK);
  EXPECT_EQ(DemuxPartialState(file), WEBP_DEMUX_DONE);
  ASSERT_TRUE(DemuxAccepts(file));
  EXPECT_EQ(DemuxChunkCount(file, "EXIF"), 0);
  EXPECT_FALSE(MuxAccepts(file));
}

// EXIF chunk present but EXIF_FLAG clear: the decoder skips the chunk, the
// demuxer parses the file but does not surface the chunk, the mux reader
// rejects the inconsistency.
TEST(MetadataApiTest, ChunkPresentButVp8xFlagMissing) {
  const std::vector<uint8_t> exif = ExifPayload();
  const std::vector<uint8_t> file = BuildExtendedFile(0, {Chunk("EXIF", exif)});
  EXPECT_EQ(DecodeHeaderStatus(file), VP8_STATUS_OK);
  EXPECT_EQ(FullDecodeStatus(file), VP8_STATUS_OK);
  EXPECT_EQ(DemuxPartialState(file), WEBP_DEMUX_DONE);
  ASSERT_TRUE(DemuxAccepts(file));
  EXPECT_EQ(DemuxChunkCount(file, "EXIF"), 0);
  EXPECT_FALSE(DemuxGetChunk(file, "EXIF", 1).has_value());
  EXPECT_FALSE(MuxAccepts(file));
  // The image itself is still decoded identically.
  ExpectSameDecodedPixels(file, GetTestImage().simple_file);
}

//------------------------------------------------------------------------------
// Fuzzed properties (regression path).

// Arbitrary metadata payloads survive the mux write -> assemble -> demux/mux
// read round trip byte-for-byte, and never change the decoded pixels.
void RoundTripPreservesChunks(const std::vector<uint8_t>& iccp,
                              const std::vector<uint8_t>& exif,
                              const std::vector<uint8_t>& xmp) {
  const TestImage& img = GetTestImage();
  const std::unique_ptr<WebPMux, fuzz_utils::UniquePtrDeleter> mux(
      WebPMuxNew());
  ASSERT_NE(mux, nullptr);
  WebPData image_data = {img.simple_file.data(), img.simple_file.size()};
  ASSERT_EQ(WebPMuxSetImage(mux.get(), &image_data, 1), WEBP_MUX_OK);
  const WebPData iccp_data = {iccp.data(), iccp.size()};
  const WebPData exif_data = {exif.data(), exif.size()};
  const WebPData xmp_data = {xmp.data(), xmp.size()};
  ASSERT_EQ(WebPMuxSetChunk(mux.get(), "ICCP", &iccp_data, 1), WEBP_MUX_OK);
  ASSERT_EQ(WebPMuxSetChunk(mux.get(), "EXIF", &exif_data, 1), WEBP_MUX_OK);
  ASSERT_EQ(WebPMuxSetChunk(mux.get(), "XMP ", &xmp_data, 1), WEBP_MUX_OK);
  WebPData assembled;
  WebPDataInit(&assembled);
  ASSERT_EQ(WebPMuxAssemble(mux.get(), &assembled), WEBP_MUX_OK);
  const std::vector<uint8_t> file(assembled.bytes,
                                  assembled.bytes + assembled.size);
  WebPDataClear(&assembled);

  ASSERT_EQ(DecodeHeaderStatus(file), VP8_STATUS_OK);
  ASSERT_EQ(DemuxPartialState(file), WEBP_DEMUX_DONE);
  ASSERT_TRUE(MuxAccepts(file));
  EXPECT_EQ(DemuxGetChunk(file, "ICCP", 1), std::make_optional(iccp));
  EXPECT_EQ(DemuxGetChunk(file, "EXIF", 1), std::make_optional(exif));
  EXPECT_EQ(DemuxGetChunk(file, "XMP ", 1), std::make_optional(xmp));
  EXPECT_EQ(MuxGetChunk(file, "ICCP"), std::make_optional(iccp));
  EXPECT_EQ(MuxGetChunk(file, "EXIF"), std::make_optional(exif));
  EXPECT_EQ(MuxGetChunk(file, "XMP "), std::make_optional(xmp));
  ExpectSameDecodedPixels(file, img.simple_file);
}

// Byte mutations and truncations of a valid extended file must never crash
// nor read out of bounds (ASan/UBSan), and the error classification of each
// entry point must stay within its documented set.
void MutationsKeepStableClassification(
    const std::vector<std::pair<uint16_t, uint8_t>>& mutations,
    uint16_t truncate_at, bool apply_truncation) {
  std::vector<uint8_t> file = BuildExtendedFile(
      ICCP_FLAG | EXIF_FLAG | XMP_FLAG, {Chunk("ICCP", IccpPayload())},
      {Chunk("EXIF", ExifPayload()), Chunk("XMP ", XmpPayload())});
  if (apply_truncation) {
    file.resize(truncate_at % (file.size() + 1));
  }
  for (const auto& [position, value] : mutations) {
    if (file.empty()) break;
    file[position % file.size()] = value;
  }
  // An empty vector may have a null data pointer, which the APIs reject with
  // INVALID_PARAM instead of a parse classification; nothing to test here.
  if (file.empty()) return;

  // The decoder header parser only reports OK / structural error / truncation
  // for in-memory RIFF data (never INVALID_PARAM, USER_ABORT, ...).
  const VP8StatusCode header_status = DecodeHeaderStatus(file);
  EXPECT_TRUE(header_status == VP8_STATUS_OK ||
              header_status == VP8_STATUS_BITSTREAM_ERROR ||
              header_status == VP8_STATUS_NOT_ENOUGH_DATA)
      << "unexpected decoder header status: " << header_status;

  // The demuxer state machine only settles in these states.
  const WebPData data = {file.data(), file.size()};
  WebPDemuxState state;
  WebPDemuxer* const dmux = WebPDemuxPartial(&data, &state);
  EXPECT_TRUE(state == WEBP_DEMUX_PARSE_ERROR ||
              state == WEBP_DEMUX_PARSING_HEADER ||
              state == WEBP_DEMUX_PARSED_HEADER || state == WEBP_DEMUX_DONE)
      << "unexpected demux state: " << state;
  // Any chunk the demuxer surfaces must lie within the input buffer.
  if (dmux != nullptr) {
    for (const char* fourcc : {"ICCP", "EXIF", "XMP "}) {
      WebPChunkIterator iter;
      if (WebPDemuxGetChunk(dmux, fourcc, 1, &iter)) {
        do {
          EXPECT_LE(iter.chunk.size, file.size());
        } while (WebPDemuxNextChunk(&iter));
      }
      WebPDemuxReleaseChunkIterator(&iter);
    }
  }
  WebPDemuxDelete(dmux);

  // Any file the mux reader accepts must re-assemble into a file that the
  // demuxer and the mux reader accept again.
  WebPMux* const mux = WebPMuxCreate(&data, 0);
  if (mux != nullptr) {
    WebPData assembled;
    WebPDataInit(&assembled);
    ASSERT_EQ(WebPMuxAssemble(mux, &assembled), WEBP_MUX_OK);
    WebPDemuxer* const check = WebPDemux(&assembled);
    EXPECT_NE(check, nullptr);
    WebPDemuxDelete(check);
    WebPMux* const mux2 = WebPMuxCreate(&assembled, 0);
    EXPECT_NE(mux2, nullptr);
    WebPMuxDelete(mux2);
    WebPDataClear(&assembled);
    WebPMuxDelete(mux);
  }
}

auto ArbitraryPayload() {
  // Non-empty (WebPMuxSetChunk() rejects NULL bytes) and small enough to keep
  // the fuzzed files fast to parse.
  return fuzztest::VectorOf(fuzztest::Arbitrary<uint8_t>())
      .WithMinSize(1)
      .WithMaxSize(256);
}

}  // namespace

FUZZ_TEST(MetadataApi, RoundTripPreservesChunks)
    .WithDomains(ArbitraryPayload(), ArbitraryPayload(), ArbitraryPayload());

FUZZ_TEST(MetadataApi, MutationsKeepStableClassification)
    .WithDomains(
        /*mutations=*/fuzztest::VectorOf(
            fuzztest::PairOf(fuzztest::Arbitrary<uint16_t>(),
                             fuzztest::Arbitrary<uint8_t>()))
            .WithMaxSize(16),
        /*truncate_at=*/fuzztest::Arbitrary<uint16_t>(),
        /*apply_truncation=*/fuzztest::Arbitrary<bool>());
