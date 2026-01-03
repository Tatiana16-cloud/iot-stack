# Telegram Bot Service

Bot for user-facing interactions: verifies identity via phone against the Catalog, lets users configure sleep times and environment thresholds, shows the ThingSpeak dashboard link, and forwards alerts/bedtime/wakeup events coming from MQTT.

## What it does
- Identity verification: user shares phone number; the bot checks the Catalog (usersList) and binds the chat to that userID. If the phone exists, it then prompts for password.
- Configuration:
  - Sleep times: wake-up (`timeawake`) and sleep (`timesleep`) -> PATCH to Catalog `user_information`.
  - Thresholds: temperature/humidity min/max -> PATCH to Catalog `threshold_parameters`.
- Dashboard link: fetches ThingSpeak channel from Catalog and sends the channel URL.
- Notifications:
  - Alerts (HR/env) from Alarm via MQTT topics `SC/alerts/{user}/{room}/hr|dht`.
  - Sleep events (bedtime/wakeup) from TimeShift via MQTT topics `SC/{user}/{room}/bedtime|wakeup`.
- Emits initTimeshift: after setting wake/sleep times, publishes `SC/{User}/{Room}/initTimeshift` with `{timeawake, timesleep}`.

## Authentication & password hashing
- Login flow:
  1) Ask for phone (international format). If not found, ask to re-enter or register with an admin.
  2) If found, ask for password. The bot deletes the message (best effort) to avoid showing the password in the chat.
  3) Hash check: `entered_hash = sha256(password_salt + password)`; compare to `auth.password_hash` stored in the Catalog.
- Catalog fields required for each user:
  - `auth.password_salt`
  - `auth.password_hash` (sha256 of `salt + password`)
- On success: binds chat to `userID`, enables menus; on failure: prompts again.

## Settings (settings.json)
- `catalogURL`: base URL of Catalog (without trailing `/catalog`).
- `brokerIP`, `brokerPort`: MQTT broker.
- `serviceInfo`:
  - `serviceID`: logical name (e.g., TelegramBot)
  - `telegram_token`: BotFather token
  - `MQTT_sub` (optional): extra MQTT subscriptions if needed.

## User flow & menus
- `/start`:
  - Asks for phone number (international format, e.g., `+573001112233`).
  - If found in Catalog, binds session and shows main menu.

- Main menu (buttons):
  - `1. Configuration`
  - `2. Show dashboard`

- Configuration menu (buttons):
  - `1. Wake/Sleep time`
  - `2. Temp/Humidity min-max`
  - `⬅️ Back`

Actions:
- Set wake/sleep time:
  - Prompts for `HH:MM` wake-up, then `HH:MM` sleep.
  - Writes to Catalog `users/{id}` → `user_information.timeawake`, `user_information.timesleep` (PATCH).
- Set temp/humidity thresholds:
  - Prompts for min/max temperature, then min/max humidity.
  - Writes to Catalog `users/{id}` → `threshold_parameters.temp_low/temp_high/hum_low/hum_high` (PATCH).
- Show dashboard:
  - Reads user from Catalog; if `thingspeak_info.channel` exists, sends `https://thingspeak.com/channels/{channel}`.

## MQTT integration
- Broker: from settings (`brokerIP`, `brokerPort`).
- Subscriptions (normalized):
  - Alerts: `SC/alerts/+/+/#`
  - Sleep events: `SC/+/+/bedtime`, `SC/+/+/wakeup`
  - Plus any extra topics from `serviceInfo.MQTT_sub`.
- Publications:
  - `SC/{User}/{Room}/initTimeshift` when user sets times (wake/sleep).
- Behavior:
  - Alerts: only forwards to verified chats when status is `ALERT`, and every 120s while still in ALERT (no spam on OK).
  - Bedtime/Wakeup: forwards each message once to the user’s chats.
  - Messages are sent via Telegram API `sendMessage` using the configured bot token.

## Catalog interactions (via `common/catalog_client.py`)
- `find_user_by_phone(phone)`: lookup for login (uses cached catalog).
- `get_user(userID)`: load user data via `/users/{id}` (dashboard channel, thresholds, etc.).
- `patch_user(userID, patch)`: update `/users/{id}` (times in `user_information`, thresholds in `threshold_parameters`).

## Runtime
- Entry: `telegram_bot.py`
- Starts Telegram polling + background MQTT listener (AlertsMQTT thread).
- Depends on: `python-telegram-bot`, `requests`, `paho-mqtt`, `common/catalog_client.py`.

## Requirements
- BotFather token with the same token used in settings.json.
- Catalog populated with users including `user_information.phone`; optional `thingspeak_info.channel`.
- MQTT topics published by Alarm (alerts) and TimeShift (bedtime/wakeup).


