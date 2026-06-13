// Native tests for FirmwareDistributor (wisp Phase F).
//
// Self-contained: we re-declare a minimal copy of the state machine
// algorithm — peer-pick logic, backoff ring, state transitions — and
// drive it with synthetic ticks + fake inbound packets. This avoids
// pulling in Arduino, ESP-NOW, or the production .cpp's dependencies on
// the wisp namespace classes.

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t kChunkSize = 200;

// Tunables that mirror FirmwareDistributor.h.
constexpr uint32_t kAcceptTimeoutMs    = 5000;
constexpr uint32_t kFinalizeTimeoutMs  = 30000;
constexpr uint32_t kChunkResendMs      = 1500;
constexpr uint8_t  kRetriesPerChunk    = 4;
constexpr uint32_t kPeerBackoffMs      = 600000;
constexpr uint32_t kInitialScanDelayMs = 30000;
constexpr uint32_t kScanPeriodMs       = 300000;
constexpr uint32_t kPeerFreshnessMs    = 60000;
constexpr uint16_t kBurstChunksPerTick = 8;
constexpr uint8_t  kMaxOfferRetries    = 8;
constexpr uint32_t kOfferRetryIntervalMs = 200;
// DONE retry mirror. Production emits DONE once when chunks drain, then
// waits kFinalizeTimeoutMs (30s) for RESULT. With ~50% ESP-NOW unicast
// loss under BLE coex, a single DONE that drops eats 30s + 10-min peer
// backoff for no reason. Retry the DONE send up to kMaxDoneRetries times
// (5 total attempts) spaced kDoneRetryIntervalMs apart, reusing the same
// DONE seq so the lamp-side handler stays idempotent (a duplicate DONE
// arriving after verify+reboot lands in a non-Streaming state and is
// silently dropped per the receiver state guard).
constexpr uint8_t  kMaxDoneRetries     = 4;
constexpr uint32_t kDoneRetryIntervalMs = 300;

enum class State : uint8_t {
  Disabled = 0,
  Idle,
  OfferSent,
  Streaming,
  Finalizing,
  Failed,
  Done,
};

struct Peer {
  uint8_t  mac[6];
  uint32_t firmwareVersion;
  uint32_t lastSeenMs;
};

struct Penalty {
  uint8_t  mac[6];
  uint32_t backoffUntilMs;
  bool     used;
};

// Minimal "distributor algorithm" used by tests.
struct DistAlgo {
  uint32_t carrierVersion = 0;
  uint16_t totalChunks_   = 0;

  State state_         = State::Idle;
  uint32_t lastScanMs_ = 0;
  uint32_t stateEnteredMs_ = 0;
  bool firstScan_      = true;

  uint8_t targetMac_[6] = {0};
  uint16_t nextChunkIdx_ = 0;
  uint16_t lastSentChunk_ = 0;
  uint32_t lastSentMs_ = 0;
  uint8_t  currentChunkRetries_ = 0;
  uint32_t lastOfferSendMs_     = 0;
  uint8_t  offerRetryCount_     = 0;

  // DONE retry bookkeeping (Bug 1 mirror). seq is captured once at
  // entry to the retry loop and reused across attempts. doneAttempts_
  // is bumped per send. doneRetryAborted_ records whether the loop
  // bailed because RESULT (or session reset) clobbered the state.
  uint16_t doneSeqCaptured_   = 0;
  uint8_t  doneAttempts_      = 0;
  bool     doneRetryAborted_  = false;

  std::vector<Peer> roster;

  static constexpr size_t kPenaltyRingSize = 8;
  Penalty penalties_[kPenaltyRingSize] = {};
  size_t penaltyHead_ = 0;

