# LampOS Flutter app

Flutter (Dart) rewrite of the Capacitor + Vue lamp control app.
Lives in the `flutter-rewrite` worktree until parity is reached on a real device.

See:
- Spec: `docs/superpowers/specs/2026-05-28-flutter-rewrite-design.md`
- Foundation plan: `docs/superpowers/plans/2026-05-28-flutter-rewrite-foundation.md`

## Run

```bash
cd software/lamp-app-flutter
flutter pub get
dart run build_runner build --delete-conflicting-outputs
flutter run
```

## Test

```bash
flutter test
```

## Codegen

After touching any `@Riverpod`, `@freezed`, or `@JsonSerializable` source:

```bash
dart run build_runner build --delete-conflicting-outputs
```

(Or `watch` instead of `build` for continuous regeneration.)
