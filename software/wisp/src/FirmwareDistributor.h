// FirmwareDistributor — the wisp side of mesh OTA. Single-peer-at-a-time
// state machine that pushes the carried signed firmware blob to one lamp
// per session.
//
// Phase F. Mirrors PaintDistributor in shape (begin/tick/state held
// per-instance, no globals) but holds a richer state machine for the
// streamed OFFER → ACCEPT → CHUNK* → DONE → RESULT handshake.
//
// State machine:
//
//   Disabled --(carrier ready)--> Idle
//   Idle      --(scan tick)----->  OfferSent       (emitOffer)
//   OfferSent --(accept rx)----->  Streaming       (start chunk loop)
//   OfferSent --(timeout)------>   Failed          (backoff peer)
//   Streaming --(chunks done)-->   Finalizing      (emitDone)
//   Streaming --(REQ rx)------->   Streaming       (rewind cursor to req)
//   Streaming --(stalls)------->   Failed          (chunk retries exhausted)
//   Finalizing --(result rx)-->    Done            (transition to Idle next tick)
//   Finalizing --(timeout)----->   Failed
//   Failed     --(next tick)-->    Idle            (peer in backoff ring)
//   Done       --(next tick)-->    Idle
//
// Concurrency: three contexts touch session state.
//   - onMeshPacket runs on the WiFi recv task (Core 0). Parses ACCEPT/REQ/
//     RESULT and pivots state. Never sends mesh frames; never blocks.
//   - tick() runs on the Arduino loop task (Core 0; the C6 is unicore so
//     "Core 0" just means "the only core"). Drives Idle scanning, scans for
//     ACCEPT/FINALIZE timeouts, and pumps Failed/Done tombstone transitions.
//   - The streaming task — a dedicated FreeRTOS task created in begin() —
//     runs OFFER retries and chunk emission at high cadence. tick()'s
//     cadence is dominated by auroraClient.loop() blocking for hundreds of
//     ms to seconds at a time, so retries and chunk bursts in tick() were
//     capped at ~1 chunk/sec. Moving them into a priority-5 task lets
//     them preempt the loop task and run at 1 ms cadence (~250 chunks/sec).
//
// Shared session state (state_, nextChunkIdx_, lastSentChunk_, lastSentMs_,
// currentChunkRetries_, offerRetryCount_, lastOfferSendMs_, targetMac_,
// sessionVersion_, etc.) is guarded by a portMUX_TYPE critical section. The
// streaming task takes the mux only to snapshot/mutate small fields; mesh
// send + Serial.print happen OUTSIDE the mux to avoid pinning ISRs.

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace lamp_protocol {
struct ParsedFwAccept;
struct ParsedFwReq;
struct ParsedFwResult;
}  // namespace lamp_protocol

namespace wisp {

class FirmwareCarrier;
class LampInventory;
class MeshLink;
struct InventoryEntry;

class FirmwareDistributor {
 public:
  // Wire up dependencies. Call once in setup() AFTER mesh + carrier are
  // initialised. If carrier->isReady() is false the distributor stays
  // permanently Disabled.
  void begin(MeshLink* mesh, FirmwareCarrier* carrier,
             LampInventory* inventory);

  // Inbound packet hook — main.cpp dispatches MSG_FW_ACCEPT/REQ/RESULT
  // here from onMeshPacket. Idempotent on irrelevant packets (wrong
  // target MAC, wrong state, etc).
  //
  // Runs on the WiFi recv task. Keep work tight: parse + a state-machine
  // pivot. No mesh sends.
  void onMeshPacket(uint8_t msgType, const uint8_t* data, size_t len);

  // Drive the state machine. Called every loop() iteration.
  void tick(uint32_t nowMs);

  enum class State : uint8_t {
    Disabled = 0,
    Idle,
    OfferSent,
    Streaming,
    Finalizing,
    Failed,
    Done,
  };
  State state() const { return state_; }