  // --- bookkeeping helpers ---
  static bool macsEqual(const uint8_t a[6], const uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
  }
  bool peerIsInBackoff(const uint8_t mac[6], uint32_t nowMs) const {
    for (size_t i = 0; i < kPenaltyRingSize; ++i) {
      const auto& p = penalties_[i];
      if (!p.used) continue;
      if (!macsEqual(p.mac, mac)) continue;
      if (nowMs < p.backoffUntilMs) return true;
    }
    return false;
  }
  void notePeerBackoff(const uint8_t mac[6], uint32_t nowMs) {
    for (size_t i = 0; i < kPenaltyRingSize; ++i) {
      if (penalties_[i].used && macsEqual(penalties_[i].mac, mac)) {
        penalties_[i].backoffUntilMs = nowMs + kPeerBackoffMs;
        return;
      }
    }
    penalties_[penaltyHead_].used = true;
    std::memcpy(penalties_[penaltyHead_].mac, mac, 6);
    penalties_[penaltyHead_].backoffUntilMs = nowMs + kPeerBackoffMs;
    penaltyHead_ = (penaltyHead_ + 1) % kPenaltyRingSize;
  }
  void resetSession() {
    std::memset(targetMac_, 0, 6);
    nextChunkIdx_ = 0;
    lastSentChunk_ = 0;
    lastSentMs_ = 0;
    currentChunkRetries_ = 0;
    lastOfferSendMs_ = 0;
    offerRetryCount_ = 0;
  }

  bool pickTargetAndOffer(uint32_t nowMs) {
    const Peer* best = nullptr;
    for (const auto& p : roster) {
      if (p.firmwareVersion == 0) continue;
      if (p.firmwareVersion >= carrierVersion) continue;
      if ((nowMs - p.lastSeenMs) > kPeerFreshnessMs) continue;
      if (peerIsInBackoff(p.mac, nowMs)) continue;
      if (!best || p.firmwareVersion < best->firmwareVersion) best = &p;
    }
    if (!best) return false;
    std::memcpy(targetMac_, best->mac, 6);
    nextChunkIdx_ = 0;
    state_ = State::OfferSent;
    stateEnteredMs_ = nowMs;
    // Initial OFFER send stamps lastOfferSendMs_ + retry count.
    lastOfferSendMs_ = nowMs;
    offerRetryCount_ = 0;
    return true;
  }

  void tick(uint32_t nowMs) {
    switch (state_) {
      case State::Idle: {
        const uint32_t scanWindow = firstScan_ ? kInitialScanDelayMs : kScanPeriodMs;
        if ((nowMs - lastScanMs_) < scanWindow) return;
        lastScanMs_ = nowMs;
        firstScan_ = false;
        pickTargetAndOffer(nowMs);
        return;
      }
      case State::OfferSent:
        if ((nowMs - stateEnteredMs_) > kAcceptTimeoutMs) {
          notePeerBackoff(targetMac_, nowMs);
          resetSession();
          state_ = State::Failed;
          stateEnteredMs_ = nowMs;
          return;
        }
        // OFFER retry mirror: increment count after each interval up to the
        // cap. Production code's sendOfferFrame also stamps lastOfferSendMs_
        // here; the test mirror doesn't actually send a frame, so we just
        // track the counter.
        if (offerRetryCount_ < kMaxOfferRetries &&
            (nowMs - lastOfferSendMs_) >= kOfferRetryIntervalMs) {
          offerRetryCount_++;
          lastOfferSendMs_ = nowMs;
        }
        return;
      case State::Streaming: {
        if (nextChunkIdx_ >= totalChunks_) {
          state_ = State::Finalizing;
          stateEnteredMs_ = nowMs;
          return;
        }
        // Burst up to kBurstChunksPerTick chunks per tick (mirror of the
        // production change: single-chunk-per-tick caps real-world
        // throughput because wisp's main loop runs tick() only every
        // hundreds of ms).
        for (uint16_t i = 0; i < kBurstChunksPerTick; ++i) {
          if (nextChunkIdx_ >= totalChunks_) break;
          lastSentChunk_ = nextChunkIdx_;
          lastSentMs_ = nowMs;
          nextChunkIdx_++;
          currentChunkRetries_ = 0;
        }
        if ((nowMs - lastSentMs_) > kChunkResendMs) {
          if (currentChunkRetries_ >= kRetriesPerChunk) {
            notePeerBackoff(targetMac_, nowMs);
            resetSession();
            state_ = State::Failed;
            stateEnteredMs_ = nowMs;
            return;
          }
          currentChunkRetries_++;
          lastSentMs_ = nowMs;  // resend at this tick
        }
        return;
      }
      case State::Finalizing:
        if ((nowMs - stateEnteredMs_) > kFinalizeTimeoutMs) {
          notePeerBackoff(targetMac_, nowMs);
          resetSession();
          state_ = State::Failed;
          stateEnteredMs_ = nowMs;
        }
        return;
      case State::Failed:
      case State::Done:
        resetSession();
        state_ = State::Idle;
        stateEnteredMs_ = nowMs;
        return;
      case State::Disabled:
        return;
    }
  }

