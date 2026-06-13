# BLE Stack Audit — 2026-05-26

Scope: ESP32 NimBLE-Arduino 2.3.6 firmware (`ble_control`, `ble_setup`, `bluetooth`, `bluetooth_pool`, `wifi.cpp`) and Capacitor mobile app (`bleControl.ts`, `ble.ts`, `scan.ts`, `lamp.ts`, `AddLamp.vue`, `Lamp.vue`). Already-fixed bugs listed in the task are explicitly excluded.

---

## 🔴 Critical

### C1 — `advertiseOnDisconnect` is `false` by default in NimBLE 2.x; lamp stops advertising after first GATT disconnect

- **Where**: `software/lamp-os/src/components/network/ble_control.cpp:47-69` (`ControlServerCallbacks`), `software/lamp-os/src/components/network/wifi.cpp:280`
- **What**: `NimBLEServer::m_advertiseOnDisconnect` defaults to `false` in NimBLE 2.x (explicitly documented in CHANGELOG line 133: "enabling automatic advertising on disconnect, which was disabled by default in 2.x"). Neither `ble_control.cpp` nor `ble_setup.cpp` calls `pServer->advertiseOnDisconnect(true)`, and the `onDisconnect` callback does not restart advertising.
- **Why it matters**: After any GATT client (mobile app) disconnects, the lamp stops being discoverable over BLE entirely. The color-sync beacon is also gone. The only recovery is a reboot. This will manifest every single time a user exits the app or their phone disconnects.
- **Fix**:
  ```cpp
  // In ble_control::start(), after s_server = NimBLEDevice::createServer():
  s_server->advertiseOnDisconnect(true);
  ```
  Or in `ControlServerCallbacks::onDisconnect`:
  ```cpp
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
      s_connAuth.erase(connInfo.getConnHandle());
      NimBLEDevice::getAdvertising()->start();  // restart advertising
  }
  ```
  The first approach is simpler and recommended.

---

### C2 — `dispatchLampAction` is called from BLE host task while `loop()` reads/writes the same behavior objects — data race

- **Where**: `software/lamp-os/src/components/network/ble_control.cpp:112,138,165,192` (all `onWrite` callbacks call `dispatchLampAction`); `software/lamp-os/src/lamps/standard_lamp.cpp:201-278` (`dispatchLampAction` modifies `shadeConfiguratorBehavior`, `baseConfiguratorBehavior`, `baseKnockoutBehavior`, `expressionManager`)
- **What**: NimBLE callbacks run on the `nimble_host` FreeRTOS task. `dispatchLampAction` modifies shared objects (`shadeConfiguratorBehavior.colors`, `baseConfiguratorBehavior.colors`, `baseKnockoutBehavior.knockoutPixels`, etc.) that `loop()` and `compositor.tick()` access concurrently on the Arduino loop task (Core 1). There is no mutex, semaphore, or critical section protecting these.
- **Why it matters**: Tearing, null-pointer reads, and corrupted color arrays. On a dual-core ESP32, `loop()` on Core 1 and the NimBLE host on Core 0 run simultaneously. This is a real data race that may corrupt colors mid-frame or cause hard faults when vectors are resized during iteration. The WebSocket path (`handleWebSocket`) goes through the same `dispatchLampAction` but the AsyncWebServer also runs on a different task — so this race already existed pre-BLE, and BLE makes it worse.
- **Fix**: Queue BLE actions through a `QueueHandle_t` instead of calling `dispatchLampAction` directly from the BLE callback. The `loop()` processes the queue each tick:
  ```cpp
  // In ble_control.cpp, instead of dispatchLampAction(doc, millis()):
  xQueueSend(g_bleActionQueue, &doc, 0);
  
  // In loop():
  JsonDocument bleDoc;
  while (xQueueReceive(g_bleActionQueue, &bleDoc, 0) == pdTRUE) {
      dispatchLampAction(bleDoc, millis());
  }
  ```

---

### C3 — `SettingsBlobCallback::onRead` calls `setValue` inside the callback; NimBLE returns the PRE-callback value for the actual read — settings blob reads may return stale data forever

