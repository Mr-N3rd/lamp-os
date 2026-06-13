// Native tests for FirmwareCarrier (wisp Phase F).
//
// Self-contained: the LSIG parse algorithm + getChunk are redeclared as
// local helpers rather than linking the production .cpp, mirroring the
// test_tuple_sampler discipline. This keeps the test free of mbedTLS,
// Arduino, and ESP32 deps.

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// Canonical LSIG layout per software/lamp-os/src/components/firmware/
// firmware_signature.hpp.
constexpr size_t kFooterLen          = 96;
constexpr size_t kFooterMagicOff     = 0;
constexpr size_t kFooterChannelOff   = 4;
constexpr size_t kFooterFwVerOff     = 12;
constexpr size_t kFooterSignedLenOff = 16;
constexpr size_t kFooterSigOff       = 32;
constexpr uint16_t kChunkSize        = 200;

struct ParsedFooter {
  bool     ok = false;
  char     channel[9] = {0};
  uint32_t firmwareVersion = 0;
  uint32_t signedRegionLength = 0;
};

uint32_t loadU32LE(const uint8_t* p) {
  return  static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

ParsedFooter parseLsig(const uint8_t* blob, size_t blobSize) {
  ParsedFooter out;
  if (!blob || blobSize < kFooterLen) return out;
  const uint8_t* f = blob + (blobSize - kFooterLen);
  if (f[0] != 'L' || f[1] != 'S' || f[2] != 'I' || f[3] != 'G') return out;
  std::memcpy(out.channel, f + kFooterChannelOff, 8);
  out.channel[8] = '\0';
  out.firmwareVersion = loadU32LE(f + kFooterFwVerOff);
  out.signedRegionLength = loadU32LE(f + kFooterSignedLenOff);
  if (static_cast<size_t>(out.signedRegionLength) + kFooterLen != blobSize) {
    out.ok = false;
    return out;
  }
  out.ok = true;
  return out;
}

uint16_t totalChunks(size_t blobSize) {
  if (blobSize == 0) return 0;
  return static_cast<uint16_t>((blobSize + kChunkSize - 1) / kChunkSize);
}

size_t getChunk(const uint8_t* blob, size_t blobSize,
                uint16_t chunkIdx, uint8_t* out, size_t maxLen) {
  if (!blob || !out) return 0;
  const size_t offset = static_cast<size_t>(chunkIdx) * kChunkSize;
  if (offset >= blobSize) return 0;
  const size_t remaining = blobSize - offset;
  const size_t n = remaining < kChunkSize ? remaining : kChunkSize;
  if (maxLen < n) return 0;
  std::memcpy(out, blob + offset, n);
  return n;
}

// Build a synthetic signed blob with body of `signedLen` bytes + valid LSIG.
std::vector<uint8_t> buildBlob(uint32_t signedLen, const char* channel,
                               uint32_t fwVersion, char magic0 = 'L') {
  std::vector<uint8_t> v(static_cast<size_t>(signedLen) + kFooterLen, 0);
  // body: deterministic content for chunk readback verification.
  for (size_t i = 0; i < signedLen; ++i) v[i] = static_cast<uint8_t>(i & 0xFF);
  uint8_t* f = v.data() + signedLen;
  f[0] = static_cast<uint8_t>(magic0);
  f[1] = 'S';
  f[2] = 'I';
  f[3] = 'G';
  std::memset(f + kFooterChannelOff, 0, 8);
  if (channel) {
    const size_t n = std::strlen(channel);
    std::memcpy(f + kFooterChannelOff, channel, n > 8 ? 8 : n);
  }
  f[kFooterFwVerOff + 0] = static_cast<uint8_t>(fwVersion & 0xFF);
  f[kFooterFwVerOff + 1] = static_cast<uint8_t>((fwVersion >> 8) & 0xFF);
  f[kFooterFwVerOff + 2] = static_cast<uint8_t>((fwVersion >> 16) & 0xFF);
  f[kFooterFwVerOff + 3] = static_cast<uint8_t>((fwVersion >> 24) & 0xFF);
  f[kFooterSignedLenOff + 0] = static_cast<uint8_t>(signedLen & 0xFF);
  f[kFooterSignedLenOff + 1] = static_cast<uint8_t>((signedLen >> 8) & 0xFF);
  f[kFooterSignedLenOff + 2] = static_cast<uint8_t>((signedLen >> 16) & 0xFF);
  f[kFooterSignedLenOff + 3] = static_cast<uint8_t>((signedLen >> 24) & 0xFF);
  // signature (64 bytes) left zero.
  return v;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_lsig_parse_happy_path(void) {
  auto blob = buildBlob(1024, "stable", 0x00010002u);
  auto f = parseLsig(blob.data(), blob.size());
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_EQUAL_STRING("stable", f.channel);
  TEST_ASSERT_EQUAL_UINT32(0x00010002u, f.firmwareVersion);
  TEST_ASSERT_EQUAL_UINT32(1024, f.signedRegionLength);
}

void test_lsig_rejects_missing_magic(void) {
  auto blob = buildBlob(512, "stable", 0x00010000u, /*magic0*/ 'X');
  auto f = parseLsig(blob.data(), blob.size());
  TEST_ASSERT_FALSE(f.ok);
}

void test_lsig_rejects_blob_too_short(void) {
  // < 96 bytes
  std::vector<uint8_t> tiny(50, 0xFF);
  auto f = parseLsig(tiny.data(), tiny.size());
  TEST_ASSERT_FALSE(f.ok);
}

void test_lsig_rejects_length_mismatch(void) {
  // Build a blob where signedRegionLength claims a different size than the
  // actual body. Hand-modify the wire bytes.
  auto blob = buildBlob(512, "stable", 0x00010000u);
  // Tamper the signedRegionLength field to claim 1024 (the real body is 512).
  uint8_t* f = blob.data() + 512;
  f[kFooterSignedLenOff + 0] = 0x00;
  f[kFooterSignedLenOff + 1] = 0x04;  // 0x0400 = 1024
  auto parsed = parseLsig(blob.data(), blob.size());
  TEST_ASSERT_FALSE(parsed.ok);
}

void test_channel_zero_padded_and_null_terminated(void) {
  auto blob = buildBlob(100, "ab", 1u);
  auto f = parseLsig(blob.data(), blob.size());
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_EQUAL_STRING("ab", f.channel);
  TEST_ASSERT_EQUAL_UINT8('\0', static_cast<uint8_t>(f.channel[8]));
}

void test_total_chunks_ceil(void) {
  // Body 400 + 96 footer = 496 bytes. Chunks = ceil(496/200) = 3.
  TEST_ASSERT_EQUAL_UINT16(3, totalChunks(496));
  // Body 200 + 96 = 296. Chunks = ceil(296/200) = 2.
  TEST_ASSERT_EQUAL_UINT16(2, totalChunks(296));
  // Exactly aligned: 200*3 = 600. Chunks = 3.
  TEST_ASSERT_EQUAL_UINT16(3, totalChunks(600));
}

void test_get_chunk_first_returns_full_chunk(void) {
  auto blob = buildBlob(1000, "stable", 1u);  // 1000+96 = 1096
  uint8_t out[kChunkSize] = {0};
  const size_t n = getChunk(blob.data(), blob.size(), 0, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(kChunkSize, n);
  // First 200 bytes are deterministic body content: i & 0xFF.
  for (size_t i = 0; i < kChunkSize; ++i) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(i & 0xFF), out[i]);
  }
}

void test_get_chunk_last_returns_partial(void) {
  // Body 250 + 96 footer = 346 bytes total. Chunks = ceil(346/200) = 2.
  // Chunk 0 = bytes 0..199 (full 200). Chunk 1 = bytes 200..345 = 146 bytes.
  auto blob = buildBlob(250, "stable", 1u);
  uint8_t out[kChunkSize] = {0};
  const size_t n = getChunk(blob.data(), blob.size(), 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(146, n);
}

void test_get_chunk_out_of_range_returns_zero(void) {
  auto blob = buildBlob(100, "stable", 1u);  // 196 bytes total → 1 chunk
  uint8_t out[kChunkSize] = {0};
  const size_t n = getChunk(blob.data(), blob.size(), 5, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_get_chunk_rejects_undersized_dest(void) {
  auto blob = buildBlob(500, "stable", 1u);  // 596 bytes, chunk 0 needs 200
  uint8_t out[100] = {0};
  const size_t n = getChunk(blob.data(), blob.size(), 0, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(0, n);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lsig_parse_happy_path);
  RUN_TEST(test_lsig_rejects_missing_magic);
  RUN_TEST(test_lsig_rejects_blob_too_short);
  RUN_TEST(test_lsig_rejects_length_mismatch);
  RUN_TEST(test_channel_zero_padded_and_null_terminated);
  RUN_TEST(test_total_chunks_ceil);
  RUN_TEST(test_get_chunk_first_returns_full_chunk);
  RUN_TEST(test_get_chunk_last_returns_partial);
  RUN_TEST(test_get_chunk_out_of_range_returns_zero);
  RUN_TEST(test_get_chunk_rejects_undersized_dest);
  return UNITY_END();
}
