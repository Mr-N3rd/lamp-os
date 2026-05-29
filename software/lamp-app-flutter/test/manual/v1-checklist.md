# V1 manual test checklist

Hardware verification for things automated tests can't cover. Walk through this before each TestFlight / internal build.

## Setup

- `flutter run --release -d <phone-id>` against a real lamp powered on in the room.

## Phase 2 — Control screen

### Happy path

- [ ] After Adopt, app lands on Control screen with `ConnectingView` showing the critter + "Connecting…" briefly.
- [ ] Control loads with the lamp's current brightness, shade, and base populated (not defaults).
- [ ] Brightness slider visibly changes the lamp's brightness while dragging.
- [ ] Releasing the slider leaves the lamp at the released value (no snap-back).
- [ ] Tapping the Shade card opens the color picker; picking a color + Save updates the lamp's shade.
- [ ] Dismissing the color picker with Cancel leaves the lamp shade unchanged.
- [ ] Tapping the Base card opens the editor sheet.
- [ ] Tapping a stop swatch in the editor opens the color picker; Save updates the lamp's base live.
- [ ] Adding a stop pushes a new white stop; the lamp's gradient reflects it.
- [ ] Reordering stops via drag changes the lamp's gradient in the new order.
- [ ] Removing a stop down to 1 disables the ✕ on the remaining stop.
- [ ] Tapping a stop (not its swatch) marks it as active; the active pink ring jumps to that stop.
- [ ] Backing out of the lamp and re-entering reads the lamp's current state (no stale data).

### Failure modes

- [ ] If the lamp is off / out of range, the screen shows the error message gracefully (centered, fogGrey text) instead of a stack trace.
- [ ] If the lamp loses power mid-session, the next write throws but the UI does not crash.
- [ ] Wrong password on an adopted lamp: writes silently no-op firmware-side; UI still appears to work. Known limitation — file a follow-up if it confuses real users.

### Critter friend

- [ ] Same lamp shows the same critter on every reconnect.
- [ ] Different lamps show different critters (within the set of 4).

## Phase 2.1 — Polish + persistence

### Realtime preview

- [ ] Open shade picker, drag the hue ring → lamp shade changes live while dragging (no need to tap Save).
- [ ] Tap Cancel in the shade picker → lamp reverts to the previous shade.
- [ ] Open base editor, tap a stop, drag in the color picker → that stop on the lamp updates live.
- [ ] Cancel in the per-stop picker → that stop reverts on the lamp.
- [ ] LampPreview critter under the brightness slider mirrors the picker in realtime — its shade matches the chosen shade color, its body shows the base gradient.

### Base editor polish

- [ ] Close (×) icon in the top-right of the Base editor sheet pops the sheet.
- [ ] Sheet height looks proportional to its content (≈60% screen, not full).

### App bar + Save

- [ ] App bar shows the lamp's friendly name (e.g. `jacko`), not the BLE device id.
- [ ] Save icon in the app bar is disabled when no edits have been made.
- [ ] After any change (brightness, shade, base), Save becomes enabled.
- [ ] Tap Save → screen shows ConnectingView for ~5-8s while the lamp fades out, reboots, and re-loads.
- [ ] After Save completes, edits are reflected in the freshly loaded state and Save is back to disabled.
- [ ] Power-cycle the lamp manually → all saved values persist (brightness, base colors, base active, shade).
- [ ] Expressions / Setup tabs do NOT show the Save icon (Control-tab-only for now).

## Phase 2.2 — Resilience (RGBW picker + disconnect handling)

### RGBW slider picker

- [ ] Shade picker shows 4 sliders (R, G, B, Warm White) with the lamp's current values pre-set.
- [ ] Dragging any slider updates the lamp and the LampPreview critter live.
- [ ] Hex string in the picker header updates as you drag.
- [ ] Picker swatch reflects the warm-white channel: pulling W up with RGB=0 turns the swatch toward warm orange; with RGB=255,255,255 the swatch stays white.
- [ ] Pulling Warm White down from 255 (default shade) makes the shade darken / colour become visible on the lamp.
- [ ] Base editor: tap a stop → picker shows R/G/B + Warm White; same behaviour.
- [ ] Cancel still reverts; Save commits the final values and the AppBar's Save action enables.

### Disconnect + auto-reconnect

- [ ] During rapid shade-drag editing the link no longer drops every few seconds (cached-service fix). Run for ≥30s of continuous slider movement.
- [ ] Walk the phone out of BLE range while editing — within a second or two, an amber "Reconnecting…" banner appears at the top of Control.
- [ ] Sliders and pickers stay interactive while disconnected (writes are silently dropped behind the scenes).
- [ ] AppBar Save icon disables while disconnected; tooltip reads "Reconnecting…".
- [ ] Walk back into range → banner clears within a few seconds; the lamp catches up to the last local values without losing the user's session.
- [ ] After reconnect, any unsaved edits from before the drop are still present and Save re-enables.
- [ ] Power-cycle the lamp manually → banner appears on the immediate disconnect, persists through the ~5 s boot, then clears once the lamp re-advertises.

## Phase 1c — Multi-lamp switching

### AppBar lamp chip

- [ ] AppBar shows the lamp's friendly name as a tappable chip with a status dot to its left.
- [ ] The status dot pulses green when connected; dim-green when in BLE range but not the current lamp; grey when out of range.
- [ ] Switching from Control → Expressions → back to Control no longer flashes the ConnectingView — the BLE link is held across tab switches.

### Picker contents

- [ ] Tap the chip → modal bottom sheet slides up showing "Your lamps".
- [ ] Each inventory row shows: status dot, lamp icon tinted by the lamp's last-seen colors, name.
- [ ] The currently-active lamp's row carries an "active" pill (no chevron).
- [ ] Inventory lamps currently within BLE range show a green/bluetooth status dot; others show grey.
- [ ] Power-cycle another lamp in the room → after ~30 s its status dot in the sheet transitions from green to grey (BLE adv staleness window).

### Switching lamps

- [ ] Tap a different inventory row → sheet pops, app navigates to that lamp's Control screen.
- [ ] After switching, the AppBar chip and ControlScreen reflect the new lamp.
- [ ] Switch back → previous lamp's Control state is loaded fresh (a new connect+section read; we don't currently cache other lamps' state).

### Other nearby lamps

- [ ] Bring an unconfigured ("standard") lamp near the phone → it appears under "Other nearby lamps" within seconds with an amber "adopt" pill.
- [ ] Tap it → AddLamp wizard opens (factory-default path; sheet pops).
- [ ] Bring a friend's already-configured lamp near the phone → it appears under "Other nearby lamps" with a green "add" pill.
- [ ] Tap → confirmation dialog → "Add" → lamp lands in inventory + becomes the active lamp. Sheet pops.
- [ ] Tap "+ Add a lamp" in the footer → onboarding shell opens.

### Live color cache

- [ ] After editing shade or base on a lamp and backing out without saving, re-opening the picker shows the inventory tile tinted by the *edited* colors (InventoryLamp.lastShadeColor / lastBaseColor cached on every live write).
