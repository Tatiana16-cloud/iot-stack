# ReportsGenerator

REST service (CherryPy) that generates a sleep report for a given user/date using data stored in ThingSpeak and metadata from the Catalog.

## What it does
- Loads per-user sleep window (timesleep/timeawake) from Catalog.
- Fetches ThingSpeak feeds for that window, normalizes timestamps to Europe/Rome.
- Computes stats for BPM, temperature, humidity, alarm activations, and derives sleep stages + a sleep quality score.
- Sends a heartbeat/upsert to Catalog at startup and on each request (best effort).

## Configuration
- `reportGenerator/settings.json`
  - `catalogURL`: Catalog base URL.
  - `reportsURL`: Service URL (used for registration/metadata).
  - `thingspeakURL`: Base URL for ThingSpeak reads.
  - `serviceInfo`: `serviceID`, `REST_endpoint`, `MQTT_sub`/`MQTT_pub` (not used in logic but registered).
  - `fields`: ThingSpeak field mapping:
    - `TS_BPM_FIELD` (default `field3`)
    - `TS_TEMP_FIELD` (default `field1`)
    - `TS_HUM_FIELD`  (default `field2`)
    - `TS_ALARM_FIELD` (default `field8`)
- Padding for TS queries: env `TS_PAD_MIN` (default 5 minutes). Data is clipped back to the exact sleep window.

## Dependencies
- `reportGenerator/reporting_service.py` (CherryPy service)
- `reportGenerator/catalog_client.py` (CatalogClient)
- Uses: pandas, numpy, dateutil, requests.

## Request flow (GET /?user_id=User1&date=YYYY-MM-DD)
1) Heartbeat + upsert (best effort) to Catalog via `CatalogClient`.
2) Determine reference date (today in Europe/Rome unless `date` provided).
3) Fetch user from Catalog (`get_user`), extract `timesleep/timeawake`, and build the local sleep window (handles midnight crossing).
4) Get ThingSpeak credentials from user (`thingspeak_info.channel`, `apikeys[]`). No env overrides are used for channel/keys (per-user from Catalog).
5) Fetch ThingSpeak feeds using `fetch_ts_robusto` with padding:
   - Tries timezone=Europe/Rome with each key and with no key (public).
   - If empty, retries with UTC start/end and no timezone parameter.
6) Select fields and clip to the exact window:
   - `pick_fields` keeps `created_at` and mapped fields, converts to numeric.
   - Clip to [start, end). If empty, returns status 200 with a message.
7) Metrics:
   - `basic_stats`: mean/min/max for bpm, temp, hum.
   - `count_led_activations`: uses the max value of the alarm counter in the window (counter accumulated and reset by initTimeshift).
   - Raw arrays for bpm/temp/hum are returned for plotting.
   - `infer_sleep_stages_from_bpm`: heuristic stages (deep/light/rem) via rolling median baseline and time spent.
   - `sleep_quality`: weighted score (0–100) combining temp, hum, bpm span, and a penalty by alert rate.
8) Response: JSON with status, user_id, window, stats, stages_hours, sleep_quality, and optional ThingSpeak fetch debug info.

## Sleep quality details
- Temp ideal ~19°C: linear decay to 0 at ±6°C.
- Hum ideal 40–60 %RH: 100 inside range; linear decay centered at 50 up to ±30.
- BPM variability: span = max–min, capped at 40; score = 100*(1–span/40).
- Alerts penalty: rate_per_hour = alarm_count / duration_hours; penalty = min(rate_per_hour * 2, 40).
- Final score: `score = clip(0.35*temp + 0.25*hum + 0.40*bpm – penalty, 0, 100)` with English labels.

## Running
- Docker: built via `docker-compose.yml` (context root, Dockerfile in `reportGenerator/`).
- Ports: exposed 8093.

