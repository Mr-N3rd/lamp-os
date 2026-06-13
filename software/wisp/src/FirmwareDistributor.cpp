#include "FirmwareDistributor.h"

#include <Arduino.h>

#include <cstring>

#include "FirmwareCarrier.h"
#include "LampInventory.h"
#include "MeshLink.h"
#include "lamp_protocol.hpp"

namespace wisp {

namespace {

inline bool macsEqual(const uint8_t a[6], const uint8_t b[6]) {
  return std::memcmp(a, b, 6) == 0;
}

// LSIG footer size in v1. lamp_protocol.hpp doesn't expose a named
// constant because the protocol field is just a uint16_t the receiver
// trusts at face value; we hardcode the v1 value here since the carrier
// always emits a 96-byte footer.
constexpr uint16_t kFwFooterLenV1 = 96;

}  // namespace

void FirmwareDistributor::begin(MeshLink* mesh, FirmwareCarrier* carrier,
                                LampInventory* inventory) {
  mesh_      = mesh;
  carrier_   = carrier;
  inventory_ = inventory;

  if (!mesh_ || !carrier_ || !inventory_ || !carrier_->isReady()) {
    state_ = State::Disabled;
    Serial.println("[wisp.fwdist] disabled (carrier not ready or deps missing)");
    return;
  }

  state_         = State::Idle;
  lastScanMs_    = millis();
  firstScan_     = true;
  stateEnteredMs_ = lastScanMs_;

#if defined(ARDUINO) || defined(ESP_PLATFORM)
  // Cache the wisp's MAC once — getMac calls esp_wifi_get_mac under the
  // hood and the streaming task would otherwise hammer it per chunk.
  mesh_->getMac(cachedSrcMac_);
  // Wake semaphore — binary, initially empty. The first transition into
  // OfferSent (in pickTargetAndOffer) gives it, kicking off the task.
  wakeSem_ = xSemaphoreCreateBinary();
  if (!wakeSem_) {
    Serial.println("[wisp.fwdist] failed to create wake semaphore");
    state_ = State::Disabled;
    return;
  }
  // Streaming task. Pinned to core 0 (the only core on the C6). Priority
  // 5 sits above the Arduino loop task (priority 1) so it preempts
  // auroraClient.loop() the moment a chunk is ready to push, but below
  // the WiFi/IDF tasks (priority 18+) so it can't starve the radio.
  const BaseType_t ok = xTaskCreatePinnedToCore(
      &FirmwareDistributor::streamingTaskTrampoline,
      "wisp.fwdist",
      kStreamingTaskStackSize,
      this,
      kStreamingTaskPriority,
      &streamingTask_,
      0);
  if (ok != pdPASS) {
    Serial.println("[wisp.fwdist] failed to create streaming task");
    state_ = State::Disabled;
    return;
  }
#endif

  Serial.printf("[wisp.fwdist] online; carrying v=0x%08lx chunks=%u\n",
                (unsigned long)carrier_->getVersion(),
                (unsigned)carrier_->getTotalChunks());
}

#if defined(ARDUINO) || defined(ESP_PLATFORM)

// Wake the streaming task. Safe from recv task + loop task + the streaming
// task itself (idempotent give on a binary semaphore).
void FirmwareDistributor::wakeStreamingTask() {
  if (wakeSem_) xSemaphoreGive(wakeSem_);
}

void FirmwareDistributor::streamingTaskTrampoline(void* arg) {
  static_cast<FirmwareDistributor*>(arg)->streamingTaskLoop();
}

// Task body. Blocks on the wake semaphore until there's work, then drains
// streamingTaskStep() until the state machine transitions out of an
// active state. The semaphore is a token-bucket of one; we give from
// state-transition sites (initial OFFER, ACCEPT rx, REQ rewind, etc) so
// the task can preempt the loop task without waiting on the next tick.
void FirmwareDistributor::streamingTaskLoop() {
  for (;;) {
    // Block until kicked. Polled timeout is defensive — under normal
    // operation we get woken explicitly; a missed wake bounded at
    // kStreamingIdlePollMs.
    xSemaphoreTake(wakeSem_, pdMS_TO_TICKS(kStreamingIdlePollMs));
    // Drain work until the state machine settles back into a non-active
    // state. streamingTaskStep returns true while it still has work to
    // do; false → go back to the semaphore.
    while (streamingTaskStep(millis())) {
      // Cooperative yield between iterations. esp_now_send is non-blocking
      // (it just queues to a shallow ring); a 1-tick delay gives the WiFi
      // task time to push frames over the air before we shove the next
      // one in.
      vTaskDelay(pdMS_TO_TICKS(kStreamingChunkSpacingMs));
    }
  }
}

// One iteration of the streaming-task loop body. Returns true if the
// caller should immediately loop again (more work pending), false if the
// task should return to the wake semaphore.
bool FirmwareDistributor::streamingTaskStep(uint32_t nowMs) {
  // Snapshot state + any session fields needed for the work decision.
  // Critical section is short — no mesh send, no Serial, no allocations.
  State s;
  uint8_t  targetMacLocal[6];
  uint32_t lastOfferLocal = 0;
  uint8_t  offerRetriesLocal = 0;
  uint16_t nextChunkLocal = 0;
  uint16_t totalChunksLocal = 0;
  portENTER_CRITICAL(&stateMux_);
  s = state_;
  std::memcpy(targetMacLocal, targetMac_, 6);
  lastOfferLocal     = lastOfferSendMs_;
  offerRetriesLocal  = offerRetryCount_;
  nextChunkLocal     = nextChunkIdx_;
  totalChunksLocal   = totalChunks_;
  portEXIT_CRITICAL(&stateMux_);

  if (s == State::OfferSent) {
    if (offerRetriesLocal >= kMaxOfferRetries) {
      // No more retries — wait for the recv task (ACCEPT) or tick()
      // (kAcceptTimeoutMs) to break us out. Idle until then.
      return false;
    }
    const uint32_t sinceLast = nowMs - lastOfferLocal;
    if (sinceLast >= kOfferRetryIntervalMs) {
      // Bump retry count under mux BEFORE sending so a fast Accept-rx
      // interleave reads the updated value.
      portENTER_CRITICAL(&stateMux_);
      offerRetryCount_++;
      portEXIT_CRITICAL(&stateMux_);
      sendOfferFrame(targetMacLocal, nowMs, /*isRetry=*/true);
      // sendOfferFrame stamps lastOfferSendMs_ under mux. Continue
      // looping; next iteration's snapshot sees the fresh value and
      // sleeps until the next retry interval.
      return true;
    }
    // Retry not yet due — sleep for the remaining slice.
    const uint32_t waitMs = kOfferRetryIntervalMs - sinceLast;
    vTaskDelay(pdMS_TO_TICKS(waitMs));
    return true;
  }

  if (s == State::Streaming) {
    if (nextChunkLocal >= totalChunksLocal) {
      // Drained — transition to Finalizing and emit DONE. Take mux to
      // commit the state change; emitDone sends OUTSIDE the mux.
      bool needFinalize = false;
      portENTER_CRITICAL(&stateMux_);
      if (state_ == State::Streaming &&
          nextChunkIdx_ >= totalChunks_) {
        state_ = State::Finalizing;
        stateEnteredMs_ = nowMs;
        needFinalize = true;
      }
      portEXIT_CRITICAL(&stateMux_);
      if (needFinalize) emitDone(nowMs);
      // Once we hit Finalizing, the streaming task has no more chunk
      // work. Let it idle on the wake semaphore; tick() handles the
      // finalize timeout, and the recv task wakes us on RESULT.
      return false;
    }
    const int rc = streamOneChunk(nowMs);
    if (rc == 1) {
      // NO_MEM — back off, then try same chunk next iteration.
      vTaskDelay(pdMS_TO_TICKS(kStreamingQueueBackoffMs));
      return true;
    }
    if (rc == 2) {
      // Carrier read failure aborted the session inside streamOneChunk.
      return false;
    }
    return true;
  }

  // Any other state: no work for the streaming task.
  return false;
}

// Single-chunk emit + index advance with mux discipline. Returns:
//   0 → chunk queued for send, indices advanced.
//   1 → ESP-NOW NO_MEM; caller should delay + retry.
//   2 → carrier read failure; session has been aborted in-place.
int FirmwareDistributor::streamOneChunk(uint32_t nowMs) {
  // Snapshot the chunk index + target under mux. We must not advance
  // nextChunkIdx_ until the frame is queued to ESP-NOW; if a concurrent
  // onReq rewinds nextChunkIdx_ between our snapshot and the send, we
  // re-read on the next iteration. emitChunk doesn't touch state_.
  uint16_t chunkIdx;
  uint8_t  targetMacLocal[6];
  uint32_t sessionTotalLenLocal;
  uint16_t chunkSize = lamp_protocol::FW_CHUNK_SIZE;
  portENTER_CRITICAL(&stateMux_);
  if (state_ != State::Streaming) {
    portEXIT_CRITICAL(&stateMux_);
    return 0;
  }
  chunkIdx = nextChunkIdx_;
  std::memcpy(targetMacLocal, targetMac_, 6);
  sessionTotalLenLocal = sessionTotalLen_;
  portEXIT_CRITICAL(&stateMux_);

  if (!mesh_ || !carrier_) return 2;
  uint8_t scratch[lamp_protocol::FW_CHUNK_SIZE];
  const size_t got = carrier_->getChunk(chunkIdx, scratch, sizeof(scratch));
  if (got == 0) {
    Serial.printf("[wisp.fwdist] getChunk(%u) returned 0; aborting\n",
                  (unsigned)chunkIdx);
    portENTER_CRITICAL(&stateMux_);
    recordPeerFailure(nowMs);  // notePeerBackoff is mux-safe (called under mux)
    resetSession();
    state_ = State::Failed;
    stateEnteredMs_ = nowMs;
    portEXIT_CRITICAL(&stateMux_);
    return 2;
  }
  uint8_t buf[lamp_protocol::FW_CHUNK_MAX_SIZE];
  const uint32_t offset =
      static_cast<uint32_t>(chunkIdx) * chunkSize;
  // seqCounter_ is only touched by streamOneChunk + emitOffer + emitDone;
  // both are called from non-recv contexts (streaming task / tick). Bump
  // under mux for safety.
  uint16_t seq;
  portENTER_CRITICAL(&stateMux_);
  seq = seqCounter_++;
  portEXIT_CRITICAL(&stateMux_);
  const size_t framed = lamp_protocol::buildFwChunk(
      buf, sizeof(buf), seq,
      cachedSrcMac_, targetMacLocal,
      chunkIdx, offset, scratch, static_cast<uint16_t>(got));
  if (!framed) return 1;
  // Send OUTSIDE the mux. esp_now_send is non-blocking; failure mode is
  // ESP_ERR_ESPNOW_NO_MEM when the TX ring is full, which is the
  // "queue contention" case the back-off handles.
  if (!mesh_->send(targetMacLocal, buf, framed)) {
    return 1;
  }
  (void)sessionTotalLenLocal;
  // Commit: advance nextChunkIdx_ IF it still points at the chunk we
  // just sent. If a concurrent onReq rewound it elsewhere, leave the
  // rewound value alone — next iteration sends from there.
  uint16_t emittedCount = 0;
  portENTER_CRITICAL(&stateMux_);
  if (state_ == State::Streaming && nextChunkIdx_ == chunkIdx) {
    lastSentChunk_ = chunkIdx;
    nextChunkIdx_++;
    currentChunkRetries_ = 0;
    lastSentMs_ = nowMs;
    lastBurstSentChunks_++;
    if (lastBurstSentChunks_ >= kStreamProgressLogEvery) {
      emittedCount = nextChunkIdx_;
      lastBurstSentChunks_ = 0;
    }
  }
  portEXIT_CRITICAL(&stateMux_);
  if (emittedCount != 0) {
    Serial.printf("[wisp.fwdist] stream progress: sent %u/%u chunks\n",
                  (unsigned)emittedCount, (unsigned)totalChunks_);
  }
  return 0;
}

#endif  // ARDUINO || ESP_PLATFORM

void FirmwareDistributor::tick(uint32_t nowMs) {
  if (state_ == State::Disabled) return;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  // loop() captured `nowMs` BEFORE auroraClient.loop() which can block for
  // seconds. The streaming task may have stamped stateEnteredMs_/lastSentMs_
  // with a FRESHER millis() during that block; using the stale param
  // underflows uint32_t subtractions and fires timeouts instantly.
  nowMs = millis();
#endif

  // Snapshot the state for the timeout/transition decisions. All
  // mutations below go through the mux. The streaming task does its own
  // OFFER retry + chunk emission cadence; tick() only owns Idle scanning,
  // timeouts, and the Failed/Done → Idle tombstone.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  State s;
  uint32_t stateEnteredMsLocal = 0;
  portENTER_CRITICAL(&stateMux_);
  s = state_;
  stateEnteredMsLocal = stateEnteredMs_;
  portEXIT_CRITICAL(&stateMux_);
#else
  State s = state_;
  uint32_t stateEnteredMsLocal = stateEnteredMs_;
#endif

  switch (s) {
    case State::Idle: {
      const uint32_t scanWindow = firstScan_ ? kInitialScanDelayMs : kScanPeriodMs;
      if (nowMs >= lastScanMs_ && (nowMs - lastScanMs_) < scanWindow) break;
      lastScanMs_ = nowMs;
      firstScan_  = false;
      pickTargetAndOffer(nowMs);
      break;
    }

    case State::OfferSent: {
      // OFFER retry cadence is handled inside the streaming task. tick()
      // only watches the overall ACCEPT-timeout window. The streaming
      // task can't drive timeouts because it sleeps on the wake
      // semaphore between work; the loop-driven tick gives us a free
      // ~1Hz pacemaker for "did we wait too long for the lamp to reply".
      if (nowMs >= stateEnteredMsLocal &&
          (nowMs - stateEnteredMsLocal) > kAcceptTimeoutMs) {
        Serial.println("[wisp.fwdist] OFFER timeout; backing off peer");
#if defined(ARDUINO) || defined(ESP_PLATFORM)
        portENTER_CRITICAL(&stateMux_);
        recordPeerFailure(nowMs);
        resetSession();
        state_ = State::Failed;
        stateEnteredMs_ = nowMs;
        portEXIT_CRITICAL(&stateMux_);
#else
        recordPeerFailure(nowMs);
        resetSession();
        state_ = State::Failed;
        stateEnteredMs_ = nowMs;
#endif
      }
      break;
    }

    case State::Streaming: {
      // Streaming itself runs on the dedicated task. tick() only checks
      // the stall watchdog (no forward progress in kChunkResendMs → bump
      // retry counter; exceeded → fail the peer). The streaming task
      // bumps lastSentMs_ on every chunk under mux.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
      uint32_t lastSentMsLocal = 0;
      uint8_t  currentRetriesLocal = 0;
      uint16_t nextChunkLocal = 0;
      uint16_t totalChunksLocal = 0;
      uint16_t lastSentChunkLocal = 0;
      portENTER_CRITICAL(&stateMux_);
      lastSentMsLocal     = lastSentMs_;
      currentRetriesLocal = currentChunkRetries_;
      nextChunkLocal      = nextChunkIdx_;
      totalChunksLocal    = totalChunks_;
      lastSentChunkLocal  = lastSentChunk_;
      portEXIT_CRITICAL(&stateMux_);
#else
      uint32_t lastSentMsLocal     = lastSentMs_;
      uint8_t  currentRetriesLocal = currentChunkRetries_;
      uint16_t nextChunkLocal      = nextChunkIdx_;
      uint16_t totalChunksLocal    = totalChunks_;
      uint16_t lastSentChunkLocal  = lastSentChunk_;
#endif
      // If we've already drained all chunks, the streaming task is
      // handling the DONE emit; nothing to do here.
      if (nextChunkLocal >= totalChunksLocal) break;
      if (nowMs >= lastSentMsLocal &&
          (nowMs - lastSentMsLocal) > kChunkResendMs) {
        if (currentRetriesLocal >= kRetriesPerChunk) {
          Serial.println("[wisp.fwdist] chunk retry budget exhausted; failing peer");
#if defined(ARDUINO) || defined(ESP_PLATFORM)
          portENTER_CRITICAL(&stateMux_);
          recordPeerFailure(nowMs);
          resetSession();
          state_ = State::Failed;
          stateEnteredMs_ = nowMs;
          portEXIT_CRITICAL(&stateMux_);
#else
          recordPeerFailure(nowMs);
          resetSession();
          state_ = State::Failed;
          stateEnteredMs_ = nowMs;
#endif
          break;
        }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
        portENTER_CRITICAL(&stateMux_);
        currentChunkRetries_++;
        // Rewind so the streaming task re-emits the stalled chunk.
        nextChunkIdx_ = lastSentChunkLocal;
        portEXIT_CRITICAL(&stateMux_);
        wakeStreamingTask();
#else
        currentChunkRetries_++;
        nextChunkIdx_ = lastSentChunkLocal;
#endif
      }
      break;
    }

    case State::Finalizing: {
      if (nowMs >= stateEnteredMsLocal &&
          (nowMs - stateEnteredMsLocal) > kFinalizeTimeoutMs) {
        Serial.println("[wisp.fwdist] FINALIZE timeout; short-backing off peer");
#if defined(ARDUINO) || defined(ESP_PLATFORM)
        portENTER_CRITICAL(&stateMux_);
        recordPeerFailureFinalize(nowMs);
        resetSession();
        state_ = State::Failed;
        stateEnteredMs_ = nowMs;
        portEXIT_CRITICAL(&stateMux_);
#else
        recordPeerFailureFinalize(nowMs);
        resetSession();
        state_ = State::Failed;
        stateEnteredMs_ = nowMs;
#endif
      }
      break;
    }

    case State::Failed:
    case State::Done:
      // Tombstone state: just return to Idle on the next tick.
      // Clear lastScanMs_ + firstScan_ so the next Idle tick scans
      // immediately — without this, after a Done/Failed we'd wait a full
      // kScanPeriodMs (5 min) before considering the next OTA candidate,
      // which compounds tail-end-stall recovery latency.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
      portENTER_CRITICAL(&stateMux_);
      resetSession();
      state_ = State::Idle;
      stateEnteredMs_ = nowMs;
      lastScanMs_ = 0;     // force next Idle scan to fire immediately
      firstScan_ = false;  // initial-delay grace already consumed
      portEXIT_CRITICAL(&stateMux_);
#else
      resetSession();
      state_ = State::Idle;
      stateEnteredMs_ = nowMs;
      lastScanMs_ = 0;
      firstScan_ = false;
#endif
      break;

    case State::Disabled:
      break;
  }
}

void FirmwareDistributor::onMeshPacket(uint8_t msgType, const uint8_t* data,
                                       size_t len) {
  if (state_ == State::Disabled) return;
  if (!mesh_) return;

  // We use millis() here because tick() is the one that drives transitions;
  // but for "did this arrive" timestamps we want a fresh sample on the recv
  // task. The state machine reads stateEnteredMs_ on tick() so a precise
  // recv timestamp here isn't load-bearing.
  const uint32_t nowMs = millis();

  switch (msgType) {
    case lamp_protocol::MSG_FW_ACCEPT: {
      lamp_protocol::ParsedFwAccept a;
      if (!lamp_protocol::parseFwAccept(data, len, a)) return;
      onAccept(a, nowMs);
      break;
    }
    case lamp_protocol::MSG_FW_REQ: {
      lamp_protocol::ParsedFwReq r;
      if (!lamp_protocol::parseFwReq(data, len, r)) return;
      onReq(r, nowMs);
      break;
    }
    case lamp_protocol::MSG_FW_RESULT: {
      lamp_protocol::ParsedFwResult r;
      if (!lamp_protocol::parseFwResult(data, len, r)) return;
      onResult(r, nowMs);
      break;
    }
    default:
      break;  // OFFER/CHUNK/DONE are wisp→lamp; we only listen for lamp→wisp.
  }
}

// --- Session bookkeeping -------------------------------------------------

void FirmwareDistributor::resetSession() {
  std::memset(targetMac_, 0, 6);
  totalChunks_     = 0;
  nextChunkIdx_    = 0;
  lastSentChunk_   = 0;
  lastSentMs_      = 0;
  currentChunkRetries_ = 0;
  sessionOfferSeq_ = 0;
  sessionVersion_  = 0;
  sessionTotalLen_ = 0;
  std::memset(sessionSha256Prefix_, 0, 8);
  lastOfferSendMs_  = 0;
  offerRetryCount_  = 0;
}

bool FirmwareDistributor::peerIsInBackoff(const uint8_t mac[6],
                                          uint32_t nowMs) const {
  for (size_t i = 0; i < kPenaltyRingSize; ++i) {
    const auto& p = penalties_[i];
    if (!p.used) continue;
    if (!macsEqual(p.mac, mac)) continue;
    // Treat the window as half-open: backoff active while now < until.
    if (nowMs < p.backoffUntilMs) return true;
  }
  return false;
}

void FirmwareDistributor::notePeerBackoff(const uint8_t mac[6], uint32_t nowMs,
                                          uint32_t durationMs) {
  // Reuse an existing slot for this MAC if present, else write into the
  // next ring slot (overwrite oldest).
  for (size_t i = 0; i < kPenaltyRingSize; ++i) {
    if (penalties_[i].used && macsEqual(penalties_[i].mac, mac)) {
      penalties_[i].backoffUntilMs = nowMs + durationMs;
      return;
    }
  }
  penalties_[penaltyHead_].used = true;
  std::memcpy(penalties_[penaltyHead_].mac, mac, 6);
  penalties_[penaltyHead_].backoffUntilMs = nowMs + durationMs;
  penaltyHead_ = (penaltyHead_ + 1) % kPenaltyRingSize;
}

void FirmwareDistributor::recordPeerFailure(uint32_t nowMs) {
  // Only record if there's a real target. (resetSession() will zero
  // targetMac_ but recordPeerFailure runs BEFORE it.)
  bool nonzero = false;
  for (int i = 0; i < 6; ++i) {
    if (targetMac_[i] != 0) { nonzero = true; break; }
  }
  if (nonzero) notePeerBackoff(targetMac_, nowMs, kPeerBackoffMs);
}

void FirmwareDistributor::recordPeerFailureFinalize(uint32_t nowMs) {
  // Same shape as recordPeerFailure but uses the short backoff window —
  // FINALIZE timeouts are typically tail-end RESULT loss, not a broken
  // peer, so we want to retry promptly rather than wait 10 minutes.
  bool nonzero = false;
  for (int i = 0; i < 6; ++i) {
    if (targetMac_[i] != 0) { nonzero = true; break; }
  }
  if (nonzero) notePeerBackoff(targetMac_, nowMs, kPeerFinalizeBackoffMs);
}

// --- Scan + offer --------------------------------------------------------

bool FirmwareDistributor::pickTargetAndOffer(uint32_t nowMs) {
  if (!inventory_ || !carrier_) return false;
  const uint32_t carriedVer = carrier_->getVersion();
  if (carriedVer == 0) return false;

  auto roster = inventory_->snapshot();
  const InventoryEntry* best = nullptr;

  for (const auto& e : roster) {
    if (e.firmwareVersion == 0) continue;            // pre-Phase-A peer
    if (e.firmwareVersion >= carriedVer) continue;   // not stale
    if ((nowMs - e.lastSeenMs) > kPeerFreshnessMs) continue;
    if (peerIsInBackoff(e.mac, nowMs)) continue;
    // Channel filter would go here if HELLO carried it. It doesn't; we
    // OFFER and let the lamp silent-reject channel mismatches. A failing
    // accept-timeout puts the peer into backoff for 10 minutes.
    if (!best || e.firmwareVersion < best->firmwareVersion) {
      best = &e;
    }
  }

  if (!best) return false;

  // Capture the target MAC on the stack — emitOffer will commit it into
  // targetMac_ under the state mux along with the rest of the session
  // snapshot.
  uint8_t pickMac[6];
  std::memcpy(pickMac, best->mac, 6);
  emitOffer(pickMac, nowMs);
  return true;
}

void FirmwareDistributor::emitOffer(const uint8_t targetMac[6], uint32_t nowMs) {
  if (!mesh_ || !carrier_) return;
  if (!carrier_->isReady()) return;

  const size_t blobSize = carrier_->getBlobSize();
  if (blobSize == 0) return;
  const uint16_t totalChunks = carrier_->getTotalChunks();
  if (totalChunks == 0) return;

  uint8_t sha[8];
  if (!carrier_->getSha256Prefix(sha)) return;

  // Initialise session state under the mux so the recv task and the
  // streaming task see a consistent snapshot.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portENTER_CRITICAL(&stateMux_);
#endif
  // Session state — pin to this offer so ACCEPT/REQ/RESULT echo match.
  // The seq is reused across retries; lamp's firmwareDedup_ collapses dupes.
  std::memcpy(targetMac_, targetMac, 6);
  sessionOfferSeq_  = seqCounter_++;
  totalChunks_      = totalChunks;
  nextChunkIdx_     = 0;
  lastSentChunk_    = 0;
  lastSentMs_       = 0;
  currentChunkRetries_ = 0;
  sessionVersion_   = carrier_->getVersion();
  sessionTotalLen_  = static_cast<uint32_t>(blobSize);
  std::memcpy(sessionSha256Prefix_, sha, 8);
  offerRetryCount_  = 0;
  lastOfferSendMs_  = nowMs;
  // Tentatively mark OfferSent before the send so a near-instant ACCEPT
  // arriving on the recv task pre-transition is correctly matched. If
  // the send fails we roll back below.
  state_ = State::OfferSent;
  stateEnteredMs_ = nowMs;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portEXIT_CRITICAL(&stateMux_);
#endif

  if (!sendOfferFrame(targetMac, nowMs, /*isRetry=*/false)) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    portENTER_CRITICAL(&stateMux_);
    resetSession();
    state_ = State::Idle;
    portEXIT_CRITICAL(&stateMux_);
#else
    resetSession();
    state_ = State::Idle;
#endif
    return;
  }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  // Kick the streaming task — it owns OFFER retries from here on.
  wakeStreamingTask();
#endif
}

