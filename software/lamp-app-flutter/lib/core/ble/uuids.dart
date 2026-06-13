abstract class BleUuids {
  // The setup service (5f64f4c1…) was deleted as part of folding initial
  // provisioning into the control service. The control service's `isAuthed`
  // returns true while `lamp.password` is empty, so a factory-default lamp
  // accepts the first claim write unauthenticated. Once a password is set,
  // every write requires GCM auth.
  static const controlService = '5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const auth           = '5f64f4d1-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const brightness     = '5f64f4d2-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const shadeColors    = '5f64f4d3-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const baseColors     = '5f64f4d4-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const baseKnockout   = '5f64f4d5-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const expressionTest = '5f64f4d6-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const settingsBlob   = '5f64f4d7-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const stateNotify    = '5f64f4d8-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const expressionOp   = '5f64f4d9-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const wifiOp         = '5f64f4da-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const wifiState      = '5f64f4db-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const lampSection    = '5f64f4dc-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const baseSection    = '5f64f4dd-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const shadeSection   = '5f64f4de-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const exprSection    = '5f64f4df-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const homeSection    = '5f64f4e0-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const nearbyLamps    = '5f64f4e3-d6d9-4a44-9b3f-3a8d6f7e6b40';
  static const remoteOp       = '5f64f4e4-d6d9-4a44-9b3f-3a8d6f7e6b40';
  // home_mode_focus (write-without-response): single u8 bool. App writes
  // 1 while on the Home Mode setup page, 0 when leaving. Firmware uses
  // this to (a) force effectiveHomeMode TRUE during BT preview, and
  // (b) route CHAR_BRIGHTNESS writes to home.brightness vs lamp.brightness.
  static const homeModeFocus  = '5f64f4e5-d6d9-4a44-9b3f-3a8d6f7e6b40';
  // social_dispositions (read + write, auth-gated): JSON map of peer name
  // to disposition 1..5 (salty, wary, neutral, fond, smitten). Read
  // returns the full map; write replaces it. Persisted firmware-side in
  // a separate NVS key so it doesn't bloat CHAR_LAMP_SECTION.
  static const socialDispositions = '5f64f4e6-d6d9-4a44-9b3f-3a8d6f7e6b40';

  // wisp_op (write-with-response, plaintext): JSON op forwarded to the
  // wisp via MSG_CONTROL_OP broadcast. Shape:
  //   {"char":"wispOp","op":"setZone","zoneId":N}
  //   {"char":"wispOp","op":"clearZone"}
  //   {"char":"wispOp","op":"setWifi","ssid":"…","pw":"…"}  (stub on the wisp)
  // The lamp does not interpret these — it broadcasts them on the mesh
  // for the wisp(s) to consume.
  static const wispOp = '5f64f4e1-d6d9-4a44-9b3f-3a8d6f7e6b40';

  // wisp_status (read + notify): merged JSON of the last wispStatus
  // MSG_CONTROL_OP broadcast (from the wisp) and the last MSG_WISP_HELLO
  // data. See features/wisp/data/wisp_repository.dart for the parsed shape.
  static const wispStatus = '5f64f4e2-d6d9-4a44-9b3f-3a8d6f7e6b40';
}
