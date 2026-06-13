/// Release channel of THIS Flutter app build. Hardcoded for v1; switch
/// to a `--dart-define=APP_CHANNEL=beta` plumbing or a build flavor
/// later when the release flow is formalized.
///
/// Possible values: `'dev'`, `'beta'`, `'stable'`. The string is
/// rendered verbatim on the Info tab next to the version number, e.g.
/// `App 1.0.0+1 (dev)`.
const String kAppChannel = 'dev';

/// Decode the firmware's packed semver (uint32) into a `M.m.p` string.
///
/// Layout matches `firmware/src/version.hpp`:
///   `(major << 16) | (minor << 8) | patch`
///
/// Each component is masked to the low byte; an all-ones / corrupt
/// payload renders as `255.255.255` rather than overflowing.
String formatFirmwareSemver(int packed) {
  final major = (packed >> 16) & 0xff;
  final minor = (packed >> 8) & 0xff;
  final patch = packed & 0xff;
  return '$major.$minor.$patch';
}