  // --- Tunables (constexpr; tests static_assert against these). ---
  // Streaming task cadence. We push one chunk, then vTaskDelay between
  // them to let the WiFi task drain the TX queue. On NO_MEM back from
  // esp_now_send we delay kStreamingQueueBackoffMs and try the same
  // chunk again. The streaming task runs at priority
  // kStreamingTaskPriority (well above the loop task's priority 1, well
  // below the WiFi/IDF tasks).
  //
  // Hardware-tested 2026-06-05: a 1ms spacing pushed ~130 chunks/sec but
  // overwhelmed the lamp (loopTask starved by Core 0's
  // esp_ota_write_with_offset flash-bus contention; lamp task_wdt
  // panics around chunk 500). A 10ms spacing paces at ~80 chunks/sec
  // (200B chunk × 80 → ~16 KB/s sustained), still well over the 50
  // chunks/sec floor in the success criteria, with enough lamp-side
  // breathing room for the loop task to keep yielding.
  static constexpr uint32_t kStreamingChunkSpacingMs   = 10;
  static constexpr uint32_t kStreamingQueueBackoffMs   = 5;
  static constexpr uint32_t kStreamingTaskStackSize    = 4096;
  static constexpr uint32_t kStreamingTaskPriority     = 5;
  // Wake-semaphore poll timeout when the task has no work — task wakes
  // every N ms to re-check state in case a signal was lost (defensive).
  static constexpr uint32_t kStreamingIdlePollMs       = 250;
  // Log a stream-progress line every N chunks emitted.
  static constexpr uint16_t kStreamProgressLogEvery    = 256;
  // OFFER retry. ESP-NOW unicast over a BLE-coex'd radio drops packets
  // intermittently; with the wisp's send-callback we now see ~50% of the
  // initial OFFER attempts MAC-layer FAIL (hardware-confirmed 2026-06-04).
  // Resend up to N times spaced kOfferRetryIntervalMs apart, reusing the
  // same sessionOfferSeq_ so the lamp's firmwareDedup_ collapses dupes.
  //
  // Interval is set to 200ms specifically to NOT resonate with BLE
  // advert intervals (commonly 100-200ms) — at 500ms we observed both
  // tries landing in the same coex busy window. 200ms shifts across
  // multiple advert slots per retry burst.
  static constexpr uint8_t  kMaxOfferRetries    = 8;
  static constexpr uint32_t kOfferRetryIntervalMs = 200;
  static constexpr uint32_t kAcceptTimeoutMs    = 5000;
  static constexpr uint32_t kFinalizeTimeoutMs  = 30000;
  // DONE retry. emitDone is sent ONCE before any retry logic ran in
  // Phase F — under BLE coex with ~50% unicast loss, a dropped DONE
  // burned the full kFinalizeTimeoutMs and put the peer in 10-min
  // backoff for no reason. Retry the DONE send up to kMaxDoneRetries
  // times after the initial send (so 1 + kMaxDoneRetries total
  // attempts), spaced kDoneRetryIntervalMs apart, reusing the same
  // DONE seq across attempts. The lamp's DONE handler is naturally
  // idempotent (it state-guards on Streaming) — a duplicate DONE
  // arriving after verify+reboot is silently dropped.
  //
  // 300ms is chosen specifically (vs OFFER's 200ms) because by the
  // time we get here the lamp has just done O(seconds) of
  // esp_ota_write_with_offset flushing — its loop task is still
  // settling and a tighter cadence risks colliding with the RESULT
  // emit. 4 retries × 300ms = 1.2s window; lamp RTT for verify+
  // RESULT emit is ~50-200ms under normal conditions, so a single
  // delivered DONE comfortably yields a RESULT before our budget
  // exhausts.
  static constexpr uint8_t  kMaxDoneRetries     = 4;
  static constexpr uint32_t kDoneRetryIntervalMs = 300;
  static constexpr uint32_t kChunkResendMs      = 1500;
  static constexpr uint8_t  kRetriesPerChunk    = 4;
  // 10-minute peer cool-down after any session failure.
  static constexpr uint32_t kPeerBackoffMs      = 600000;
  // Short backoff for FINALIZE-timeout case: tail-end RESULT loss is
  // recoverable on a fast retry without the 10-minute penalty.
  static constexpr uint32_t kPeerFinalizeBackoffMs = 15000;  // tail-end loss: retry promptly
  // 5-minute scan period after a 30s post-boot grace.
  static constexpr uint32_t kInitialScanDelayMs = 30000;
  static constexpr uint32_t kScanPeriodMs       = 300000;
  // Freshness window for a peer to be considered an OTA candidate.
  static constexpr uint32_t kPeerFreshnessMs    = 60000;

 private:
  // Scan + offer. Returns true if a target was picked.
  bool pickTargetAndOffer(uint32_t nowMs);

  void emitOffer(const uint8_t targetMac[6], uint32_t nowMs);
  // Build + unicast the OFFER frame using current session state. Used by
  // emitOffer for the initial send and by tick()'s OfferSent retry loop
  // for resends. Returns false on build/send failure (caller decides
  // whether to roll back state).
  bool sendOfferFrame(const uint8_t targetMac[6], uint32_t nowMs,
                      bool isRetry);
  // Build + unicast the DONE frame using current session state. Called
  // from the streaming task after the last chunk emits.
  void emitDone(uint32_t nowMs);

