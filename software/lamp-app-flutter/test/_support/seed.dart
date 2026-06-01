import 'dart:convert';
import 'dart:typed_data';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/uuids.dart';

/// Writes every per-section characteristic the firmware exposes so
/// `controlNotifierProvider`'s build() can read them without throwing. Used
/// from every widget/notifier test that drives the control surface.
///
/// Defaults match an out-of-the-box lamp; named overrides let individual
/// tests vary just the fields they care about. The control-password and
/// device id come from the caller because they're test-fixture identifiers,
/// not lamp state.
Future<void> seedControlBle(
  InMemoryBleClient ble, {
  required String deviceId,
  String name = 'jacko',
  int brightness = 50,
  bool advancedEnabled = false,
  int basePx = 35,
  int baseAc = 0,
  int baseBpp = 4,
  String baseColorsJson = '["#300783FF"]',
  String baseKnockoutJson = '[]',
  int shadePx = 38,
  int shadeBpp = 4,
  String shadeColorsJson = '["#000000FF"]',
  String homeSsid = '',
  int homeBrightness = 60,
  String expressionsJson = '[]',
}) async {
  await ble.connect(deviceId);
  await ble.write(
      deviceId,
      BleUuids.controlService,
      BleUuids.lampSection,
      Uint8List.fromList(utf8.encode(
        '{"name":"$name","brightness":$brightness,'
        '"advancedEnabled":$advancedEnabled}',
      )));
  await ble.write(
      deviceId,
      BleUuids.controlService,
      BleUuids.baseSection,
      Uint8List.fromList(utf8.encode(
        '{"px":$basePx,"ac":$baseAc,"bpp":$baseBpp,'
        '"colors":$baseColorsJson,"knockout":$baseKnockoutJson}',
      )));
  await ble.write(
      deviceId,
      BleUuids.controlService,
      BleUuids.shadeSection,
      Uint8List.fromList(utf8.encode(
        '{"px":$shadePx,"bpp":$shadeBpp,"colors":$shadeColorsJson}',
      )));
  await ble.write(
      deviceId,
      BleUuids.controlService,
      BleUuids.homeSection,
      Uint8List.fromList(utf8.encode(
        '{"ssid":"$homeSsid","brightness":$homeBrightness}',
      )));
  await ble.write(deviceId, BleUuids.controlService, BleUuids.exprSection,
      Uint8List.fromList(utf8.encode(expressionsJson)));
  await ble.disconnect(deviceId);
}