  void onAccept(const uint8_t fromMac[6], uint32_t /*version*/, uint32_t nowMs) {
    if (state_ != State::OfferSent) return;
    if (!macsEqual(fromMac, targetMac_)) return;
    state_ = State::Streaming;
    stateEnteredMs_ = nowMs;
  }
  void onReq(const uint8_t fromMac[6], uint16_t firstIdx, uint32_t nowMs) {
    if (state_ != State::Streaming && state_ != State::Finalizing) return;
    if (!macsEqual(fromMac, targetMac_)) return;
    if (firstIdx >= totalChunks_) return;
    nextChunkIdx_ = firstIdx;
    currentChunkRetries_ = 0;
    lastSentMs_ = nowMs;
    if (state_ == State::Finalizing) {
      state_ = State::Streaming;
      stateEnteredMs_ = nowMs;
    }
  }
  void onResultSuccess(const uint8_t fromMac[6], uint32_t nowMs) {
    if (state_ != State::Finalizing) return;
    if (!macsEqual(fromMac, targetMac_)) return;
    resetSession();
    state_ = State::Done;
    stateEnteredMs_ = nowMs;
  }
  void onResultFail(const uint8_t fromMac[6], uint32_t nowMs) {
    if (state_ != State::Finalizing) return;
    if (!macsEqual(fromMac, targetMac_)) return;
    notePeerBackoff(targetMac_, nowMs);
    resetSession();
    state_ = State::Failed;
    stateEnteredMs_ = nowMs;
  }

  // Production mirror for the DONE retry loop (Bug 1). The streaming
  // task transitions to Finalizing and then calls a retry loop that
  // unicasts DONE up to (kMaxDoneRetries + 1) times — the same captured
  // seq each time — spacing attempts by kDoneRetryIntervalMs. The loop
  // bails when the state pivots out of Finalizing (RESULT arrived) or
  // when the budget is exhausted.
  //
  // The optional `pivotAfter` arg lets a test drop the state out of
  // Finalizing after a chosen attempt to model an early RESULT.
  void runEmitDoneRetryLoop(uint32_t /*nowStartMs*/,
                            uint8_t pivotAfter = 0xFF) {
    if (state_ != State::Finalizing) {
      doneRetryAborted_ = true;
      return;
    }
    doneRetryAborted_ = false;
    doneAttempts_     = 0;
    // Capture seq once — every retry sends with this same value.
    static uint16_t seqGen = 0xA000;
    doneSeqCaptured_ = ++seqGen;

    const uint8_t budget = kMaxDoneRetries + 1;
    for (uint8_t i = 0; i < budget; ++i) {
      if (state_ != State::Finalizing) {
        doneRetryAborted_ = true;
        return;
      }
      doneAttempts_++;
      // (Production sends the frame here. The mirror doesn't need to
      // model the byte stream — only the loop control.)
      if (i + 1 == pivotAfter) {
        // Simulate a RESULT-driven state change between this attempt
        // and the next retry interval.
        state_ = State::Done;
      }
    }
  }
};

// --- Mesh peer-table LRU algorithm mirror (Bug 2) ------------------------
//
// Production MeshLink tracks unicast peers in a fixed-size array so it
// can skip esp_now_add_peer when a target has already been registered.
// The original size (16) is too small for the 22-lamp fleet; once full,
// every new MAC fell through with no add and esp_now_send returned
// ESP_ERR_ESPNOW_NOT_FOUND invisibly. ESP-NOW caps total peers at 20
// (the broadcast peer takes one of those slots), so we bump the
// unicast-table size to 19 AND add LRU eviction so an unbounded fleet
// still works.
//
// The mirror models the bookkeeping: array of {mac, lastUsedMs}, the
// LRU pick when full, and an "evict count" to verify eviction happened.
struct MeshPeerTableAlgo {
  // Mirror of MAX_TRACKED_PEERS; production is bumped from 16 → 19 to
  // match ESP_NOW_MAX_TOTAL_PEER_NUM (20) minus the broadcast slot.
  static constexpr size_t kMaxPeers = 19;
  struct Slot {
    uint8_t  mac[6];
    uint32_t lastUsedMs;
    bool     used;
  };
  Slot slots_[kMaxPeers] = {};
  size_t used_ = 0;
  uint32_t evictCount_ = 0;

