# Mesh-Wide Expression Triggering

## Context

Expressions (Glitchy, Pulse, Shifty, Breathing) currently run
independently on each lamp. There is no firmware primitive for "lamp A
tells lamp B to play this expression" or "all lamps glitch together with
these colors." Base color changes can already be sent to a specific lamp
via the existing `REMOTE_OP` path, but there is no notion of timed
overrides ("flash teal for 8s then fade back").

This design adds a flexible underlying primitive that supports those
patterns and future ones (boot-time mesh effects, social/proximity
reactions, etc.) without bolting on a new wire format per feature.

**Goal:** one canonical "fire this expression" verb across the mesh,
plus a thin sender-side API any code on the lamp can call to dispatch it
to one peer or all peers (with optional stagger). Temporary color
overrides become *just another expression*. No new clock-sync
infrastructure required.

## Architecture

### Key decisions

- **Expression invocation is the universal mesh verb.** A single new
  `char:"triggerExpression"` carries the existing `ExpressionConfig`-
  shaped payload — no per-effect verbs.
- **Sending is decoupled from configuration.** Propagation is *not* a
  field on `ExpressionConfig`. Instead, an explicit
  `sendExpressionTo` / `sendExpressionToAll` API is callable from
  anywhere (BLE handlers, expression `onComplete`, boot, social
  behavior, future hooks).
- **Stagger is sender-side.** Originator unicasts each peer with
  progressively larger `delayMs`. Receivers use their own `millis()`
  after message arrival — no clock sync.
- **Receivers never re-propagate.** Loops are structurally impossible;
  the ESP-NOW dedup ring is a backstop.
- **Cascade is a manager-level convention.** Any expression whose
  `parameters` map includes `cascadeEnabled: 1` and `cascadeStaggerMs: N`
  is fanned out on local trigger. No per-expression C++ code needed.
- **Temporary color override = new internal expression `ColorHold`.**
  Snapshots base, swaps colors, fades back on complete. Same mesh verb
  works for it.
- **`internalOnly` flag on `ExpressionConfig`** keeps things like
  `ColorHold` usable as primitives without surfacing them in the app's
  expression-config UI.

### Out of scope

- No automatic propagation of auto-scheduled expressions. Cascading is
  always opt-in, expressed in the expression's `parameters` map.
- No `setAllBaseColors` broadcast verb. App can iterate if it needs to.
- No clock sync / wall-clock scheduling between lamps.

## Components

### `ExpressionInvocation` struct (new)

`software/lamp-os/src/expressions/expression_invocation.hpp`

A subset of `ExpressionConfig` representing a one-time "fire this now":

```cpp
struct ExpressionInvocation {
  std::string type;                                  // "glitchy", "pulse", "colorHold", ...
  std::vector<Color> colors;                         // optional palette (empty = use configured)
  uint8_t target;                                    // SHADE=1 / BASE=2 / BOTH=3
  std::map<std::string, uint32_t> parameters;        // expression-specific (pulseSpeed, holdMs, ...)
  uint32_t delayMs = 0;                              // receiver delays this long before firing
};
```

Plus serialize/parse helpers:

- `std::string serializeInvocation(const ExpressionInvocation&)` — uses
  ArduinoJson.
- `bool parseInvocation(const JsonObjectConst&, ExpressionInvocation& out)`.

Reuses existing color hex helpers from `util/color.hpp`.

### Mesh send API on `ShowReceiver`

`software/lamp-os/src/components/network/show_receiver.hpp/cpp`

```cpp
class ShowReceiver {
  // ...existing...
  void sendExpressionTo(const std::string& peerName,
                        const ExpressionInvocation& inv);
  void sendExpressionToAll(const ExpressionInvocation& inv,
                           uint32_t staggerMs = 0,
                           bool includeSelf = false);
};
```

- `sendExpressionTo` looks up `peerName` in `NearbyLamps`; if `hasMac`,
  builds `{char:"triggerExpression", ...inv}` and calls existing
  `sendControlOp(mac, json)`. If no MAC yet, no-op (log it).
- `sendExpressionToAll` snapshots the peer list, iterates in
  deterministic order (by name), assigns each peer
  `delayMs += staggerMs * index`, and unicasts each. If `includeSelf`,
  the originator also fires locally with `delayMs = 0`.

### Receive handler

`software/lamp-os/src/lamps/standard_lamp.cpp` (in `applyRemoteOpLocal()`)

A new case for `char == "triggerExpression"`:

- Parse `ExpressionInvocation` from JSON.
- If `delayMs == 0`, call `expressionManager.triggerInvocation(inv)`
  immediately.
- Otherwise enqueue a small pending-trigger record
  (`{ExpressionInvocation, uint32_t fireAtMs}`) drained each loop
  iteration.
- **Never re-emit.** Loop break is structural.

### `ExpressionManager` — two trigger paths + generic cascade

`software/lamp-os/src/expressions/expression_manager.hpp/cpp`

```cpp
// LOCAL path: any trigger originating on this lamp (auto-schedule,
// BLE test, app, internal call). Honors the cascade convention.
void triggerExpression(const std::string& type, /* existing args */);

// REMOTE path: only called from applyRemoteOpLocal() when a
// triggerExpression message arrives. Overrides colors/params for this
// single firing. NEVER cascades.
void triggerInvocation(const ExpressionInvocation& inv);
```

