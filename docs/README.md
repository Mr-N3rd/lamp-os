# Docs

Map of what lives here and what's load-bearing for current work.

## Authoritative refs

The first place to look before changing anything cross-cutting.

- [`mesh-api.md`](mesh-api.md) — wire-format spec for every ESP-NOW mesh message. Code wins ties; update this doc when it doesn't.
- [`mesh-deployment.md`](mesh-deployment.md) — operational reference: the v0x03 production lock-in, BLE coex notes, empirical per-link recv rates, gossip-relay math, deployment checklist, troubleshooting playbook. Read before chasing a "mesh feels unreliable" report.
- [`expressions.md`](expressions.md) — developer guide to the lamp's expressions subsystem (auto-triggered animations). How to write a new one, how the wisp-override gate works, the testing pattern.

## Design specs

Specs that describe how a non-trivial subsystem was designed. They're not implementation plans (those rot the moment they ship); they document intent + reasoning that's still useful to revisit.

- [`superpowers/specs/2026-05-25-lamp-led-type-advanced-setting-design.md`](superpowers/specs/2026-05-25-lamp-led-type-advanced-setting-design.md) — why `byteOrder` exists in BaseSection/ShadeSection alongside `bpp`.
- [`superpowers/specs/2026-06-02-factory-reset-design.md`](superpowers/specs/2026-06-02-factory-reset-design.md) — the NVS-wipe contract + the "boot into adoption mode when NVS is empty" assumption.
- [`superpowers/specs/2026-06-02-mesh-expression-triggering-design.md`](superpowers/specs/2026-06-02-mesh-expression-triggering-design.md) — the `cascadeEnabled` / `cascadeStaggerMs` convention. Cross-linked from `expressions.md`.
- [`superpowers/specs/2026-06-02-social-tab-design.md`](superpowers/specs/2026-06-02-social-tab-design.md) — social subsystem design (dispositions, greetings, the per-peer state machine).
- [`superpowers/specs/2026-06-03-wisp-design.md`](superpowers/specs/2026-06-03-wisp-design.md) — wisp architecture overview.

## Operational notes

Live policy + audit findings that affect current work.

- [`superpowers/notes/2026-06-10-accepted-security-threats.md`](superpowers/notes/2026-06-10-accepted-security-threats.md) — the cleartext-secret threats we are NOT fixing (T1 wispOp setWifi PSK leak, T2 first-claim password) and why. Referenced from `// SECURITY:` comments at the affected call sites.
- [`superpowers/audits/2026-05-26-ble-stack-audit.md`](superpowers/audits/2026-05-26-ble-stack-audit.md) + [`-DEFERRED.md`](superpowers/audits/2026-05-26-ble-stack-audit-DEFERRED.md) — BLE stack audit findings. The DEFERRED variant lists items that require larger refactors; verify any item is still relevant before treating it as an action.

## What's NOT here (and why)

Implementation plans are deliberately absent. They have a short half-life: once a feature ships, the code is truth and the plan rots. If you find a stale plan, delete it. New work should track state in skill plans (under `~/.claude/plans/`), commits, and inline `// TODO:` comments — not in this tree.