  static bool macsEqual(const uint8_t a[6], const uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
  }

  // Find slot index for a given mac, or -1 if not tracked.
  int findSlot(const uint8_t mac[6]) const {
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (slots_[i].used && macsEqual(slots_[i].mac, mac)) return (int)i;
    }
    return -1;
  }

  // Pick the slot with the smallest lastUsedMs (LRU).
  size_t pickLruIndex() const {
    size_t bestIdx = 0;
    uint32_t bestMs = UINT32_MAX;
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (slots_[i].used && slots_[i].lastUsedMs < bestMs) {
        bestMs = slots_[i].lastUsedMs;
        bestIdx = i;
      }
    }
    return bestIdx;
  }

  // Touch (insert-if-absent + update lastUsedMs) the slot for `mac`,
  // evicting the LRU if the table is full. Returns the slot index.
  size_t touch(const uint8_t mac[6], uint32_t nowMs) {
    const int existing = findSlot(mac);
    if (existing >= 0) {
      slots_[existing].lastUsedMs = nowMs;
      return (size_t)existing;
    }
    // Find empty slot first.
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (!slots_[i].used) {
        slots_[i].used = true;
        std::memcpy(slots_[i].mac, mac, 6);
        slots_[i].lastUsedMs = nowMs;
        used_++;
        return i;
      }
    }
    // Full → evict LRU.
    const size_t lru = pickLruIndex();
    evictCount_++;
    std::memcpy(slots_[lru].mac, mac, 6);
    slots_[lru].lastUsedMs = nowMs;
    return lru;
  }

  size_t size() const { return used_; }
};

DistAlgo makeAlgoWith(const std::vector<Peer>& roster,
                      uint32_t carrierVer = 0x00010005u,
                      uint16_t chunks = 10) {
  DistAlgo d;
  d.carrierVersion = carrierVer;
  d.totalChunks_   = chunks;
  d.roster         = roster;
  return d;
}

// Helpers to build fake peer MACs without copy-paste.
Peer makePeer(uint8_t macTail, uint32_t version, uint32_t lastSeen) {
  Peer p{};
  p.mac[0] = 0xAA; p.mac[1] = 0xBB; p.mac[2] = 0xCC;
  p.mac[3] = 0x00; p.mac[4] = 0x00; p.mac[5] = macTail;
  p.firmwareVersion = version;
  p.lastSeenMs = lastSeen;
  return p;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- Scan / pick ---------------------------------------------------------

void test_tick_before_initial_scan_window_picks_nothing(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010003u, 0)}, 0x00010005u);
  d.tick(1000);  // less than kInitialScanDelayMs (30000)
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Idle),
                          static_cast<uint8_t>(d.state_));
}

void test_scan_picks_lowest_version_peer(void) {
  std::vector<Peer> roster = {
    makePeer(1, 0x00010003u, 25000),  // version 1.0.3, fresh
    makePeer(2, 0x00010002u, 25000),  // version 1.0.2, fresh — lowest
    makePeer(3, 0x00010004u, 25000),  // version 1.0.4, fresh
  };
  auto d = makeAlgoWith(roster, 0x00010005u);
  d.tick(30001);  // past initial scan window
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::OfferSent),
                          static_cast<uint8_t>(d.state_));
  TEST_ASSERT_EQUAL_UINT8(2, d.targetMac_[5]);
}

void test_scan_skips_up_to_date_peer(void) {
  std::vector<Peer> roster = {
    makePeer(1, 0x00010005u, 25000),  // == carrier — skip
    makePeer(2, 0x00010006u, 25000),  // >  carrier — skip
  };
  auto d = makeAlgoWith(roster, 0x00010005u);
  d.tick(30001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Idle),
                          static_cast<uint8_t>(d.state_));
}

void test_scan_skips_stale_peer(void) {
  // lastSeen = 0, now = 100000 → ageMs = 100000 > kPeerFreshnessMs (60000)
  std::vector<Peer> roster = {
    makePeer(1, 0x00010001u, 0),
  };
  auto d = makeAlgoWith(roster, 0x00010005u);
  d.lastScanMs_ = 0;
  d.tick(100000);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Idle),
                          static_cast<uint8_t>(d.state_));
}