bool FirmwareDistributor::sendOfferFrame(const uint8_t targetMac[6],
                                         uint32_t nowMs, bool isRetry) {
  if (!mesh_ || !carrier_) return false;
  // Snapshot session fields under mux so a concurrent recv/reset can't
  // tear them mid-build.
  uint16_t sessionOfferSeqLocal;
  uint32_t sessionVersionLocal;
  uint32_t sessionTotalLenLocal;
  uint16_t totalChunksLocal;
  uint8_t  sha[8];
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portENTER_CRITICAL(&stateMux_);
#endif
  sessionOfferSeqLocal = sessionOfferSeq_;
  sessionVersionLocal  = sessionVersion_;
  sessionTotalLenLocal = sessionTotalLen_;
  totalChunksLocal     = totalChunks_;
  std::memcpy(sha, sessionSha256Prefix_, 8);
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portEXIT_CRITICAL(&stateMux_);
#endif
  uint8_t srcMac[6];
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  std::memcpy(srcMac, cachedSrcMac_, 6);
#else
  mesh_->getMac(srcMac);
#endif
  uint8_t buf[lamp_protocol::FW_OFFER_FIXED_SIZE];
  const char* channel = carrier_->getChannel();
  const size_t channelLen = channel ? std::strlen(channel) : 0;
  const size_t n = lamp_protocol::buildFwOffer(
      buf, sizeof(buf), sessionOfferSeqLocal,
      srcMac, targetMac,
      sessionVersionLocal, sessionTotalLenLocal, lamp_protocol::FW_CHUNK_SIZE,
      channel, channelLen,
      sha, kFwFooterLenV1, totalChunksLocal);
  if (!n) {
    Serial.println("[wisp.fwdist] buildFwOffer failed (defensive)");
    return false;
  }
  if (!mesh_->send(targetMac, buf, n)) {
    Serial.println("[wisp.fwdist] OFFER send failed (esp_now_send returned false)");
    return false;
  }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portENTER_CRITICAL(&stateMux_);
#endif
  lastOfferSendMs_ = nowMs;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portEXIT_CRITICAL(&stateMux_);
#endif
  Serial.printf("[wisp.fwdist] OFFER%s → %02X:%02X:%02X:%02X:%02X:%02X "
                "v=0x%08lx chunks=%u seq=%u\n",
                isRetry ? " (retry)" : "",
                targetMac[0], targetMac[1], targetMac[2],
                targetMac[3], targetMac[4], targetMac[5],
                (unsigned long)sessionVersionLocal, (unsigned)totalChunksLocal,
                (unsigned)sessionOfferSeqLocal);
  return true;
}

