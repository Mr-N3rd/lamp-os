/// The set of critter SVGs that carry the `Shade`/`Body` gradient defs
/// LampPreview needs for runtime recoloring. The wizard picks a random
/// `critterIndex` in 1..8 at adopt/add time and stores it on
/// `InventoryLamp`; this module maps that index to one of the available
/// assets.
///
/// critter-7 was originally a single-gradient sketch in lamplit-web (only
/// `critter7Gradient`); we hand-classified each path into shade vs body
/// based on its y-coordinate range and republished it with two gradients
/// (`critter7Shade` + `critter7Body`) so the runtime recoloring works.
///
/// TODO: same hand-classification pass could lift critter-asset-2/4/6/8
/// from lamplit-web's `/public` directory into a Shade/Body split if we
/// want eight distinct critters.
library;

const _critters = [
  'assets/critters/critter-1.svg',
  'assets/critters/critter-3.svg',
  'assets/critters/critter-5.svg',
  'assets/critters/critter-7.svg',
];

/// Look up the SVG asset path for a given critter index. Falls back to a
/// stable hash of [deviceId] when [critterIndex] is null (legacy lamps).
String critterAssetFor({
  required int? critterIndex,
  required String deviceId,
}) {
  if (critterIndex != null) {
    // critterIndex is 1..8; cycle through the 4 available critters.
    final i = (critterIndex - 1) % _critters.length;
    return _critters[i.abs()];
  }
  final hash = deviceId.codeUnits.fold<int>(0, (a, b) => a + b);
  return _critters[hash.abs() % _critters.length];
}