**`triggerInvocation`:** find existing entries matching `inv.type` (and
`target`); for each, override runtime colors/parameters with the
invocation's values for this single firing, then call `trigger()`. If no
matching entry exists, no-op (revisit later if cascades feel
constrained).

**Cascade convention:** after firing via the *local* path, the manager
inspects the expression's `parameters` map for `cascadeEnabled` (0/1)
and `cascadeStaggerMs` (0..N). If `cascadeEnabled == 1`, build an
`ExpressionInvocation` from the expression's current state and call
`showReceiver.sendExpressionToAll(inv, cascadeStaggerMs,
/*includeSelf=*/false)`. The built invocation strips
`cascadeEnabled` / `cascadeStaggerMs` from `parameters` so receivers
don't see them (defense-in-depth against accidental re-cascade).

This makes cascade a free capability for any expression — Glitchy,
Pulse, and future expressions all opt in by setting two parameter keys.

### `internalOnly` flag on `ExpressionConfig` (slice 2)

`software/lamp-os/src/config/config_types.hpp`

```cpp
class ExpressionConfig {
  // ...existing...
  bool internalOnly = false;   // app filters these from configuration UI
};
```

Round-tripped through config JSON load/save. Firmware doesn't enforce —
purely metadata for the app to filter on.

### `ColorHoldExpression` (slice 2)

`software/lamp-os/src/expressions/color_hold_expression.hpp/cpp`

- Parameters (via `configureFromParameters`):
  - `holdMs` (default 5000)
  - `fadeInMs` (default 400)
  - `fadeOutMs` (default 1200)
- `onTrigger`: snapshot current `defaultColors` for the target, begin
  fade-in to invocation's colors.
- `onUpdate`: phase machine (fading in → holding → fading out) using
  existing `fadeLinear()` and `millis()` deltas.
- `onComplete`: restore snapshotted colors.
- Marked `internalOnly = true` by default.

Registered in `ExpressionManager`'s factory so
`triggerInvocation({type:"colorHold", ...})` resolves correctly.

### App: Glitchy cascade UI (slice 1)

`software/lamp-app-flutter/lib/features/lamp_shell/presentation/widgets/expression_params_panel.dart`

Extend `_GlitchyParams` (mirror the existing `_PulseParams` slider
pattern in the same file):

1. **Toggle** — `Switch` with label "Cascade to other lamps". Writes
   `parameters['cascadeEnabled'] = v ? 1 : 0`.
2. **Slider** (revealed when toggle is on) — `_ParamSlider` with `min:
   0, max: 5000, format: (v) => v == 0 ? 'instant' : '${(v/1000).
   toStringAsFixed(1)}s'`. Writes `parameters['cascadeStaggerMs']`.

`software/lamp-app-flutter/lib/features/lamp_shell/domain/expression_meta.dart`

Update Glitchy's `defaultParameters` to include `'cascadeEnabled': 0,
'cascadeStaggerMs': 0`.

Write path is unchanged: existing `expressionOp` upsert flow in
`control_notifier.dart` carries the new parameter keys with no new code.

## Wire format

ESP-NOW `MSG_CONTROL_OP` payload (JSON, fits within the ~230-byte
CONTROL_OP limit):

```json
{
  "char": "triggerExpression",
  "type": "glitchy",
  "colors": ["#FF1100", "#FF8800"],
  "target": 3,
  "parameters": {},
  "delayMs": 0
}
```

Staggered cascade: sender emits N of these per peer with `delayMs = 0,
staggerMs, 2*staggerMs, ...`.

## Verification

1. **Local triggers still work.** Existing BLE `expressionOp` writes
   behave identically.
2. **Two-lamp test, immediate broadcast.** Trigger Glitchy on A with
   cascade enabled + stagger=0. Both lamps glitch within one frame.
3. **Staggered cascade test, 3+ lamps.** Same with stagger=200. Each
   lamp fires ~200ms after the prior, by lamp name order.
4. **Unicast targeted trigger.** `sendExpressionTo("B", ...)` from A.
   Only B fires.
5. **Loop break.** Cascade enabled on all lamps; trigger once from A.
   Exactly one wave — no re-cascade.
6. **`ColorHold` end-to-end (slice 2).** Trigger `colorHold` on B with
   `holdMs:5000`. Fade-in → hold → fade back to original base colors.
7. **`internalOnly` round-trip (slice 2).** Set, write via
   `settings_blob`, reboot, confirm preserved.
8. **Wire size.** Confirm worst-case `triggerExpression` JSON
   (5 colors + parameters map + delayMs) fits within ESP-NOW's
   ~230-byte CONTROL_OP payload limit.

## Open items

- Whether `triggerInvocation` should upsert when no matching configured
  expression exists, or silently no-op. Default: no-op.
- Bounds on the receiver's delayed-trigger pending queue (size cap,
  behavior on overflow).
- Whether `sendExpressionToAll` should broadcast (single ESP-NOW frame)
  when `staggerMs == 0` for efficiency vs. always unicasting. Lean:
  unicast for now (simpler, deterministic per-peer addressability).
- Exact field name & JSON casing for `internalOnly` in app-facing
  payloads.