void FirmwareDistributor::emitDone(uint32_t nowMs) {
  if (!mesh_) return;
  // Snapshot session fields under mux. emitDone runs on the streaming
  // task; the recv task could conceivably collide with a late OFFER
  // resend or a session reset in flight.
  //
  // seq is captured ONCE here and reused across every retry attempt
  // below. The lamp's onDoneOnLoop is state-guarded (Streaming-only),
  // so a duplicate DONE arriving after verify+reboot or after the
  // first DONE already triggered the apply path is a silent no-op.
  // Reusing the seq lets the lamp's general dedup paths (if any later
  // add a control-plane dedup ring) collapse the duplicates.
  uint32_t sessionVersionLocal;
  uint32_t sessionTotalLenLocal;
  uint8_t  sha[8];
  uint8_t  targetMacLocal[6];
  uint16_t seq;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portENTER_CRITICAL(&stateMux_);
#endif
  sessionVersionLocal  = sessionVersion_;
  sessionTotalLenLocal = sessionTotalLen_;
  std::memcpy(sha, sessionSha256Prefix_, 8);
  std::memcpy(targetMacLocal, targetMac_, 6);
  seq = seqCounter_++;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portEXIT_CRITICAL(&stateMux_);
#endif

  uint8_t srcMac[6];
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  std::memcpy(srcMac, cachedSrcMac_, 6);
#else
  mesh_->getMac(srcMac);
#endif
  uint8_t buf[lamp_protocol::FW_DONE_FIXED_SIZE];
  const size_t n = lamp_protocol::buildFwDone(
      buf, sizeof(buf), seq,
      srcMac, targetMacLocal,
      sessionVersionLocal, sessionTotalLenLocal,
      sha, kFwFooterLenV1);
  if (!n) return;

  // First attempt + up to kMaxDoneRetries follow-ups. Bail early if
  // the state pivots out of Finalizing — onResult (Done/Failed),
  // onReq (back to Streaming for late gap fill), or the tick()
  // finalize-timeout path can all change state under us, and any of
  // those means we don't need to keep spamming DONE.
  //
  // Asymmetry with OFFER retries: OFFER retries are paced by tick()
  // checking lastOfferSendMs_ from the streaming-task step loop
  // because the streaming task must yield to other work between
  // OFFER attempts. DONE retries don't compete with anything —
  // chunk emission is done, so we can stay in a tight loop here.
  const uint8_t  budget = 1 + kMaxDoneRetries;
  for (uint8_t attempt = 0; attempt < budget; ++attempt) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    State sNow;
    portENTER_CRITICAL(&stateMux_);
    sNow = state_;
    portEXIT_CRITICAL(&stateMux_);
    if (sNow != State::Finalizing) {
      Serial.printf("[wisp.fwdist] DONE retry bail: state pivoted (attempts=%u)\n",
                    (unsigned)attempt);
      return;
    }
#else
    if (state_ != State::Finalizing) return;
#endif
    mesh_->send(targetMacLocal, buf, n);
    Serial.printf("[wisp.fwdist] DONE%s → %02X:%02X:%02X:%02X:%02X:%02X "
                  "(attempt %u/%u, waiting RESULT)\n",
                  attempt == 0 ? "" : " (retry)",
                  targetMacLocal[0], targetMacLocal[1], targetMacLocal[2],
                  targetMacLocal[3], targetMacLocal[4], targetMacLocal[5],
                  (unsigned)(attempt + 1), (unsigned)budget);
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    if (attempt + 1 < budget) {
      // Sleep before the next attempt. vTaskDelay yields the CPU so the
      // WiFi task can deliver the queued frame, the lamp can process it,
      // and a RESULT can arrive and pivot state_ → Done/Failed before our
      // next loop iteration.
      vTaskDelay(pdMS_TO_TICKS(kDoneRetryIntervalMs));
    }
#endif
  }
  (void)nowMs;
}

