#include "crypto.hpp"

#include <cstring>

#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

namespace lamp { namespace crypto {

namespace {

bool deriveKey(const uint8_t* salt, size_t saltLen,
               const char* info,
               const std::string& password,
               uint8_t outKey[32]) {
  const mbedtls_md_info_t* md =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) return false;
  // mbedtls_hkdf does Extract+Expand in one call. Output length is 32 (SHA-256).
  int rc = mbedtls_hkdf(
      md, salt, saltLen,
      reinterpret_cast<const uint8_t*>(password.data()), password.size(),
      reinterpret_cast<const uint8_t*>(info), std::strlen(info),
      outKey, 32);
  return rc == 0;
}

}  // namespace

bool isPrefixed(const uint8_t* p, size_t n) {
  return n >= 1 && (p[0] == MAGIC_PLAINTEXT || p[0] == MAGIC_CIPHERTEXT);
}

uint8_t magicByte(const uint8_t* p, size_t n) {
  return n ? p[0] : 0;
}

bool decryptOp(const uint8_t* p, size_t n,
               const uint8_t* uuidLE16, const char* name,
               const std::string& password,
               PerConnState& conn, std::string& out) {
  out.clear();
  if (n < 1 + NONCE_LEN + TAG_LEN) return false;
  if (p[0] != MAGIC_CIPHERTEXT) return false;
  if (password.empty()) return false;

  const uint8_t* nonce = p + 1;
  const uint8_t* tag = nonce + NONCE_LEN;
  const uint8_t* ct = tag + TAG_LEN;
  size_t ctLen = n - 1 - NONCE_LEN - TAG_LEN;

  // Reject replays within this connection.
  std::array<uint8_t, NONCE_LEN> nonceArr{};
  std::memcpy(nonceArr.data(), nonce, NONCE_LEN);
  for (const auto& seen : conn.recentNonces) {
    if (seen == nonceArr) return false;
  }

  uint8_t key[32];
  if (!deriveKey(uuidLE16, 16, name, password, key)) {
    std::memset(key, 0, sizeof(key));
    return false;
  }

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = false;
  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0) {
    out.resize(ctLen);
    int rc = mbedtls_gcm_auth_decrypt(
        &gcm, ctLen,
        nonce, NONCE_LEN,
        /*add=*/nullptr, 0,
        tag, TAG_LEN,
        ct,
        reinterpret_cast<uint8_t*>(&out[0]));
    ok = (rc == 0);
  }
  mbedtls_gcm_free(&gcm);
  std::memset(key, 0, sizeof(key));

  if (!ok) {
    out.clear();
    return false;
  }

  // Record the accepted nonce in the replay window.
  conn.recentNonces.push_back(nonceArr);
  if (conn.recentNonces.size() > MAX_RECENT_NONCES) {
    conn.recentNonces.pop_front();
  }
  return true;
}

}}  // namespace lamp::crypto
