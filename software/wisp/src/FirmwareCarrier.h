// FirmwareCarrier — owns the embedded signed lamp firmware blob and its
// parsed LSIG footer.
//
// Phase F. The blob lives in flash .rodata as `kEmbeddedFirmware[]`
// (defined in include/embedded_firmware.h, originally from the build-
// tooling `embed_firmware.py` pipeline; stub in dev until that lands).
//
// The signed blob is laid out as `signed_region || LSIG_footer(96 bytes)`.
// LSIG carries channel + version + signedRegionLength + signature; we
// validate the structural fields here (NOT the cryptographic signature —
// wisp doesn't carry the public key; lamps verify on receipt).
//
// Lifecycle: construct once globally; call begin() in setup(); read via
// the accessors. If begin() returns false (missing/invalid footer) the
// carrier permanently stays not-ready; FirmwareDistributor checks this
// and silently stays Disabled. Wisp continues palette work either way.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wisp {

// Parsed mirror of the 96-byte LSIG footer. Field widths + offsets match
// the CANONICAL layout pinned by software/lamp-os/src/components/firmware/
// firmware_signature.hpp (the lamp-side verify is the authority).
//
// Wire layout (96 bytes total):
//   magic(4)   = "LSIG"
//   channel(8) = zero-padded ASCII
//   firmwareVersion(4 LE)
//   signedRegionLength(4 LE)
//   reserved(12) = zeros
//   signature(64) = ed25519 raw over image[0..signedRegionLength)
struct SignedFirmwareFooter {
  uint8_t  magic[4];          // 'L' 'S' 'I' 'G'
  char     channel[9];        // 8 wire bytes + null
  uint32_t firmwareVersion;   // packed semver (major<<16)|(minor<<8)|patch
  uint32_t signedRegionLength;// bytes covered by sig (= blob size - 96)
  uint8_t  reserved[12];
  uint8_t  signature[64];     // ed25519 over the signed region
};

class FirmwareCarrier {
 public:
  // Locate the LSIG footer at the tail of the embedded blob, validate
  // magic + signedRegionLength, populate accessors. Returns true on
  // success; false if the embedded blob is missing the LSIG magic
  // (typical for stub builds before embed_firmware.py runs) or fails
  // any structural check. Idempotent: safe to call once.
  bool begin();

  bool isReady() const { return ready_; }

  // Packed semver of the carried firmware. Zero when !isReady().
  uint32_t getVersion() const { return ready_ ? footer_.firmwareVersion : 0u; }

  // 9-byte null-terminated channel string. When !isReady() returns ""
  // (the stable empty-string pointer below; not nullptr) so callers can
  // pass it to printf-family functions unconditionally.
  const char* getChannel() const { return ready_ ? footer_.channel : ""; }

  // Total bytes of the carried signed blob (signed_region + 96).
  // Zero when !isReady().
  size_t getBlobSize() const { return ready_ ? blobSize_ : 0u; }

  // First 8 bytes of sha256(signed_region) — image fingerprint used as
  // the FW_OFFER.sha256Prefix and FW_DONE.sha256Prefix echo.
  //
  // Computed lazily in begin() and cached. Output is 8 bytes; callers
  // pass a stable destination buffer. Returns true on success, false if
  // the carrier isn't ready.
  bool getSha256Prefix(uint8_t out[8]) const;

  // Convenience: ceil(blobSize / FW_CHUNK_SIZE). Zero when !isReady().
  uint16_t getTotalChunks() const;

  // Copy `[chunkIdx * FW_CHUNK_SIZE, next-200-or-tail)` into `out`.
  // Returns bytes written (==FW_CHUNK_SIZE for every chunk except
  // possibly the last). Returns 0 on out-of-range index, null buffer,
  // or insufficient buffer capacity, or when !isReady().
  size_t getChunk(uint16_t chunkIdx, uint8_t* out, size_t maxLen) const;

 private:
  bool ready_ = false;
  const uint8_t* blob_ = nullptr;
  size_t blobSize_ = 0;
  SignedFirmwareFooter footer_ = {};
  uint8_t sha256Prefix_[8] = {0};
};

}  // namespace wisp
