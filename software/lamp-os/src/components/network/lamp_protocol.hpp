#ifndef LAMP_PROTOCOL_H
#define LAMP_PROTOCOL_H

// Shared wire format for repeater <-> lamp over ESP-NOW broadcast.
// Header-only and identical between software/artnet-repeater/src/lamp_protocol.hpp
// and software/lamp-os/src/components/network/lamp_protocol.hpp — keep them in sync
// manually until either side is ready to live in a shared lib.

#include <cstdint>
#include <cstring>

namespace lamp_protocol {

constexpr uint8_t MAGIC_0 = 'L';
constexpr uint8_t MAGIC_1 = 'M';
constexpr uint8_t PROTOCOL_VERSION = 0x01;

enum MsgType : uint8_t {
  MSG_HELLO = 0x01,
  MSG_COLORS = 0x02,
  // Forwarded BLE control write. Payload is JSON tagged with a `char` field
  // naming the local control surface to invoke (brightness, shadeColors,
  // baseColors, knockout, expressionOp, settings, mqtt, ...). The local
  // pending-slot post functions handle the routing; downstream drain in
  // loop() runs unchanged.
  MSG_CONTROL_OP = 0x03,
};

constexpr size_t HEADER_SIZE = 6;
constexpr size_t COLORS_SIZE = 24;       // header(6) + payload(18)
constexpr size_t HELLO_FIXED_SIZE = 19;  // header(6) + sourceMac(6) + colors(8) — name length and bytes added on top
constexpr size_t HELLO_MAX_NAME = 32;
constexpr size_t HELLO_MAX_SIZE = HELLO_FIXED_SIZE + 1 + HELLO_MAX_NAME;  // +1 for name length byte

// MSG_CONTROL_OP frame: header(6) + targetMac(6) + sourceMac(6) + payloadLen(2) + payload(N).
// ESP-NOW max frame is 250 bytes; subtract the 20-byte fixed prefix.
constexpr size_t CONTROL_FIXED       = HEADER_SIZE + 6 + 6 + 2;
constexpr size_t CONTROL_MAX_PAYLOAD = 250 - CONTROL_FIXED;  // 230
constexpr size_t CONTROL_MAX_SIZE    = CONTROL_FIXED + CONTROL_MAX_PAYLOAD;

constexpr size_t MAX_PACKET_SIZE = CONTROL_MAX_SIZE > HELLO_MAX_SIZE
                                       ? CONTROL_MAX_SIZE
                                       : HELLO_MAX_SIZE;

struct ParsedColors {
  uint16_t seq;
  uint8_t targetMac[6];
  uint8_t shade[4];  // RGBW
  uint8_t base[4];   // RGBW
  uint8_t mode;
  uint8_t parameter;
};

struct ParsedHello {
  uint16_t seq;
  uint8_t sourceMac[6];
  uint8_t shade[4];
  uint8_t base[4];
  uint8_t nameLen;
  char name[HELLO_MAX_NAME + 1];  // null-terminated copy
};

struct ParsedControlOp {
  uint16_t seq;
  uint8_t targetMac[6];
  uint8_t sourceMac[6];
  uint16_t payloadLen;
  const uint8_t* payload;  // points into the recv buffer; caller must not retain past this call
};

// Build a COLORS frame into `buf`. Returns 0 on bad args, COLORS_SIZE on success.
inline size_t buildColors(uint8_t* buf, size_t bufLen, uint16_t seq,
                          const uint8_t targetMac[6],
                          const uint8_t shadeRGBW[4], const uint8_t baseRGBW[4],
                          uint8_t mode, uint8_t parameter) {
  if (!buf || bufLen < COLORS_SIZE || !targetMac || !shadeRGBW || !baseRGBW) return 0;
  buf[0] = MAGIC_0;
  buf[1] = MAGIC_1;
  buf[2] = PROTOCOL_VERSION;
  buf[3] = MSG_COLORS;
  buf[4] = static_cast<uint8_t>(seq & 0xFF);
  buf[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  std::memcpy(&buf[6], targetMac, 6);
  std::memcpy(&buf[12], shadeRGBW, 4);
  std::memcpy(&buf[16], baseRGBW, 4);
  buf[20] = mode;
  buf[21] = parameter;
  buf[22] = 0;
  buf[23] = 0;
  return COLORS_SIZE;
}

// Build a HELLO frame into `buf`. `name` is utf-8, NOT null-terminated on the wire.
// `nameLen` clamped to HELLO_MAX_NAME. Returns 0 on bad args, total bytes written on success.
inline size_t buildHello(uint8_t* buf, size_t bufLen, uint16_t seq,
                         const uint8_t sourceMac[6],
                         const uint8_t shadeRGBW[4], const uint8_t baseRGBW[4],
                         const char* name, size_t nameLen) {
  if (!buf || !sourceMac || !shadeRGBW || !baseRGBW) return 0;
  if (nameLen > HELLO_MAX_NAME) nameLen = HELLO_MAX_NAME;
  const size_t total = HELLO_FIXED_SIZE + 1 + nameLen;  // +1 for nameLen byte
  if (bufLen < total) return 0;
  buf[0] = MAGIC_0;
  buf[1] = MAGIC_1;
  buf[2] = PROTOCOL_VERSION;
  buf[3] = MSG_HELLO;
  buf[4] = static_cast<uint8_t>(seq & 0xFF);
  buf[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  std::memcpy(&buf[6], sourceMac, 6);
  std::memcpy(&buf[12], shadeRGBW, 4);
  std::memcpy(&buf[16], baseRGBW, 4);
  buf[19] = static_cast<uint8_t>(nameLen);
  if (nameLen && name) std::memcpy(&buf[20], name, nameLen);
  return total;
}

// Build a CONTROL_OP frame. Payload is opaque (JSON in practice). Returns
// total bytes written on success, 0 on bad args / oversize.
inline size_t buildControlOp(uint8_t* buf, size_t bufLen, uint16_t seq,
                             const uint8_t targetMac[6],
                             const uint8_t sourceMac[6],
                             const uint8_t* payload, size_t payloadLen) {
  if (!buf || !targetMac || !sourceMac) return 0;
  if (payloadLen > CONTROL_MAX_PAYLOAD) return 0;
  const size_t total = CONTROL_FIXED + payloadLen;
  if (bufLen < total) return 0;
  buf[0] = MAGIC_0;
  buf[1] = MAGIC_1;
  buf[2] = PROTOCOL_VERSION;
  buf[3] = MSG_CONTROL_OP;
  buf[4] = static_cast<uint8_t>(seq & 0xFF);
  buf[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  std::memcpy(&buf[6], targetMac, 6);
  std::memcpy(&buf[12], sourceMac, 6);
  buf[18] = static_cast<uint8_t>(payloadLen & 0xFF);
  buf[19] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  if (payloadLen && payload) std::memcpy(&buf[CONTROL_FIXED], payload, payloadLen);
  return total;
}

// Validate magic + version. Returns the msg type or 0 if invalid.
inline uint8_t inspect(const uint8_t* data, size_t len) {
  if (!data || len < HEADER_SIZE) return 0;
  if (data[0] != MAGIC_0 || data[1] != MAGIC_1) return 0;
  if (data[2] != PROTOCOL_VERSION) return 0;
  return data[3];
}

inline bool parseColors(const uint8_t* data, size_t len, ParsedColors& out) {
  if (inspect(data, len) != MSG_COLORS || len < COLORS_SIZE) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.targetMac, &data[6], 6);
  std::memcpy(out.shade, &data[12], 4);
  std::memcpy(out.base, &data[16], 4);
  out.mode = data[20];
  out.parameter = data[21];
  return true;
}

inline bool parseControlOp(const uint8_t* data, size_t len, ParsedControlOp& out) {
  if (inspect(data, len) != MSG_CONTROL_OP || len < CONTROL_FIXED) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.targetMac, &data[6], 6);
  std::memcpy(out.sourceMac, &data[12], 6);
  out.payloadLen = static_cast<uint16_t>(data[18]) | (static_cast<uint16_t>(data[19]) << 8);
  if (out.payloadLen > CONTROL_MAX_PAYLOAD) return false;
  if (len < CONTROL_FIXED + out.payloadLen) return false;
  out.payload = &data[CONTROL_FIXED];
  return true;
}

inline bool parseHello(const uint8_t* data, size_t len, ParsedHello& out) {
  if (inspect(data, len) != MSG_HELLO || len < HELLO_FIXED_SIZE + 1) return false;
  out.seq = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  std::memcpy(out.sourceMac, &data[6], 6);
  std::memcpy(out.shade, &data[12], 4);
  std::memcpy(out.base, &data[16], 4);
  const uint8_t rawLen = data[19];
  const uint8_t nameLen = rawLen > HELLO_MAX_NAME ? HELLO_MAX_NAME : rawLen;
  if (len < static_cast<size_t>(HELLO_FIXED_SIZE + 1 + nameLen)) return false;
  out.nameLen = nameLen;
  if (nameLen) std::memcpy(out.name, &data[20], nameLen);
  out.name[nameLen] = '\0';
  return true;
}

// Gossip dedup: small fixed-size ring tracking (sourceMac, msgType, seq) tuples
// seen recently. Drops duplicates so re-broadcasts terminate.
class DedupRing {
 public:
  static constexpr size_t CAPACITY = 32;

  // Returns true if (mac, msgType, seq) is new (and records it); false if seen.
  bool record(const uint8_t mac[6], uint8_t msgType, uint16_t seq) {
    for (size_t i = 0; i < CAPACITY; i++) {
      const Entry& e = entries_[i];
      if (e.used && e.msgType == msgType && e.seq == seq &&
          std::memcmp(e.mac, mac, 6) == 0) {
        return false;
      }
    }
    Entry& slot = entries_[head_];
    slot.used = true;
    slot.msgType = msgType;
    slot.seq = seq;
    std::memcpy(slot.mac, mac, 6);
    head_ = (head_ + 1) % CAPACITY;
    return true;
  }

 private:
  struct Entry {
    bool used = false;
    uint8_t msgType = 0;
    uint16_t seq = 0;
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  };
  Entry entries_[CAPACITY];
  size_t head_ = 0;
};

}  // namespace lamp_protocol

#endif