- **Where**: `software/lamp-os/src/components/network/ble_control.cpp:227-232`
- **What**: The NimBLE 2.x `handleGattEvent` for a READ op (see `NimBLEServer.cpp:631-641`) works as follows: (1) call `pAtt->readEvent(peerInfo)` which fires `onRead`, (2) then `os_mbuf_append(ctxt->om, val.data(), val.size())` appends `val` — the characteristic's stored value — into the response buffer. The `onRead` callback must call `c->setValue()` to update the stored value BEFORE the mbuf append, which happens in the same stack frame immediately after `readEvent` returns. Because `setValue` updates `m_value` and `val` is a reference to `m_value`, this timing is actually correct for the first (and only) read chunk.
- **However**, there is a subtle bug: for long reads (value > MTU-3 bytes), NimBLE sends subsequent `Read Blob Request` ATT operations. For these follow-up requests, `om_len == 0` so `readEvent` is NOT called (see comment: "Don't call readEvent if the buffer len is 0 (this is a follow up to a previous read)"). NimBLE returns subsequent chunks from the stored value set during the first `onRead`. This is actually fine and correct behavior. BUT: if the firmware is actively changing `s_config` between chunks (via another write), the stored value (set by the first `onRead`'s `setValue`) is still used — so the read is atomic relative to config changes ONLY if config doesn't change between the first and subsequent ATT read chunks. This is unlikely but worth noting.
- **The real bug here is lower severity than initially thought**, but a concrete issue remains: `onRead` is called for every initial read. If the settings JSON is large (say 600 bytes) and the app performs a long read (many MTU-sized chunks), only the first chunk triggers `onRead`. This is correct NimBLE behavior, so the current code is OK. Downgraded to important — see I1.

---

### C4 — `pruneLamps` / `pruneStages` skip alternating expired elements (erase-while-forward-iterating bug)

- **Where**: `software/lamp-os/src/components/network/bluetooth_pool.cpp:38-46`, `79-86`
- **What**: Both `pruneLamps()` and `pruneStages()` iterate `for (int i = 0; i < pool.size(); i++)` and call `pool.erase(pool.begin() + i)` without decrementing `i`. After erasing element at index `i`, the next element shifts to `i`, but `i++` skips it. When multiple consecutive lamps are expired simultaneously, every other one survives until the next prune cycle.
- **Why it matters**: The lamp pool slowly fills with ghost entries (up to `MAX_POOL_SIZE = 20`) after lamps leave range. The social behavior can try to color-sync with lamps that are no longer present. In environments with many lamps cycling in/out, the pool saturates and new lamps are silently dropped (`addLamp` checks `if (lampPool.size() < MAX_POOL_SIZE)`).
- **Fix**:
  ```cpp
  void BluetoothPool::pruneLamps() {
      uint32_t timeNow = millis();
      auto it = lampPool.begin();
      while (it != lampPool.end()) {
          if (it->lastSeenTimeMs + LAMP_PRUNE_TIME_MS < timeNow) {
              it = lampPool.erase(it);
          } else {
              ++it;
          }
      }
  }
  ```
  Apply the same fix to `pruneStages()`.

---

### C5 — Post-`saveSettings` reconnect calls `initialize()` without `cleanup()` — stale GATT handle on Android

- **Where**: `software/lamp-app/src/stores/lamp.ts:368-373`
- **What**: After `writeSettingsBlob` the firmware reboots, dropping the GATT connection. The app waits 5 seconds then calls `initialize(savedTarget)` directly, without first calling `cleanup()`. `initialize()` does call `BleClient.stopLEScan()` but NOT `BleClient.disconnect()`. On Android, a previous GATT object in error state ("still connecting" or "disconnected but not cleaned up") is not released, and the subsequent `BleClient.connect()` will fail or produce a stale GATT cache.
- **Why it matters**: After every "Save Changes," the reconnect fails on Android, leaving the user stuck on the connecting spinner until they manually back out and re-open the lamp. On iOS this is less likely to manifest.
- **Fix**:
  ```typescript
  setTimeout(async () => {
      try {
          await cleanup()           // <-- add this
          await initialize(savedTarget)
      } catch (err) { ... }
  }, 5_000)
  ```

---

## 🟡 Important

### I1 — `settings_blob` read: `onRead` calls `setValue` from the BLE host task; `s_config` is mutated from the Arduino loop task — torn read risk

- **Where**: `software/lamp-os/src/components/network/ble_control.cpp:227-232`
- **What**: `SettingsBlobCallback::onRead` calls `s_config->asJsonDocument()` and `c->setValue(json.c_str())`. `s_config` is also written by `dispatchLampAction` from the loop task (e.g., `s_config->base.knockoutPixels` in `knockout` action). NimBLE `onRead` callbacks run on the BLE host task. Although the write itself here is safe (read-only), reading `s_config` while `loop()` modifies it (knockoutPixels, brightness levels via the struct) can return partially updated config JSON.
- **Why it matters**: The app could read a settings blob where some fields are half-updated, leading to an inconsistent state being sent back in the next `writeSettingsBlob`. This would silently overwrite lamp config with corrupted data.
- **Fix**: Same queue/lock solution as C2. Short-term: add `portENTER_CRITICAL` / `portEXIT_CRITICAL` around `s_config->asJsonDocument()` in `onRead`.

---

### I2 — MTU stays at 23 bytes unless the Capacitor plugin sends Exchange MTU; `TARGET_MTU = 512` is defined but never requested by the app

- **Where**: `software/lamp-app/src/services/bleControl.ts:23` (`TARGET_MTU` constant, unused), `software/lamp-os/src/components/network/ble_control.cpp:54` (`NimBLEDevice::setMTU(512)`)
- **What**: `NimBLEDevice::setMTU(512)` sets the peripheral's preferred MTU via `ble_att_set_preferred_mtu`. However, in BLE 4.x, the central (client) must initiate the MTU exchange (`Exchange MTU Request`). The firmware cannot force this. The `@capacitor-community/bluetooth-le` plugin's `BleClient.connect()` does NOT automatically send an MTU exchange request on iOS (Core Bluetooth ignores preferred MTU on peripherals; iOS uses 185 bytes by default for notifications). On Android, the plugin does not call `requestMtu()`. The `TARGET_MTU` constant in `bleControl.ts` is dead code — it's exported but never passed to any plugin call.
- **Why it matters**: With a 23-byte MTU (default), reading `settings_blob` (typically 400-600 bytes) requires 20+ ATT Read Blob Request round trips. Each has a latency of ~50-100ms on iOS. Total read time: 1-5 seconds. Also, `writeSettingsBlob` with a large JSON will fail if the ATT write size exceeds the MTU — `BleClient.write` does NOT fragment long writes automatically; it sends a single ATT Write Request limited to `MTU - 3` bytes. A 500-byte settings write at 23-byte MTU = 20-byte payload = write fails with ATT error `0x0D` (attribute too long).
- **Why it matters (write side)**: `writeSettingsBlob` is critical — this is how settings are saved. If MTU is 23 bytes and the settings JSON > 20 bytes, the write SILENTLY fails on the firmware side (returns `BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN`, which NimBLE logs but the app doesn't see — `BleClient.write` may throw or silently succeed depending on plugin version).
- **Fix**: 
  1. On Android: call `await BleClient.requestMtu(deviceId, 512)` in `initialize()` immediately after connecting.
  2. On iOS: Core Bluetooth 5.0+ supports `maximumWriteValueLength(for:)` up to 512 bytes automatically after connection. You can't call `requestMtu` on iOS via this plugin, but iOS negotiates ~185-512 automatically on modern hardware.
  3. Remove the dead `TARGET_MTU` export from `bleControl.ts` to avoid confusion, or use it.
  4. On firmware: `setDataLen` (line 53) sets the LL data length to 512, which is a separate concept from ATT MTU and only helps throughput, not the max write payload size.

---

### I3 — `writeExpressionComplete` sends a zero-byte write; some BLE stacks reject ATT WRITE with 0 payload

- **Where**: `software/lamp-app/src/services/bleControl.ts:105`
- **What**: `textToDataView('')` creates a `DataView` of a 0-byte buffer. The plugin calls `BleClient.write` (ATT Write Request) with 0 bytes. The ATT spec allows 0-byte values for WRITE_WITHOUT_RESPONSE but for WRITE_WITH_RESPONSE (`NIMBLE_PROPERTY::WRITE`), behavior is implementation-defined. iOS Core Bluetooth and some Android BLE stacks reject or silently drop 0-byte writes.
- **Why it matters**: Expression preview mode never exits on iOS/certain Android versions — the lamp stays in test mode until power-cycled.
- **Fix**: Send a single sentinel byte instead:
  ```typescript
  export async function writeExpressionComplete(deviceId: string): Promise<void> {
    // Firmware treats both empty string and "complete" as test_expression_complete.
    // Send "complete" as 1+ bytes to avoid 0-byte write rejection by BLE stacks.
    await BleClient.write(deviceId, CONTROL_SERVICE_UUID, CHAR_EXPRESSION_TEST, textToDataView('complete'))
  }
  ```

---

### I4 — `addOrUpdateLamp` updates only the timestamp, not the color data — color-sync pool never reflects live color changes from other lamps

- **Where**: `software/lamp-os/src/components/network/bluetooth_pool.cpp:25-36`
- **What**: When a known lamp (matched by name) broadcasts a new advertisement, `addOrUpdateLamp` only updates `lastSeenTimeMs`. The `baseColor` and `shadeColor` from the new advertisement are discarded. The social behavior `SocialBehavior` reads `baseColor` / `shadeColor` from the pool, but it will always see the first-ever color broadcast from each lamp.
- **Why it matters**: Social color-sync between lamps doesn't work correctly after any lamp changes its colors. Lamps will "sync" to each other's boot-time colors, not their current colors.
- **Fix**:
  ```cpp
  void BluetoothPool::addOrUpdateLamp(BluetoothLampRecord lamp) {
      for (int i = 0; i < lampPool.size(); i++) {
          if (lampPool[i].name == lamp.name) {
              lampPool[i].lastSeenTimeMs = millis();
              lampPool[i].baseColor  = lamp.baseColor;   // <-- add
              lampPool[i].shadeColor = lamp.shadeColor;  // <-- add
              return;
          }
      }
      addLamp(lamp);
  }
  ```

---

### I5 — BLE manufacturer data (color beacon) is set once at `begin()` and never updated when lamp colors change

- **Where**: `software/lamp-os/src/components/network/bluetooth.cpp:134-151`
- **What**: `setManufacturerData` is called once with the boot-time colors. When the lamp's colors change (via BLE GATT write, WebSocket, or DMX), the advertisement manufacturer data is never refreshed. `NimBLEAdvertising::refreshAdvertisingData()` is available in NimBLE 2.x and would update the advertised packet mid-advertising.
- **Why it matters**: Other lamps scanning for color-sync see stale colors. The app's scan view shows stale colors. This breaks the core social/color-sync feature.
- **Fix**: Add a `BluetoothComponent::updateColors(Color base, Color shade)` method that calls `pAdvertising->setManufacturerData(data)` then `pAdvertising->refreshAdvertisingData()`. Call it from `dispatchLampAction` when handling `base` or `shade` actions (note: do this on the loop task, not from the BLE callback, since `refreshAdvertisingData` calls GAP APIs).

---

### I6 — `toApMode()` is called from `handleStageMode()` on every stage→AP transition; it calls `NimBLEDevice::getAdvertising()->start()` which was already running

- **Where**: `software/lamp-os/src/lamps/standard_lamp.cpp:143`, `software/lamp-os/src/components/network/wifi.cpp:245-288`
- **What**: `handleStageMode()` calls `wifi.toApMode()` whenever stage mode is lost (stages list goes empty). `toApMode()` runs the full GATT setup + `server->start()` + `advertising->start()` sequence. `NimBLEServer::start()` has an `m_gattsStarted` guard, so the GATT part is idempotent. `NimBLEAdvertising::start()` also returns early if already advertising. However, between the `scan->stop()` and `scan->start(0)` calls at lines 263-286, there is a window where the scan is paused. If `toApMode` is called many times (e.g., intermittent stage signal), the scan is repeatedly stopped/started, potentially causing NimBLE state machine issues (especially if a connection is in progress).
- **Why it matters**: Frequent stage→AP transitions could disrupt an active mobile app BLE session. Low risk but should be guarded.
- **Fix**: Add a guard: only run the BLE portion of `toApMode()` on first call. For subsequent calls, only the WiFi mode switch needs to happen.

---

### I7 — `BLE_GAP_ADV_INTERVAL_MS` (named for advertising) is used as the SCAN interval — naming confusion could cause future bugs

- **Where**: `software/lamp-os/src/components/network/bluetooth.hpp:19`, `software/lamp-os/src/components/network/bluetooth.cpp:129`
- **What**: `BLE_GAP_ADV_INTERVAL_MS = 1000` is passed to `pScan->setInterval(BLE_GAP_ADV_INTERVAL_MS)`. The name implies it's an advertising interval but it's actually the scan interval. There is a separate `BLE_GAP_SCAN_INTERVAL_MS = 400` that is defined but NEVER USED anywhere. The wrong constant is applied.
- **Why it matters**: A scan interval of 1000ms at 15ms window = 1.5% duty cycle — the lamp will only be scanning 1.5% of the time. At 400ms/15ms = 3.75% duty cycle. This makes color-sync detection of nearby lamps significantly slower.
- **Fix**: 
  1. Delete `BLE_GAP_ADV_INTERVAL_MS`.
  2. Use `BLE_GAP_SCAN_INTERVAL_MS` for `pScan->setInterval()`.
  3. Or rename and consolidate so the constant names clearly indicate their purpose.

---

### I8 — Scan restarted with `start(0)` (continuous) in `wifi.cpp` but `onScanEnd` restarts with `BLE_GAP_SCAN_TIME_MS` (1000ms periodic) — inconsistent scan mode

- **Where**: `software/lamp-os/src/components/network/wifi.cpp:285`, `software/lamp-os/src/components/network/bluetooth.cpp:110`
- **What**: `wifi.cpp::toApMode()` restarts the scan as `scan->start(0)` (infinite/continuous). When NimBLE's scan encounters an error or is stopped externally, `onScanEnd` fires and calls `NimBLEDevice::getScan()->start(BLE_GAP_SCAN_TIME_MS)` (1000ms). After this, scan runs in 1000ms cycles with `onScanEnd` callbacks rather than continuously. The modes are inconsistent: the initial restart is continuous, but any error recovery reverts to periodic.
- **Why it matters**: After any scan disruption (e.g., WiFi channel switch, GAP event), the lamp silently degrades to periodic 1-second scan cycles. Not a crash, but subtly degrades color-sync responsiveness.
- **Fix**: In `onScanEnd`, restart with `start(0)` instead of `start(BLE_GAP_SCAN_TIME_MS)` to maintain continuous scan mode.

---

### I9 — `ble_setup::stop()` and `ble_control::stop()` call `NimBLEDevice::getAdvertising()->stop()` but don't re-enable advertising — any `stop()` call silences the lamp permanently

- **Where**: `software/lamp-os/src/components/network/ble_setup.cpp:148`, `software/lamp-os/src/components/network/ble_control.cpp:361`
- **What**: Both `stop()` implementations begin with `NimBLEDevice::getAdvertising()->stop()`. If `stop()` is ever called (e.g., during a factory reset flow, or future feature that temporarily disables BLE control), advertising is not restarted. There's no `getAdvertising()->start()` at the end of `stop()`.
- **Why it matters**: Not triggered today since neither `stop()` is called at runtime, but the implementation is a landmine. A future developer calling `ble_control::stop()` expecting to just remove the GATT service will kill advertising too.
- **Fix**: Remove the `getAdvertising()->stop()` lines from both `stop()` implementations. Let the caller manage advertising lifecycle explicitly. Or restart advertising at the end of `stop()`.

---

### I10 — `CORE_DEBUG_LEVEL=0` suppresses all NimBLE internal error logs while `LAMP_DEBUG` is active — firmware errors are silent

- **Where**: `software/lamp-os/platformio.ini:62`
- **What**: The build flags include both `-D LAMP_DEBUG` (enables our Serial.printf logs) and `-D CORE_DEBUG_LEVEL=0` (suppresses all IDF/NimBLE `ESP_LOGE`, `NIMBLE_LOGE`, `NIMBLE_LOGW` output). If NimBLE's internal functions fail (e.g., `ble_gatts_start` returns an error, `ble_gap_adv_start` fails, MTU exchange fails), the only log is the return code check in `NimBLEServer.cpp` which is also guarded by `NIMBLE_LOGE` — which is suppressed.
- **Why it matters**: Silent failures. When debugging the bugs above (GATT registration, advertising restart, MTU issues), there is no NimBLE-level error output. The developer only sees our application-level Serial.printf statements.
- **Fix**: For development builds, set `CORE_DEBUG_LEVEL=3` (INFO) or at minimum `CORE_DEBUG_LEVEL=1` (ERROR):
  ```ini
  -D CORE_DEBUG_LEVEL=1  ; 0=none, 1=error, 2=warn, 3=info, 4=debug
  ```
  This will not significantly impact performance but will surface NimBLE errors. Document this in `platformio.ini` as intentionally set to 0 for production.

---

## 🟢 Minor

### M1 — `ControlServerCallbacks` allocated with `new` and registered with `deleteCallbacks=false` — memory leaked on `ble_control::stop()` + `start()` cycle

- **Where**: `software/lamp-os/src/components/network/ble_control.cpp:291`
- **What**: `s_server->setCallbacks(new ControlServerCallbacks(), false)`. The `false` means NimBLE will not free the callbacks object. When `ble_control::stop()` is called followed by `start()`, `createServer()` returns the existing server, `setCallbacks` is called again with a new `ControlServerCallbacks()`, and the old one is leaked. This path isn't exercised today but could be (factory reset).
- **Fix**: Store the pointer and delete it in `stop()`:
  ```cpp
  static ControlServerCallbacks* s_serverCallbacks = nullptr;
  // in start():
  s_serverCallbacks = new ControlServerCallbacks();
  s_server->setCallbacks(s_serverCallbacks, false);
  // in stop():
  delete s_serverCallbacks; s_serverCallbacks = nullptr;
  ```
  Or use `deleteCallbacks=true` and simply create a new one each `start()`.

---

### M2 — `NimBLECharacteristic` callbacks (`AuthCallback`, `BrightnessCallback`, etc.) are heap-allocated with `new` and never freed on `stop()`

- **Where**: `software/lamp-os/src/components/network/ble_control.cpp:297-340`
- **What**: Every `createCharacteristic(...)->setCallbacks(new XxxCallback())` call allocates a callback on the heap. `NimBLECharacteristic::~NimBLECharacteristic` (line 53 of `NimBLECharacteristic.cpp`) does NOT free `m_pCallbacks` — it only frees descriptors. When `removeService(s_service, true)` is called in `stop()`, the characteristic objects are freed, but not their registered callback objects.
- **Fix**: Use static instances (no heap) or store pointers and free in `stop()`. For the current firmware lifecycle (callbacks live until reboot), this is only a concern if `stop()`/`start()` cycles happen at runtime.

---

### M3 — Service UUID not added to scan response despite `enableScanResponse(true)` being set

- **Where**: `software/lamp-os/src/components/network/bluetooth.cpp:140`
- **What**: `pAdvertising->enableScanResponse(true)` reserves the scan response PDU but nothing is added to it. The 128-bit control service UUID is intentionally omitted from the primary advertisement (31-byte limit with manufacturer data + name). The scan response PDU (another 31 bytes) is available but unused. Adding the service UUID there would allow the app's `scanForLamps` to detect configured lamps by UUID even when their manufacturer data format changes.
- **Fix**: Consider `pAdvertising->addServiceUUID(ble_control::SERVICE_UUID)` — NimBLE automatically places overflow UUIDs into scan response when `enableScanResponse` is true. This would fix the lamp detection path for future firmware that doesn't use manufacturer data encoding.

---

### M4 — `debug` panel in `Lamp.vue` is permanently open (`<details open>`) — leaks lamp name, connection steps, and password auth status to anyone looking at the screen

- **Where**: `software/lamp-app/src/layout/Lamp.vue:130`
- **What**: `<details class="lamp-debug-panel" open>` is always expanded on every lamp page. It shows BLE connection steps including "password: `<set>`" confirmation and all GATT discovery details.
- **Fix**: Remove the `open` attribute before any production release. Or gate the entire debug panel behind a build flag / developer mode toggle.

---

### M5 — `AddLamp.vue` scans `debug` panel also permanently open

- **Where**: `software/lamp-app/src/pages/AddLamp.vue:169`
- **What**: Same issue as M4 — `<details ... open>` exposes raw BLE scan data.
- **Fix**: Same as M4.

---

### M6 — `ble_setup::start()` uses module-level static pending values (`pendingSsid`, etc.) — multiple concurrent setup connections could corrupt them

- **Where**: `software/lamp-os/src/components/network/ble_setup.cpp:18-20`
- **What**: `pendingSsid`, `pendingPassword`, and `pendingName` are module-level statics shared across all connections. If two BLE clients are connected simultaneously and both write to `CHAR_SSID`, the second write overwrites the first. The `apply` write then uses whatever is in the staging area.
- **Why it matters**: Low-risk in practice (setup lamps rarely have multiple clients), but worth noting. If a connection glitch causes a partial write to be reused in the next connection's apply, wrong credentials could be saved.
- **Fix**: Use the connection handle from `connInfo` to key a per-connection staging map (same pattern as `s_connAuth` in `ble_control.cpp`).

---

### M7 — Auth check logic: `state.value.lamp?.password` check to decide whether to authenticate is wrong — if lamp has no password set in config, app skips auth even if `newTarget.password` is provided

- **Where**: `software/lamp-app/src/stores/lamp.ts:474`
- **What**: `if (newTarget.password && state.value.lamp?.password)` — both conditions must be true. If the lamp has a password configured but the settings blob didn't include it (e.g., the firmware redacts the password field in the JSON response for security), `state.value.lamp?.password` will be falsy and auth will be skipped, making all subsequent writes fail silently.
- **Current behavior**: `asJsonDocument()` in `config.cpp:144` only includes password if `!lamp.password.empty()`. If the lamp has a password, it IS included in the settings blob. So today this is OK. But it's fragile — if that serialization ever changes (security audit, redaction), auth breaks silently.
- **Fix**: Auth should be triggered if `newTarget.password` is provided regardless of what the settings blob says:
  ```typescript
  if (newTarget.password) {
      await authConnection(newTarget.deviceId, newTarget.password)
  }
  ```

---

### M8 — `handleStageMode` in `standard_lamp.cpp` names `foundStages->at(0)` without checking index 0 existence (size check is on `size() == 0` vs `size() > 0`)

- **Where**: `software/lamp-os/src/lamps/standard_lamp.cpp:144-146`
- **What**: The condition checks `foundStages->size() > 0` before calling `foundStages->at(0)`, which is fine. But `at(0)` throws `std::out_of_range` if the vector is empty — and the check uses `> 0` so if size is exactly 0, this branch isn't taken. This is actually correct. Not a bug, just note that `[]` operator would be safer style.

---

### M9 — iOS plist: only `NSBluetoothAlwaysUsageDescription` present; `NSBluetoothPeripheralUsageDescription` not needed for iOS 13+ but worth documenting

- **Where**: `software/lamp-app/ios/App/App/Info.plist:50`
- **What**: The plist contains only `NSBluetoothAlwaysUsageDescription`. On iOS 13+, `NSBluetoothPeripheralUsageDescription` is deprecated and only `NSBluetoothAlwaysUsageDescription` is required. This is correct. Document this explicitly so no one adds `NSBluetoothPeripheralUsageDescription` back in a misguided attempt to "fix" BLE permissions.

---

### M10 — `NimBLEDevice::setMTU` is called in `onConnect` (line 54) but this has no effect for the current connection — it sets the preferred MTU for FUTURE connections only

- **Where**: `software/lamp-os/src/components/network/ble_control.cpp:54`
- **What**: `NimBLEDevice::setMTU(TARGET_MTU)` sets the preferred MTU globally via `ble_att_set_preferred_mtu`. This is best called once at initialization (e.g., in `start()`), not in `onConnect`. Calling it in `onConnect` is redundant after the first connection.
- **Fix**: Move to `ble_control::start()`, before creating the server. Document that MTU exchange is initiated by the central (client), not the peripheral.

---

## ✅ Checked & OK

- **`NimBLEServer::start()` idempotency**: Has `m_gattsStarted` guard — safe to call from `toApMode()` on repeated invocations.
- **CCCD auto-creation for `NOTIFY` characteristics**: NimBLE's `ble_gatts.c` (line 253, 316) auto-creates CCCDs when `BLE_GATT_CHR_F_NOTIFY` flag is set. No manual CCCD descriptor needed. `state_notify` will work correctly.
- **Long reads for `settings_blob`**: NimBLE correctly handles multi-chunk reads at the stack level. `handleGattEvent` appends the full stored value to the mbuf; NimBLE slices it into ATT Read Blob responses. No application-level long-read handling needed. `BleClient.read` in the Capacitor plugin reassembles chunks automatically.
- **`NimBLEServer::removeService(service, true)` in `stop()`**: The `deleteSvc=true` parameter correctly deletes the service and its characteristics.
- **`setCallbacks(..., false)` for `ControlServerCallbacks`**: Correct — prevents NimBLE from deleting a callback we allocated. The `false` is intentional and documented in the code comment.
- **Android permissions**: `BLUETOOTH_SCAN` with `neverForLocation` flag + `BleClient.initialize({ androidNeverForLocation: true })` are correctly paired. `ACCESS_FINE_LOCATION` is correctly restricted to API 30 and below.
- **iOS BLE permissions**: `NSBluetoothAlwaysUsageDescription` is present and sufficient for iOS 13+.
- **Scan-stops-before-GATT-registration**: Already fixed — `wifi.cpp::toApMode()` stops scan before calling `ble_setup::start()` and `ble_control::start()`.
- **`NimBLEAdvertising::start()` when already advertising**: Returns early with a warning log — idempotent. Safe for the repeated `toApMode()` calls.
- **`ble_setup::start()` and `ble_control::start()` idempotency**: Both check their `running`/`s_running` guard.
- **`addServiceUUID` overflow**: Service UUID correctly omitted from primary advertisement (31-byte limit). Decision is documented in `ble_control.cpp:346-348`.
- **Connection limit (3)**: The lamp acts as peripheral only. Scanning (observer role) does not consume connection slots. 3 simultaneous GATT connections from phones is adequate.
- **`s_config` null check**: `s_config` is set before any characteristic callbacks can fire (set in `start()`, callbacks only fire after `server->start()` + `advertising->start()`). Safe.
- **`Preferences` two-instance concern**: Both `wifi.cpp::prefs` and `standard_lamp.cpp::prefs` access the same NVS namespace `"lamp"` / key `"cfg"`. They don't overlap in their `begin()`/`end()` calls. The underlying NVS partition is shared correctly.
- **`onSync` callback**: Not overridden — NimBLE default behavior handles re-sync after BT controller reset. Advertising restarts on sync (if `advertiseOnDisconnect` were set — see C1).
- **Router architecture**: `Lamp.vue` is a layout component with child routes. Navigating between lamp tabs (home/expressions/setup/info) does NOT unmount `Lamp.vue`, so BLE connection is preserved across tab switches. `onUnmounted` only fires when leaving `/lamp/:id` entirely.
- **Inventory schema**: `InventoryLamp` uses `JSON.parse` with a try/catch on load. Old entries with extra fields (e.g., `lastIp`) are silently loaded with extra properties — TypeScript interface is not enforced at runtime, but the extra fields are harmless.

---

## 📝 Recommendations

### R1 — Introduce a thread-safe action queue between BLE callbacks and the main loop

The root cause of C2 and I1 is that BLE callbacks (NimBLE host task, Core 0) directly modify state that `loop()` (Arduino loop, Core 1) also reads/writes. A lightweight `QueueHandle_t` with a fixed-size ring buffer (10-20 entries) would completely decouple these. The queue should be created at BLE init time. `dispatchLampAction` should only be called from the loop task.

### R2 — Unify `Preferences` instances

Currently `standard_lamp.cpp` and `wifi.cpp` each declare their own `Preferences prefs`. While they happen to work (both access the same NVS namespace/key, and their `begin()`/`end()` calls don't overlap), this is confusing and fragile. Pass `prefs` from `standard_lamp.cpp` into `wifi.cpp` via `WifiComponent::begin()`, and stop declaring `Preferences prefs` in `wifi.cpp`. This eliminates the dual-instance footgun.

### R3 — Color-sync manufacturer data needs a live-update path

`BluetoothComponent` currently exposes no method to update the advertised colors. This is a core feature gap — the social/color-sync feature works on boot-time colors only. Add `BluetoothComponent::updateBeaconColors(Color base, Color shade)` that calls `setManufacturerData` + `refreshAdvertisingData()`. Call it from `dispatchLampAction` after handling `base` and `shade` actions, on the loop task (not from BLE callbacks, since GAP calls are not safe from the BLE host task context).

### R4 — Implement auth verification before adding to inventory

Currently `AddLamp.vue` adds to inventory and navigates to the lamp page without verifying the password. Auth is done lazily in `initialize()` after loading settings, and a wrong password shows a generic "Couldn't read settings" error (which happens because `readSettingsBlob` succeeds fine — it's subsequent writes that fail silently). Consider writing to a test characteristic immediately after `authConnection()` and checking the response to verify auth before saving to inventory.

### R5 — Document the `advertiseOnDisconnect` architectural decision once fixed

Once C1 is fixed, add a comment in `ble_control::start()` explaining why `advertiseOnDisconnect(true)` is set — specifically that NimBLE 2.x changed the default from `true` to `false`, and the CHANGELOG entry documenting this as a known migration gap (CHANGELOG line 133). This prevents future regression when upgrading NimBLE versions.

### R6 — Consider `INDICATE` instead of `NOTIFY` for `state_notify`

`NOTIFY` is fire-and-forget with no acknowledgement. If the mobile app is busy processing when a notification arrives, it may be dropped. For the `state_notify` characteristic (which triggers a settings re-read), a dropped notification means the app's UI falls out of sync with the firmware. `INDICATE` (with ACK) would be more reliable but has higher latency. Given that `state_notify` fires only on significant events (brightness change, settings save), the latency trade-off is acceptable. Consider switching.

### R7 — The `saving.value = false` is only set in the success/error paths of post-save reconnect, not in the timeout path

- **Where**: `software/lamp-app/src/stores/lamp.ts:379` — `saving.value = false` is only set if `target.value` is null. The `setTimeout` callback sets `saving.value = false` only in the catch branch. If `initialize` succeeds in the setTimeout callback, `saving.value = false` is never explicitly reset — it's left as `true` until `initialize` completes (which calls `saving.value = false` at line 497). This is technically correct but fragile. If `initialize` throws and falls through without hitting the catch, `saving.value` stays `true`.

### R8 — `BLE_ADVERTISING_INTERVAL_MIN=400`, `BLE_ADVERTISING_INTERVAL_MAX=650`

These are in units of 0.625ms as per BLE spec (so 250ms and 406ms respectively). This is on the high side for a connectable peripheral that users need to discover quickly. Consider `160`/`240` (100ms/150ms) for better discoverability at a modest power cost. The BLE connection itself is established once and maintained, so discoverability interval matters more than power during idle advertising.

---

*Audit performed by reviewing all listed source files, NimBLE-Arduino 2.3.6 library source (`NimBLEServer.cpp`, `NimBLECharacteristic.cpp`, `NimBLEService.cpp`, `NimBLEAdvertising.cpp`, `ble_gatts.c`, `NimBLELocalValueAttribute.h`, `nimconfig.h`), the library CHANGELOG, and the NimBLE migration guide.*
