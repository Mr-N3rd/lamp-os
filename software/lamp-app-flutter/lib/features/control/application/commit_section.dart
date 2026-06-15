// lib/features/control/application/commit_section.dart
//
// Local-knowledge tag that per-pane call sites pass to
// `controlNotifier.commit()` to identify which section they just
// edited. CHAR_COMMIT itself is parameterless on the wire — the
// firmware persists whatever's in RAM regardless of section. The
// section IS used by the (rare) writeSettingsBlob fallback when a
// caller wants the equivalent partial-blob behavior on a pre-Phase-A
// lamp; that path is opt-in per-call.

enum CommitSection {
  lamp,
  base,
  shade,
  homeMode,
  baseKnockout,
}
