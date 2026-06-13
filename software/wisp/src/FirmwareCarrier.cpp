#include "FirmwareCarrier.h"

#include <Arduino.h>
#include <mbedtls/sha256.h>

#include <cstring>

#include "embedded_firmware.h"
#include "lamp_protocol.hpp"

namespace wisp {

namespace {

// Little-endian load helpers — match the lamp_protocol.hpp convention.
inline uint32_t loadU32LE(const uint8_t* p) {
  return  static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

// LSIG footer wire layout (96 bytes total) — CANONICAL per
// software/lamp-os/src/components/firmware/firmware_signature.hpp.
//
//   magic(4)               = "LSIG"
//   channel(8)             = zero-padded ASCII
//   firmwareVersion(4 LE)
//   signedRegionLength(4 LE)
//   reserved(12)           = zeros
//   signature(64)          = ed25519 over image[0..signedRegionLength)
constexpr size_t kFooterLen          = 96;
constexpr size_t kFooterMagicOff     = 0;
constexpr size_t kFooterChannelOff   = 4;
constexpr size_t kFooterFwVerOff     = 12;
constexpr size_t kFooterSignedLenOff = 16;
constexpr size_t kFooterReservedOff  = 20;
constexpr size_t kFooterReservedLen  = 12;
constexpr size_t kFooterSigOff       = 32;

}  // namespace

bool FirmwareCarrier::begin() {
  if (ready_) return true;

  blob_     = kEmbeddedFirmware;
  blobSize_ = kEmbeddedFirmwareLen;

  if (!blob_ || blobSize_ < kFooterLen) {
    Serial.println("[wisp.fwcarrier] disabled: blob smaller than LSIG footer");
    return false;
  }

  const uint8_t* footer = blob_ + (blobSize_ - kFooterLen);

  if (footer[kFooterMagicOff + 0] != 'L' ||
      footer[kFooterMagicOff + 1] != 'S' ||
      footer[kFooterMagicOff + 2] != 'I' ||
      footer[kFooterMagicOff + 3] != 'G') {
    Serial.println("[wisp.fwcarrier] disabled: LSIG magic mismatch");
    return false;
  }

  std::memcpy(footer_.magic, footer + kFooterMagicOff, 4);
  std::memcpy(footer_.channel, footer + kFooterChannelOff, 8);
  footer_.channel[8] = '\0';
  footer_.firmwareVersion = loadU32LE(footer + kFooterFwVerOff);
  footer_.signedRegionLength = loadU32LE(footer + kFooterSignedLenOff);
  std::memcpy(footer_.reserved, footer + kFooterReservedOff, kFooterReservedLen);
  std::memcpy(footer_.signature, footer + kFooterSigOff, 64);

  // Cross-check: signedRegionLength + 96 should equal blobSize. Mismatch
  // means a truncated embed or a generator bug — refuse to enable.
  if (static_cast<size_t>(footer_.signedRegionLength) + kFooterLen != blobSize_) {
    Serial.printf("[wisp.fwcarrier] disabled: signedRegionLength(%u)+96 != blobSize(%u)\n",
                  (unsigned)footer_.signedRegionLength,
                  (unsigned)blobSize_);
    return false;
  }

  // Compute sha256 prefix over the signed region (NOT the footer). The
  // lamp side's verify path will re-hash and compare; offer/done echoes
  // need only the first 8 bytes.
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  // mbedTLS API: 0 = SHA-256 (not SHA-224). Returns 0 on success.
  if (mbedtls_sha256_starts(&ctx, 0) != 0 ||
      mbedtls_sha256_update(&ctx, blob_, footer_.signedRegionLength) != 0) {
    mbedtls_sha256_free(&ctx);
    Serial.println("[wisp.fwcarrier] disabled: sha256 compute failed");
    return false;
  }
  uint8_t digest[32];
  if (mbedtls_sha256_finish(&ctx, digest) != 0) {
    mbedtls_sha256_free(&ctx);
    Serial.println("[wisp.fwcarrier] disabled: sha256 finish failed");
    return false;
  }
  mbedtls_sha256_free(&ctx);
  std::memcpy(sha256Prefix_, digest, sizeof(sha256Prefix_));

  ready_ = true;
  Serial.printf("[wisp.fwcarrier] ready channel=%s version=0x%08lx "
                "blob=%u chunks=%u sha256=%02x%02x%02x%02x...\n",
                footer_.channel,
                (unsigned long)footer_.firmwareVersion,
                (unsigned)blobSize_,
                (unsigned)getTotalChunks(),
                sha256Prefix_[0], sha256Prefix_[1],
                sha256Prefix_[2], sha256Prefix_[3]);
  return true;
}

bool FirmwareCarrier::getSha256Prefix(uint8_t out[8]) const {
  if (!ready_ || !out) return false;
  std::memcpy(out, sha256Prefix_, 8);
  return true;
}

uint16_t FirmwareCarrier::getTotalChunks() const {
  if (!ready_) return 0;
  const size_t chunk = lamp_protocol::FW_CHUNK_SIZE;
  // ceil(blobSize / chunk). Overflow protection: blobSize <= ~1.4 MB and
  // chunk = 200, so total fits in uint16_t well below 0xFFFF.
  const size_t totalCeil = (blobSize_ + chunk - 1) / chunk;
  if (totalCeil > 0xFFFFu) return 0xFFFFu;
  return static_cast<uint16_t>(totalCeil);
}

size_t FirmwareCarrier::getChunk(uint16_t chunkIdx, uint8_t* out,
                                 size_t maxLen) const {
  if (!ready_ || !out) return 0;
  const size_t chunk = lamp_protocol::FW_CHUNK_SIZE;
  const size_t offset = static_cast<size_t>(chunkIdx) * chunk;
  if (offset >= blobSize_) return 0;
  const size_t remaining = blobSize_ - offset;
  const size_t n = remaining < chunk ? remaining : chunk;
  if (maxLen < n) return 0;
  std::memcpy(out, blob_ + offset, n);
  return n;
}

}  // namespace wisp
