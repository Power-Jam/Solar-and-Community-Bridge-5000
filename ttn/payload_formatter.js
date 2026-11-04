// TTN uplink payload formatter (Applications > Payload formatters > Uplink > Custom Javascript).
// Decodes the binary uplinks built in src/lora_service.cpp into the fields the
// mqttdashboard "solar" profile expects. All values are big-endian.
//
//   FPort 10  int16 temperature (0.01 C)
//   FPort 20  uint16 voltage (mV), int32 current (uA), uint32 power (mW)
//   FPort 40  int32 latitude, int32 longitude (1e-7 deg), uint8 satellites (255 = unknown)

function readUint(bytes, offset, length) {
  var value = 0;
  for (var i = 0; i < length; i++) {
    value = value * 256 + bytes[offset + i];
  }
  return value;
}

function readInt(bytes, offset, length) {
  var value = readUint(bytes, offset, length);
  var limit = Math.pow(2, 8 * length);
  return value >= limit / 2 ? value - limit : value;
}

function decodeUplink(input) {
  var bytes = input.bytes;
  var data = { schema_version: 1, device_type: "solar" };

  switch (input.fPort) {
    case 10:
      if (bytes.length !== 2) break;
      data.temperature_c = readInt(bytes, 0, 2) / 100;
      return { data: data };

    case 20:
      if (bytes.length !== 10) break;
      data.voltage_v = readUint(bytes, 0, 2) / 1000;
      data.current_a = readInt(bytes, 2, 4) / 1000000;
      data.power_w = readUint(bytes, 6, 4) / 1000;
      return { data: data };

    case 40:
      if (bytes.length !== 9) break;
      data.gps = {
        fix: true,
        latitude: readInt(bytes, 0, 4) / 10000000,
        longitude: readInt(bytes, 4, 4) / 10000000
      };
      if (bytes[8] !== 255) {
        data.gps.satellites = bytes[8];
      }
      return { data: data };

    default:
      return { errors: ["unknown FPort " + input.fPort] };
  }
  return { errors: ["unexpected length " + bytes.length + " on FPort " + input.fPort] };
}
