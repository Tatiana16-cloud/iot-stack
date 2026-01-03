# Sensors (ESP32 + DHT22 + Servo/Fan)

ESP32 firmware that publishes DHT22 telemetry over MQTT, listens for ALERT and sampling control messages, and drives a servo-based fan accordingly.

## Hardware
- MCU: ESP32.
- Sensor: DHT22 on pin 15.
- Actuator: Servo (fan) on pin 18.

## Network & Identity
- Wi-Fi: SSID `Wokwi-GUEST`, no password (as configured in sketch).
- MQTT broker: `test.mosquitto.org:1883`.
- Logical identifiers (hardcoded): `USER_ID = {User2}`, `ROOM_ID = {Room1}`.
- MQTT client ID: `sc-wokwi-{User}/{Room}`.

## Catalog Integration (dynamic identity)
- The device holds a unique `DEVICE_ID` (e.g., `ESP2`) that must exist in the Catalog.
- At startup (real device, `USE_CATALOG_LOOKUP=1`):
  1) Calls Catalog `GET /rooms` and finds the room that lists this `deviceID` in `connected_devices`.
  2) Extracts `roomID` and `userID`, then builds all MQTT topics dynamically.
  3) Sends `PATCH /devices/{DEVICE_ID}` to update:
     - `availableServices: ["MQTT"]`
     - `servicesDetails` with MQTT topics (pub/sub) and timestamp.
  4) If Catalog lookup fails, it falls back to the hardcoded IDs and logs the issue.
- Wokwi/offline mode: set `USE_CATALOG_LOOKUP=0` to skip HTTP calls and use the hardcoded `{User}/{Room}` so the simulator doesn’t loop/reset when the Catalog is unreachable.
- Topics are rebuilt after resolving `userID`/`roomID`; no static user/room is needed in the firmware once Catalog is reachable.

## MQTT Topics
- Publish:
  - Telemetry (SenML): `SC/{User}/{Room}/dht`
    - Fields: `temp` (°C), `hum` (%RH).
  - Servo state (SenML): `SC/{User}/{Room}/ServoDHT`
    - Field: `servoFan` (bool).
  - Device status (JSON): `SC/{User}/{Room}/down`
    - `{ "device": "...", "type": "dht", "status": "...", "servoFan": bool, "sampling": bool }`
- Subscribe:
  - DHT alerts (exact): `SC/alerts/{User}/{Room}/dht`
  - DHT alerts (wildcard): `SC/alerts/+/+/dht`
  - Sampling control: `SC/{User}/{Room}/sampling` (payload `{ "enable": true|false }`)

## Message Formats
- Telemetry (SenML array):
  ```json
  [
    {"bn":"{User}/{Room}/","bt":0,"e":[
      {"n":"temp","u":"Cel","v":T},
      {"n":"hum","u":"%RH","v":H}
    ]}
  ]
  ```
- Servo state (SenML):
  ```json
  [
    {"bn":"ServoState","bt":0,"e":[{"n":"servoFan","u":"bool","vb":true|false}]}
  ]
  ```
- Status/diagnostic (JSON):
  ```json
  {"device":"ESP2","type":"dht","status":"OK|ALERT|MONITORING_ON|MONITORING_OFF","servoFan":bool,"sampling":bool}
  ```
- Sampling control (JSON in):
  ```json
  {"enable":true|false}
  ```
- Alerts (JSON in, expected):
  ```json
  { "events":[ { "status":"ALERT" | "OK", ... } , ... ] }
  ```
  The firmware also falls back to searching for `"status":"ALERT"` in the payload text if parsing fails.

## Runtime Logic
- Connectivity: `ensureWifi()` and `ensureMqtt()` maintain Wi-Fi/MQTT; resubscribes on reconnect. MQTT buffer set to 2048 bytes.
- Sampling flag:
  - `sampling_enabled` starts as `false`; no telemetry is published until a sampling-enable message is received.
  - Receiving `/sampling` toggles `sampling_enabled` and publishes status; disabling also forces servo off and publishes servo state.
- Alerts handling:
  - Subscribed to both exact and wildcard `/dht` alerts; checks if any event has `status:"ALERT"`.
  - If sampling is enabled and an alert is detected, it turns the fan ON (servo 180°), publishes servo state (SenML) and status (`down`); if alert clears, turns fan OFF and publishes updated state.
- Telemetry loop:
  - Every 2 seconds (when `sampling_enabled` is true), reads DHT22 and publishes SenML temp/hum to `.../dht`.
- Initial state:
  - Servo initialized to OFF and publishes initial servo state on startup.

## Build/Dependencies
- Arduino/PlatformIO with libraries: WiFi, PubSubClient, DHTesp, ESP32Servo, ArduinoJson.
- Sketch: `devices_code/Sensors/sketch.ino`.

## Notes
- User/room IDs are hardcoded; adapt if you need dynamic provisioning.
- Alerts parsing is tolerant: uses JSON parse first, falls back to substring search for `"status":"ALERT"`.


