# LampOS Flutter app

iOS/Android control app for the lamp fleet. Talks BLE GATT to lamps;
does not directly participate in the ESP-NOW mesh.

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