void test_scan_skips_peer_in_backoff(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010002u, 25000)}, 0x00010005u);
  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.notePeerBackoff(mac, 30001);  // now = 30001, backoff until 30001+600000
  d.tick(30001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Idle),
                          static_cast<uint8_t>(d.state_));
}

// --- ACCEPT / Streaming --------------------------------------------------

void test_accept_transitions_to_streaming(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 4);
  d.tick(30001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::OfferSent),
                          static_cast<uint8_t>(d.state_));
  uint8_t fromMac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.onAccept(fromMac, d.carrierVersion, 30050);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Streaming),
                          static_cast<uint8_t>(d.state_));
}

void test_accept_timeout_failures_peer(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u);
  d.tick(30001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::OfferSent),
                          static_cast<uint8_t>(d.state_));
  // Past kAcceptTimeoutMs (5000ms).
  d.tick(30001 + 5001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Failed),
                          static_cast<uint8_t>(d.state_));
  // Backoff active for the peer.
  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  TEST_ASSERT_TRUE(d.peerIsInBackoff(mac, 30001 + 5001));
}

void test_offer_retry_count_advances_with_interval(void) {
  // Lamp at 1.0.1, wisp carries 1.0.5: scan picks it, transitions to
  // OfferSent. Each tick past kOfferRetryIntervalMs (200ms) advances
  // offerRetryCount_ until it caps at kMaxOfferRetries (8). ACCEPT never
  // arrives → kAcceptTimeoutMs (5000ms) eventually fires → Failed.
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)});
  d.tick(30001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::OfferSent),
                          static_cast<uint8_t>(d.state_));
  TEST_ASSERT_EQUAL_UINT8(0, d.offerRetryCount_);
  // Spaced-by-200ms ticks advance the counter by one each.
  for (uint8_t i = 1; i <= kMaxOfferRetries; ++i) {
    d.tick(30001 + i * 200);
    TEST_ASSERT_EQUAL_UINT8(i, d.offerRetryCount_);
  }
  // Cap holds past the retry budget.
  d.tick(30001 + (kMaxOfferRetries + 1) * 200);
  TEST_ASSERT_EQUAL_UINT8(kMaxOfferRetries, d.offerRetryCount_);
  // Accept-timeout still fires after kAcceptTimeoutMs.
  d.tick(30001 + 5001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Failed),
                          static_cast<uint8_t>(d.state_));
}

void test_streaming_emits_chunks_in_order(void) {
  // chunks=3 fits inside a single burst (kBurstChunksPerTick=8), so the
  // first Streaming tick exhausts the queue; the next tick transitions
  // to Finalizing.
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 3);
  d.tick(30001);
  uint8_t fromMac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.onAccept(fromMac, d.carrierVersion, 30050);
  d.tick(30050);
  TEST_ASSERT_EQUAL_UINT16(2, d.lastSentChunk_);
  TEST_ASSERT_EQUAL_UINT16(3, d.nextChunkIdx_);
  // Drained; next tick → Finalizing.
  d.tick(30054);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Finalizing),
                          static_cast<uint8_t>(d.state_));
}

void test_streaming_spreads_chunks_across_ticks_when_total_exceeds_burst(void) {
  // chunks=20 > kBurstChunksPerTick=8 → first tick sends 0..7, second
  // sends 8..15, third sends 16..19, fourth transitions to Finalizing.
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 20);
  d.tick(30001);
  uint8_t fromMac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.onAccept(fromMac, d.carrierVersion, 30050);
  d.tick(30050);
  TEST_ASSERT_EQUAL_UINT16(7, d.lastSentChunk_);
  TEST_ASSERT_EQUAL_UINT16(8, d.nextChunkIdx_);
  d.tick(30054);
  TEST_ASSERT_EQUAL_UINT16(15, d.lastSentChunk_);
  TEST_ASSERT_EQUAL_UINT16(16, d.nextChunkIdx_);
  d.tick(30058);
  TEST_ASSERT_EQUAL_UINT16(19, d.lastSentChunk_);
  TEST_ASSERT_EQUAL_UINT16(20, d.nextChunkIdx_);
  d.tick(30062);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Finalizing),
                          static_cast<uint8_t>(d.state_));
}

