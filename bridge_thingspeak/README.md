# Bridge ThingSpeak

Service that consumes MQTT events from `SC/<User>/<Room>/...`, processes sensor data and alerts, and publishes to ThingSpeak using per-user/room credentials stored in the Catalog.

## Purpose
- Send sensor readings to ThingSpeak (averaged per time window).
- Send actuator values and alert counts in the same periodic publish.
- Respect ThingSpeak rate-limit (1 update every ≥15s per channel) by consolidating a single payload per `minPeriodSec`.

## Configuration (settings.json)
- `catalogURL`: Catalog URL (used to fetch API keys/channel per user/room).
- `ThingspeakWriteURL`: ThingSpeak write endpoint.
- `brokerIP` / `brokerPort`: MQTT broker.
- `minPeriodSec`: base sending window; averages and direct values are consolidated in this window (keep ≥15s).
- `wakeupDelaySec`: optional; currently the bridge computes the remaining time to `minPeriodSec` on wakeup, this can serve as a default/fallback.
- `service.serviceID`, `service.MQTT_sub`: MQTT subscriptions (use `{User}/{Room}` placeholders), includes `wakeup` and `initTimeshift`.
- `fields`: logical-name → `fieldN` mapping in ThingSpeak. Example: `"alerts": "field8"`, `"temp": "field1"`. Ensure `alerts` is mapped if you want to chart it.

## Data flow
1) **MQTT subscribe**: dynamic topics `SC/<User>/<Room>/Light|dht|hr|servoV|LedL|wakeup|...`, plus `SC/alerts/<User>/<Room>/...` and `initTimeshift`.
2) **Processing**:
   - SenML (sensors): accumulate sum/count per field (`temp`, `hum`, `bpm`, `light`) for averaging.
   - Actuators/control (`servoCurtain`, `LedL`, `ServoDHT`): keep last value (no averaging).
   - Alerts JSON (`SC/alerts/...`): count only events with `status="ALERT"`. Increment `alerts` per user/room and store in state.
   - `initTimeshift`: reset alert counter and state for that user/room.
3) **Periodic send (every minPeriodSec or wakeup trigger)**:
   - Compute averages for aggregatable fields (or send last known value/null if no new samples).
   - Include last direct values (actuators, `alerts` count).
   - A `wakeup` message schedules the next send after the remaining time to satisfy `minPeriodSec` since the last send; a background ticker (`_check_wakeup_due`) fires without waiting for new MQTT messages.
   - Publish a single payload to ThingSpeak. If response has `entry_id=0`, ThingSpeak rejected (rate-limit), logged as WARNING.

## Alert behavior
- Only count alerts whose JSON payload has `status="ALERT"` in `events`.
- The `alerts` value is sent in the periodic payload with other fields.
- If `alerts` is not mapped in `fields`, a warning is logged and it is skipped.
- `wakeup` does not force-send immediately; it schedules the next send after the remaining time to satisfy `minPeriodSec`.

## Catalog
- Uses `common/catalog_client.py` to get per-user `thingspeak_info` (`apikeys[0]`, `channel`).
- Upserts the service on startup (serviceID, subs/pubs).

## Useful logs
- DEBUG for SenML parsing/skips.
- INFO for publishes (`TS periodic ... entry_id=...`) and alert counts.
- WARNING if ThingSpeak rejects (`entry_id=0`) or if mapping/creds are missing.

## Limitations & best practices
- Keep `minPeriodSec` ≥ 15s to avoid ThingSpeak rate-limit.
- Keep `fields` consistent with channel charts (each fieldN active and mapped).
- Ensure per-user/room credentials exist in Catalog (`thingspeak_info.apikeys`, `channel`).

