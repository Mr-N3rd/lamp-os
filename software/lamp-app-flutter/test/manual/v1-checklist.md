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