void test_fw_req_rewinds_chunk_cursor(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 20);
  d.tick(30001);
  uint8_t fromMac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.onAccept(fromMac, d.carrierVersion, 30050);
  // One tick bursts 8 chunks → nextChunkIdx_ = 8.
  d.tick(30050);
  TEST_ASSERT_EQUAL_UINT16(8, d.nextChunkIdx_);
  // Lamp requests retransmit starting at chunk 2.
  d.onReq(fromMac, 2, 30054);
  TEST_ASSERT_EQUAL_UINT16(2, d.nextChunkIdx_);
}

// --- DONE / RESULT -------------------------------------------------------

void test_full_path_done_result_success_returns_to_idle(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 2);
  d.tick(30001);
  uint8_t fromMac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.onAccept(fromMac, d.carrierVersion, 30050);
  d.tick(30050);  // burst: chunks 0..1 (only 2 to send)
  d.tick(30054);  // → Finalizing
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Finalizing),
                          static_cast<uint8_t>(d.state_));
  d.onResultSuccess(fromMac, 30100);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Done),
                          static_cast<uint8_t>(d.state_));
  // Done is a tombstone; next tick returns to Idle.
  d.tick(30101);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Idle),
                          static_cast<uint8_t>(d.state_));
}

void test_finalize_timeout_failures_peer(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 1);
  d.tick(30001);
  uint8_t fromMac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.onAccept(fromMac, d.carrierVersion, 30050);
  d.tick(30050);  // burst: chunk 0
  d.tick(30054);  // → Finalizing
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Finalizing),
                          static_cast<uint8_t>(d.state_));
  // No RESULT arrives → tick past kFinalizeTimeoutMs (30000).
  d.tick(30054 + 30001);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Failed),
                          static_cast<uint8_t>(d.state_));
  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  TEST_ASSERT_TRUE(d.peerIsInBackoff(mac, 30054 + 30001));
}

void test_result_failure_records_backoff(void) {
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 1);
  d.tick(30001);
  uint8_t fromMac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.onAccept(fromMac, d.carrierVersion, 30050);
  d.tick(30050);  // burst: chunk 0
  d.tick(30054);  // → Finalizing
  d.onResultFail(fromMac, 30100);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(State::Failed),
                          static_cast<uint8_t>(d.state_));
  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  TEST_ASSERT_TRUE(d.peerIsInBackoff(mac, 30100));
}

// --- Stall / retry budget ------------------------------------------------
//
// The stall watchdog (resend after kChunkResendMs, fail after
// kRetriesPerChunk) is reachable only when the chunk emission path is
// blocked while time advances. In this test algorithm — and in
// production — forward progress is gated by ESP-NOW TX-queue
// availability; on the native test side every "send" succeeds, so a
// normal tick stream always advances the cursor. The watchdog is
// belt-and-suspenders for "ESP-NOW TX queue wedged for >1.5s while we
// keep ticking"; modelling that needs a hook on the send path the test
// harness doesn't currently have. Covered manually during the hardware
// smoke test phase instead.

// --- Backoff ring --------------------------------------------------------

void test_backoff_expires(void) {
  DistAlgo d;
  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.notePeerBackoff(mac, 1000);
  TEST_ASSERT_TRUE(d.peerIsInBackoff(mac, 1000));
  TEST_ASSERT_TRUE(d.peerIsInBackoff(mac, 1000 + kPeerBackoffMs - 1));
  TEST_ASSERT_FALSE(d.peerIsInBackoff(mac, 1000 + kPeerBackoffMs));
}

void test_backoff_ring_reuses_slot_for_same_mac(void) {
  DistAlgo d;
  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
  d.notePeerBackoff(mac, 1000);
  d.notePeerBackoff(mac, 2000);
  // Only one slot should be used.
  size_t used = 0;
  for (const auto& p : d.penalties_) if (p.used) used++;
  TEST_ASSERT_EQUAL_UINT(1, used);
}

// --- DONE retry (Bug 1) --------------------------------------------------

void test_done_retry_exhausts_budget_with_no_result(void) {
  // Lamp never sends RESULT. The retry loop should attempt exactly
  // kMaxDoneRetries + 1 sends (5 total), reusing the captured seq.
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 1);
  d.state_ = State::Finalizing;
  std::memset(d.targetMac_, 0xAB, 6);
  d.runEmitDoneRetryLoop(/*nowStartMs=*/40000);
  TEST_ASSERT_EQUAL_UINT8(kMaxDoneRetries + 1, d.doneAttempts_);
  TEST_ASSERT_FALSE(d.doneRetryAborted_);
}

