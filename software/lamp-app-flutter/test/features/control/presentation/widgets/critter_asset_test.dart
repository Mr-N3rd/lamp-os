import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/presentation/widgets/critter_asset.dart';

void main() {
  test('critterIndex 1..4 maps to four distinct critter SVGs', () {
    final picks = {
      for (var i = 1; i <= 4; i++)
        critterAssetFor(critterIndex: i, deviceId: 'x'),
    };
    expect(picks.length, 4);
  });

  test('critterIndex 5..8 cycles back through the same four (no more SVGs yet)',
      () {
    for (var i = 1; i <= 4; i++) {
      expect(
        critterAssetFor(critterIndex: i, deviceId: 'x'),
        critterAssetFor(critterIndex: i + 4, deviceId: 'x'),
      );
    }
  });

  test('null critterIndex falls back to a deterministic deviceId hash', () {
    expect(
      critterAssetFor(critterIndex: null, deviceId: 'lamp-A'),
      critterAssetFor(critterIndex: null, deviceId: 'lamp-A'),
    );
  });
}