  void recordPeerFailure(uint32_t nowMs);
  // Variant used when FINALIZE times out — likely a tail-end RESULT loss
  // rather than a fundamentally broken peer; uses the short backoff so we
  // retry quickly instead of waiting 10 minutes.
  void recordPeerFailureFinalize(uint32_t nowMs);
  void resetSession();

  // Inbound dispatchers.
  void onAccept(const lamp_protocol::ParsedFwAccept& a, uint32_t nowMs);
  void onReq(const lamp_protocol::ParsedFwReq& r, uint32_t nowMs);
  void onResult(const lamp_protocol::ParsedFwResult& r, uint32_t nowMs);

  // --- Streaming task ---
  // Created in begin() if the carrier is ready. Runs OFFER retries +
  // chunk emission. tick() and the recv path signal it via wakeSem_.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  static void streamingTaskTrampoline(void* arg);
  void        streamingTaskLoop();
  // Called from the streaming task — returns true while there's still
  // work to do (state is OfferSent or Streaming). Encapsulates one
  // iteration of the task's inner loop: send one OFFER retry if due, OR
  // send one chunk if streaming, OR returns false to put the task back
  // on the wake semaphore.
  bool        streamingTaskStep(uint32_t nowMs);
  // Single-chunk emit used by the streaming task. Returns:
  //   0 → chunk sent, advance to next.
  //   1 → ESP-NOW NO_MEM; back off and retry same chunk.
  //   2 → carrier read failure; session aborted.
  int         streamOneChunk(uint32_t nowMs);
  // Signal the streaming task to wake. Safe from recv task + tick().
  void        wakeStreamingTask();
#endif

  // Peer backoff ring. Linear scan; size matches a reasonable
  // worst-case (concurrent failures across a 50-lamp fleet are a venue-
  // level radio problem, not an OTA problem).
  static constexpr size_t kPenaltyRingSize = 8;
  struct PeerPenalty {
    uint8_t  mac[6];
    uint32_t backoffUntilMs;
    bool     used;
  };
  bool peerIsInBackoff(const uint8_t mac[6], uint32_t nowMs) const;
  void notePeerBackoff(const uint8_t mac[6], uint32_t nowMs,
                       uint32_t durationMs);

  // --- Wiring ---
  MeshLink*        mesh_      = nullptr;
  FirmwareCarrier* carrier_   = nullptr;
  LampInventory*   inventory_ = nullptr;

  // --- State machine ---
  State    state_           = State::Disabled;
  uint32_t lastScanMs_      = 0;
  uint32_t stateEnteredMs_  = 0;
  bool     firstScan_       = true;

  // --- Active session ---
  uint8_t  targetMac_[6]          = {0};
  uint16_t totalChunks_           = 0;
  uint16_t nextChunkIdx_           = 0;
  uint16_t lastSentChunk_          = 0;
  uint32_t lastSentMs_             = 0;
  uint8_t  currentChunkRetries_    = 0;
  uint16_t sessionOfferSeq_        = 0;
  uint32_t sessionVersion_         = 0;
  uint32_t sessionTotalLen_        = 0;
  uint8_t  sessionSha256Prefix_[8] = {0};
  uint16_t seqCounter_             = 0;
  // OFFER retry state. lastOfferSendMs_ stamps each (re)send so tick()'s
  // OfferSent branch can space retries by kOfferRetryIntervalMs.
  uint32_t lastOfferSendMs_        = 0;
  uint8_t  offerRetryCount_        = 0;
  // Rolling counter of chunks emitted since the last progress-log line.
  // Reset to 0 after each log to space the lines out by kStreamProgressLogEvery.
  uint16_t lastBurstSentChunks_    = 0;

#if defined(ARDUINO) || defined(ESP_PLATFORM)
  // Critical-section mux protecting all session state shared between the
  // recv task (onMeshPacket → onAccept/onReq/onResult), the loop task
  // (tick), and the streaming task. Initialised in begin().
  portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;
  // Wake semaphore for the streaming task. Given by the entity that
  // transitions state into OfferSent or Streaming (initial offer, ACCEPT
  // rx, REQ rewind). Token-bucket of 1 — multiple gives collapse to a
  // single wake, which is exactly what we want.
  SemaphoreHandle_t wakeSem_       = nullptr;
  TaskHandle_t      streamingTask_ = nullptr;
  // Cached MAC of the wisp for chunk-frame source address. Reading
  // mesh_->getMac requires an esp_wifi_get_mac call (not cheap); we
  // snapshot it once in begin().
  uint8_t           cachedSrcMac_[6] = {0};
#endif

  // --- Backoff bookkeeping ---
  PeerPenalty penalties_[kPenaltyRingSize] = {};
  size_t      penaltyHead_                 = 0;
};

}  // namespace wisp