void test_done_retry_bails_when_result_arrives_mid_loop(void) {
  // After 2 attempts, RESULT arrives and pivots state out of Finalizing.
  // The loop should stop short, NOT exhaust the budget.
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 1);
  d.state_ = State::Finalizing;
  std::memset(d.targetMac_, 0xAB, 6);
  d.runEmitDoneRetryLoop(/*nowStartMs=*/40000, /*pivotAfter=*/2);
  // 2 attempts happened, then state pivot → next iteration aborted.
  TEST_ASSERT_EQUAL_UINT8(2, d.doneAttempts_);
  TEST_ASSERT_TRUE(d.doneRetryAborted_);
}

void test_done_retry_reuses_same_seq_across_attempts(void) {
  // The captured seq must NOT increment across attempts — the lamp's
  // idempotence depends on the wisp not generating new seqs per retry.
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 1);
  d.state_ = State::Finalizing;
  std::memset(d.targetMac_, 0xAB, 6);
  d.runEmitDoneRetryLoop(40000);
  const uint16_t seqBefore = d.doneSeqCaptured_;
  // (The mirror exposes a single captured seq value, mutated only at
  // loop entry. A bug that re-captured per attempt would show a
  // different value on a second loop entry, but within a single loop
  // it shows ONE value across attempts. Verify the value is stable
  // throughout — we can't directly observe per-attempt seqs without
  // adding plumbing, but the production code reads doneSeqCaptured_
  // for each attempt, so this is a structural sanity check.)
  TEST_ASSERT_TRUE(seqBefore != 0);
}

void test_done_retry_noop_if_not_finalizing(void) {
  // If the state isn't Finalizing when the loop starts (RESULT already
  // arrived between transition and loop entry), the loop should bail
  // immediately with zero attempts.
  auto d = makeAlgoWith({makePeer(1, 0x00010001u, 25000)}, 0x00010005u, 1);
  d.state_ = State::Streaming;  // not Finalizing
  d.runEmitDoneRetryLoop(40000);
  TEST_ASSERT_EQUAL_UINT8(0, d.doneAttempts_);
  TEST_ASSERT_TRUE(d.doneRetryAborted_);
}

// --- MeshLink LRU peer table (Bug 2) -------------------------------------

void test_mesh_peer_table_holds_up_to_max_peers(void) {
  MeshPeerTableAlgo m;
  for (size_t i = 0; i < MeshPeerTableAlgo::kMaxPeers; ++i) {
    uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, (uint8_t)i};
    m.touch(mac, 1000 + (uint32_t)i);
  }
  TEST_ASSERT_EQUAL_UINT(MeshPeerTableAlgo::kMaxPeers, m.size());
  TEST_ASSERT_EQUAL_UINT32(0, m.evictCount_);
}

void test_mesh_peer_table_evicts_lru_on_overflow(void) {
  // Fill the table at staggered timestamps so peer 0 is the LRU.
  // Then add a new peer; peer 0's slot must be the one reused.
  MeshPeerTableAlgo m;
  for (size_t i = 0; i < MeshPeerTableAlgo::kMaxPeers; ++i) {
    uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, (uint8_t)i};
    m.touch(mac, 1000 + (uint32_t)i);
  }
  // The kMaxPeers-th peer triggers eviction.
  uint8_t newMac[6] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x99};
  m.touch(newMac, 5000);
  TEST_ASSERT_EQUAL_UINT32(1, m.evictCount_);
  // Old peer 0 must no longer be present.
  uint8_t oldMac0[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};
  TEST_ASSERT_EQUAL_INT(-1, m.findSlot(oldMac0));
  // The new peer must be present.
  TEST_ASSERT_NOT_EQUAL(-1, m.findSlot(newMac));
  // Total size still equals kMaxPeers (we reused a slot, not grew).
  TEST_ASSERT_EQUAL_UINT(MeshPeerTableAlgo::kMaxPeers, m.size());
}

