/// The set of critter SVGs that carry the `Shade`/`Body` gradient defs
/// LampPreview needs for runtime recoloring. The wizard picks a random
/// `critterIndex` in 1..8 at adopt/add time and stores it on
/// `InventoryLamp`; this module maps that index to one of the available
/// assets.
///
/// TODO: extend the catalog to 8 distinct critters once we have SVGs that
/// properly split into shade + body. Today only critters 1, 3, 5, 7 (from
/// lamplit-web) have the right structure; the asset-N variants are
/// single-fill sketches and would need per-path classification to be
/// usable. While there are only 4 viable SVGs, indices cycle through
/// them so the mapping is deterministic per-lamp and only visually
/// "doubled up" — i.e. lamp index 1 and lamp index 5 share a critter
/// shape.
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