// --- Inbound dispatchers -------------------------------------------------

void FirmwareDistributor::onAccept(const lamp_protocol::ParsedFwAccept& a,
                                   uint32_t nowMs) {
  // The recv path runs on the WiFi task. All state inspection +
  // mutation happens under the mux; mesh sends + Serial.printf happen
  // outside the mux.
  bool wake = false;
  bool logAccept = false;
  bool logBackoff = false;
  uint8_t logStatus = 0;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portENTER_CRITICAL(&stateMux_);
#endif
  if (state_ != State::OfferSent ||
      !macsEqual(a.sourceMac, targetMac_) ||
      a.offerSeq != sessionOfferSeq_ ||
      a.version != sessionVersion_) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&stateMux_);
#endif
    return;
  }
  if (a.status != lamp_protocol::FwAcceptStatus::Accept) {
    logBackoff = true;
    logStatus = static_cast<uint8_t>(a.status);
    recordPeerFailure(nowMs);
    resetSession();
    state_ = State::Failed;
    stateEnteredMs_ = nowMs;
  } else {
    state_ = State::Streaming;
    stateEnteredMs_ = nowMs;
    // The streaming task may be parked on the wake semaphore — kick it
    // so the first chunk goes out immediately, not at the next idle
    // poll boundary.
    wake = true;
    logAccept = true;
  }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portEXIT_CRITICAL(&stateMux_);
