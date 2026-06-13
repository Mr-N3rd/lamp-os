# BLE Audit — deferred items

Findings from `2026-05-26-ble-stack-audit.md` that weren't applied today because they require larger refactors. Hold these for v1.1 or v2.

## C2 — BLE callback / loop() data race

**Severity:** Critical (theoretically — hasn't been observed yet)

`dispatchLampAction` is called from BLE `onWrite` callbacks on the NimBLE host task (Core 0). It mutates `shadeConfiguratorBehavior.colors`, `baseConfiguratorBehavior.colors`, `baseKnockoutBehavior.knockoutPixels`, and `expressionManager` — which `loop()` and `compositor.tick()` read concurrently on Core 1. No mutex. The same race already existed pre-BLE via the WebSocket path; BLE makes it worse.

**Fix shape:** Introduce a `QueueHandle_t` for BLE actions. BLE callbacks push `JsonDocument` into the queue; `loop()` drains the queue each tick and calls `dispatchLampAction` from the loop task only.

```cpp
// In ble_control or a new module:
extern QueueHandle_t g_bleActionQueue;  // created at boot, depth ~20

// In BLE callbacks (any onWrite that previously called dispatchLampAction):
xQueueSend(g_bleActionQueue, &doc, 0);

// In loop():
JsonDocument bleDoc;
while (xQueueReceive(g_bleActionQueue, &bleDoc, 0) == pdTRUE) {
    dispatchLampAction(bleDoc, millis());
}
```

Same applies to the WebSocket handler path which has the same problem and should share the queue.

## I1 — `s_config` read from BLE host task during settings_blob read

`SettingsBlobCallback::onRead` calls `s_config->asJsonDocument()` from the BLE host task while `loop()` mutates `s_config` (e.g., knockoutPixels). Risk: torn read producing inconsistent JSON.

**Fix shape:** Either (a) use the same queue solution as C2 — defer the JSON serialization to the loop task and have the BLE callback wait for the result, or (b) wrap the read in a critical section. (a) is cleaner.

## I5 — Color-sync beacon manufacturer data never updated on color change

`BluetoothComponent::begin()` sets manufacturer data (base+shade RGB) once at boot. When colors change at runtime (BLE GATT, WebSocket, DMX), the advertised packet still shows the boot colors. Social color-sync between lamps sees stale colors.

**Fix shape:**
1. Add `BluetoothComponent::updateBeaconColors(Color base, Color shade)` that re-packs the 8-byte payload and calls `pAdvertising->setManufacturerData(data)` then `pAdvertising->refreshAdvertisingData()`.
2. Call it from `dispatchLampAction` after handling `base` and `shade` actions (loop task — NOT from BLE callbacks; GAP calls must not run on the BLE host task).
3. Tie this to the queue from C2 — beacon refresh happens on the loop side automatically.

## I6 — `toApMode` called repeatedly on stage transitions could disrupt active BLE sessions

`handleStageMode()` calls `wifi.toApMode()` whenever stage signal is lost. `toApMode` now stops + restarts the scan and re-runs `server->start()` (idempotent guard) and `advertising->start()` (returns early if already running). If the user has an active BLE GATT session and the stage signal flickers, the scan stop/start could disrupt the session.

**Fix shape:** In `toApMode`, guard the BLE setup portion: only run if not already in AP mode. Track state and skip if already configured.

## I8 — `onScanEnd` restarts scan with `BLE_GAP_SCAN_TIME_MS` (1000ms periodic) instead of `start(0)` continuous

After any scan disruption, the scan silently degrades from continuous to 1-second periodic cycles. Subtle color-sync responsiveness regression.

**Fix shape:** Change `onScanEnd` to call `scan->start(0)` for continuous mode.

## I9 — `ble_setup::stop()` and `ble_control::stop()` call `getAdvertising()->stop()` without restarting

Landmine for future refactors. Calling `stop()` to "just remove the GATT service" kills advertising too.

**Fix shape:** Remove `getAdvertising()->stop()` calls from both `stop()` implementations. Let the caller manage advertising lifecycle explicitly.

## M1 / M2 — Callback memory leaks on `stop()` + `start()` cycle

`ControlServerCallbacks` and characteristic callbacks are heap-allocated and never freed because `setCallbacks(..., false)` is used. Not triggered today (stop/start isn't exercised at runtime). Fix when adding factory reset feature.

## R6 — `INDICATE` vs `NOTIFY` for `state_notify`

`state_notify` is fire-and-forget NOTIFY. If the app is busy when a notification arrives, it's dropped. INDICATE has ACK with higher latency — more reliable for "lamp state changed, please refresh" events. Consider switching.

## R7 — `saving.value = false` not reset on success path of post-save reconnect

Technically correct today (`initialize()` resets it internally) but fragile. Set explicitly in the success path of the setTimeout callback.

## R8 — Advertising intervals on the high side (250-406ms)

`BLE_ADVERTISING_INTERVAL_MIN=400, MAX=650` translates to 250ms/406ms (units are 0.625ms). Slow discovery for a connectable peripheral. Consider 100ms/150ms (`160`/`240`) for better discoverability at modest power cost.
