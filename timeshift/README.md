# TimeShift Service

Microservice that automates sleep routine transitions (night/day) per user/room. It reads user sleep times from the Catalog, listens to light measurements, and publishes MQTT commands/events for bedtime/wakeup, sampling, curtain (servo), and LED.

## What it does
- Reads per-user sleep times (`user_information.timesleep/timeawake`) from Catalog (on demand).
- Listens to light sensor topics and caches last light value per user/room.
- Decides transition to NIGHT/DAY based on current time vs. user sleep window.
- On transitions:
  - Night (bedtime): publish `bedtime`, enable sampling, close curtain (servo 0), LED OFF.
  - Day (wakeup): publish `wakeup`, decide LED ON/OFF from light reading and pot_min/pot_max, open curtain (servo 90), disable sampling.
- Registers its MQTT pub/sub in the Catalog serviceList via `upsert_service` at startup.
- Listens to initTimeshift events to seed pairs/user times and avoid false transitions on startup.

## Settings (`timeshift/settings.json`)
- `catalogURL`: Catalog base (can include `/catalog`).
- `brokerIP`, `brokerPort`: MQTT broker.
- `timezone`: e.g., `Europe/Rome`.
- `light_raw_threshold`/`light_wait_sec`: legacy light-related params (not used in current logic for threshold).
- `serviceInfo`:
  - `serviceID`: e.g., `TimeShift`
  - `MQTT_sub`:
    - `Light`: topic template, default `SC/{User}/{Room}/Light`
  - `MQTT_pub`:
    - `sampling`: `SC/{User}/{Room}/sampling`
    - `bedtime`:  `SC/{User}/{Room}/bedtime`
    - `wakeup`:   `SC/{User}/{Room}/wakeup`
    - `down`:     `SC/{User}/{Room}/down`
    - `servoV`:   `SC/{User}/{Room}/servoV`
    - `LedL`:     `SC/{User}/{Room}/LedL`
- `MQTT_sub`:
  - `Light`: `SC/{User}/{Room}/Light`
  - `InitTimeshift`: `SC/{User}/{Room}/initTimeshift`

## MQTT flow
- Subscribe (Light): `SC/<User>/<Room>/Light` (SenML expected with `raw`).
- Subscribe (InitTimeshift): `SC/<User>/<Room>/initTimeshift` (seeds pairs and current phase to avoid immediate false transitions).
- Publish on transitions:
  - `bedtime` (event, retain False, payload `{"ts": epoch}`)
  - `wakeup`  (event, retain False, payload `{"seconds": wake_alarm_seconds}`)
  - `sampling` (state, retain True, payload `{"enable": bool}`)
  - `servoV` (state, retain True, payload `"0"` or `"90"`)
  - `LedL` (state, retain True, SenML boolean payload)

## Light decision logic (LED)
- Uses per-user thresholds from Catalog: `threshold_parameters.pot_min` / `pot_max`.
- If missing, fallback: `pot_min=0`, `pot_max=4095`.
- Threshold = (pot_min + pot_max) / 2. If last `raw` < threshold ⇒ LED ON at wakeup; else OFF.
- If no cached light for the pair, defaults LED ON at wakeup.
- Example: if `pot_min=0` and `pot_max=800`, the mid-threshold is `400`; any `raw < 400` will turn LED ON at wakeup.

## Catalog interactions (via `common/catalog_client.py`)
- `get_user(user_id)`: to read `user_information.timesleep/timeawake` and `roomID`.
- `get_room(room_id)`: best-effort validation (not bulk).
- `user_thresholds(user_id)`: to read `threshold_parameters` (for pot_min/pot_max).
- `upsert_service(...)`: registers `serviceID`, `REST_endpoint`, `MQTT_sub`, `MQTT_pub`, `timestamp` at startup.

## Pair discovery (user/room)
- No bulk fetch of all users/rooms. Pairs are learned from incoming Light messages:
  - From topic `SC/<user>/<room>/Light`, stores `(user, room)` (canonicalized `{User}/{Room}`).
  - Best-effort: fetch user to confirm `roomID`; if found, also registers that pair and optionally fetches that room (no full list).
- From topic `SC/<user>/<room>/initTimeshift`, stores `(user, room)`, seeds phase with current times, and best-effort fetches user/room.

## Runtime
- Entry: `timeshift.py`
- Uses: `common/catalog_client.py`
- Dependencies: `paho-mqtt`, `requests`

## Running
- Docker: service `timeshift` in `docker-compose.yml` (uses `timeshift/Dockerfile`).
- Local dev: `pip install -r timeshift/requirements.txt` and `python timeshift.py` (needs broker reachable and catalog URL).