#endif

  if (logBackoff) {
    Serial.printf("[wisp.fwdist] ACCEPT status=%u; backing off\n",
                  (unsigned)logStatus);
  }
  if (logAccept) {
    Serial.printf("[wisp.fwdist] ACCEPT from %02X:%02X:%02X:%02X:%02X:%02X; streaming\n",
                  a.sourceMac[0], a.sourceMac[1], a.sourceMac[2],
                  a.sourceMac[3], a.sourceMac[4], a.sourceMac[5]);
  }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  if (wake) wakeStreamingTask();
#else
  (void)wake;
#endif
}

void FirmwareDistributor::onReq(const lamp_protocol::ParsedFwReq& r,
                                uint32_t nowMs) {
  bool wake = false;
  bool log = false;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portENTER_CRITICAL(&stateMux_);
#endif
  if ((state_ != State::Streaming && state_ != State::Finalizing) ||
      !macsEqual(r.sourceMac, targetMac_) ||
      r.firstChunkIdx >= totalChunks_) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&stateMux_);
#endif
    return;
  }
  // Rewind cursor. The streaming task re-emits from here; once it walks
  // past the gap it naturally resumes forward emit.
  nextChunkIdx_        = r.firstChunkIdx;
  currentChunkRetries_ = 0;
  lastSentMs_          = nowMs;
  if (state_ == State::Finalizing) {
    state_ = State::Streaming;
    stateEnteredMs_ = nowMs;
  }
  wake = true;
  log  = true;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portEXIT_CRITICAL(&stateMux_);