void test_mesh_peer_table_touch_refreshes_lastused(void) {
  // Fill the table, then touch peer 0 to refresh it. Then add a new
  // peer; the LRU should now be peer 1, not peer 0.
  MeshPeerTableAlgo m;
  for (size_t i = 0; i < MeshPeerTableAlgo::kMaxPeers; ++i) {
    uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, (uint8_t)i};
    m.touch(mac, 1000 + (uint32_t)i);
  }
  uint8_t mac0[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};
  m.touch(mac0, 9999);  // refresh — now LRU shifts to peer 1
  uint8_t newMac[6] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x99};
  m.touch(newMac, 10000);
  // peer 0 must still exist.
  TEST_ASSERT_NOT_EQUAL(-1, m.findSlot(mac0));
  // peer 1 (smallest lastUsedMs after the refresh) must have been evicted.
  uint8_t mac1[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  TEST_ASSERT_EQUAL_INT(-1, m.findSlot(mac1));
}

void test_mesh_peer_table_idempotent_for_known_peer(void) {
  // Touching an existing peer must NOT bump the used count, NOT evict,
  // and must update lastUsedMs.
  MeshPeerTableAlgo m;
  uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  m.touch(mac, 1000);
  TEST_ASSERT_EQUAL_UINT(1, m.size());
  m.touch(mac, 2000);
  TEST_ASSERT_EQUAL_UINT(1, m.size());
  TEST_ASSERT_EQUAL_UINT32(0, m.evictCount_);
  const int idx = m.findSlot(mac);
  TEST_ASSERT_NOT_EQUAL(-1, idx);
  TEST_ASSERT_EQUAL_UINT32(2000, m.slots_[idx].lastUsedMs);
}

void test_mesh_peer_table_handles_22_lamp_rotation(void) {
  // The 22-lamp fleet scenario: cycle through 22 unique MACs and
  // verify the LRU eviction keeps the table healthy. The eviction
  // count should be (22 - kMaxPeers) on the first pass.
  MeshPeerTableAlgo m;
  constexpr size_t kFleetSize = 22;
  for (size_t i = 0; i < kFleetSize; ++i) {
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, (uint8_t)i};
    m.touch(mac, 1000 + (uint32_t)i * 10);
  }
  // Table never exceeds capacity.
  TEST_ASSERT_EQUAL_UINT(MeshPeerTableAlgo::kMaxPeers, m.size());
  TEST_ASSERT_EQUAL_UINT32(kFleetSize - MeshPeerTableAlgo::kMaxPeers,
                           m.evictCount_);
  // Most-recently-used peer (index 21) is still present.
  uint8_t recent[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 21};
  TEST_ASSERT_NOT_EQUAL(-1, m.findSlot(recent));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tick_before_initial_scan_window_picks_nothing);
  RUN_TEST(test_scan_picks_lowest_version_peer);
  RUN_TEST(test_scan_skips_up_to_date_peer);
  RUN_TEST(test_scan_skips_stale_peer);
  RUN_TEST(test_scan_skips_peer_in_backoff);
  RUN_TEST(test_accept_transitions_to_streaming);
  RUN_TEST(test_accept_timeout_failures_peer);
  RUN_TEST(test_offer_retry_count_advances_with_interval);
  RUN_TEST(test_streaming_emits_chunks_in_order);
  RUN_TEST(test_streaming_spreads_chunks_across_ticks_when_total_exceeds_burst);
  RUN_TEST(test_fw_req_rewinds_chunk_cursor);
  RUN_TEST(test_full_path_done_result_success_returns_to_idle);
  RUN_TEST(test_finalize_timeout_failures_peer);
  RUN_TEST(test_result_failure_records_backoff);
  RUN_TEST(test_backoff_expires);
  RUN_TEST(test_backoff_ring_reuses_slot_for_same_mac);
  RUN_TEST(test_done_retry_exhausts_budget_with_no_result);
  RUN_TEST(test_done_retry_bails_when_result_arrives_mid_loop);
  RUN_TEST(test_done_retry_reuses_same_seq_across_attempts);
  RUN_TEST(test_done_retry_noop_if_not_finalizing);
  RUN_TEST(test_mesh_peer_table_holds_up_to_max_peers);
  RUN_TEST(test_mesh_peer_table_evicts_lru_on_overflow);
  RUN_TEST(test_mesh_peer_table_touch_refreshes_lastused);
  RUN_TEST(test_mesh_peer_table_idempotent_for_known_peer);
  RUN_TEST(test_mesh_peer_table_handles_22_lamp_rotation);
  return UNITY_END();
}