#endif

  if (log) {
    Serial.printf("[wisp.fwdist] REQ first=%u count=%u reason=%u; rewinding\n",
                  (unsigned)r.firstChunkIdx, (unsigned)r.chunkCount,
                  (unsigned)static_cast<uint8_t>(r.reason));
  }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  if (wake) wakeStreamingTask();
#else
  (void)wake;
#endif
}

void FirmwareDistributor::onResult(const lamp_protocol::ParsedFwResult& r,
                                   uint32_t nowMs) {
  bool logSuccess = false;
  bool logFailure = false;
  uint8_t logStatus = 0;
  uint8_t logDetail = 0;
  uint8_t logMac[6] = {0};
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portENTER_CRITICAL(&stateMux_);
#endif
  if (state_ != State::Finalizing ||
      !macsEqual(r.sourceMac, targetMac_) ||
      r.version != sessionVersion_) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&stateMux_);
#endif
    return;
  }
  const uint8_t status = static_cast<uint8_t>(r.status);
  std::memcpy(logMac, r.sourceMac, 6);
  if (status == static_cast<uint8_t>(lamp_protocol::FwResultStatus::Success)) {
    resetSession();
    state_ = State::Done;
    stateEnteredMs_ = nowMs;
    logSuccess = true;
  } else {
    logFailure = true;
    logStatus = status;
    logDetail = r.detail;
    recordPeerFailure(nowMs);
    resetSession();
    state_ = State::Failed;
    stateEnteredMs_ = nowMs;
  }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  portEXIT_CRITICAL(&stateMux_);
#endif

  if (logSuccess) {
    Serial.printf("[wisp.fwdist] RESULT success from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  logMac[0], logMac[1], logMac[2],
                  logMac[3], logMac[4], logMac[5]);
  }
  if (logFailure) {
    Serial.printf("[wisp.fwdist] RESULT failure status=%u detail=%u\n",
                  (unsigned)logStatus, (unsigned)logDetail);
  }
}

}  // namespace wisp
