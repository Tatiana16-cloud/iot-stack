# Node-RED UI

Node-RED exposes the user-facing web UI at `http://localhost:1880/ui/`. The data directory is `NodeRed/data` (mounted in Docker at `/data`), which holds flows, credentials, and dashboard configuration.

## Purpose
- Provide a login UI (userID, phone, password) backed by the Catalog.
- Route the user to the dashboard after successful authentication.

## Architecture & data flow
1) **Inputs** (Dashboard widgets)
   - userID, phone, password.
   - Each input writes to flow context: `login_userID`, `login_phone`, `login_password`.
2) **Prepare request** (Function node)
   - Reads the three flow variables, trims them, and sets `msg.login`.
   - Performs `GET http://catalog:9080/catalog` to retrieve `usersList`.
3) **Validation** (Function node)
   - Normalizes userID (removes `{}`) and phone (digits only).
   - Builds `entered_hash = sha256(salt + password)` for each user in `usersList`.
   - Compares normalized userID, phone, and hash (`auth.password_hash` with `auth.password_salt`).
   - On success: sets `flow.current_user_id` and switches to Dashboard.
   - On failure: sends “Data does not match” and clears the three inputs.
4) **Status/UX**
   - Status text is shown then cleared after a delay.
   - Dashboard tab is selected only on successful login.

## Password hashing
- Algorithm: SHA-256 on `salt + password` (implemented inline in the Function node).
- Catalog fields required per user: `auth.password_salt`, `auth.password_hash`.
- Debug logs: `node.warn` prints normalized inputs, `string_to_hash`, `entered_hash`, `stored_hash`, and match flag (useful for diagnosing mismatches).

## Integration with reportGenerator (Dashboard)
- The UI expects reports from the service `ReportsGenerator` (CherryPy) at `http://reports_generator:8093`.
- Endpoint used: `GET /?user_id={UserID}&date=YYYY-MM-DD` (date optional; default = today in Europe/Rome).
- What reportGenerator does (summary):
  - Reads the user from Catalog (`/users/{id}`) to get `timeawake/timesleep` and ThingSpeak channel/keys.
  - Fetches ThingSpeak feeds for the sleep window (fields: temp, hum, bpm, alarm counter).
  - Computes stats (mean/min/max), sleep stages from BPM, and sleep quality (0–100) with an alerts penalty.
  - Returns JSON ready for charts (raw series for bpm/temp/hum and aggregated metrics).
- Node-RED should call this endpoint after a successful login using the authenticated `user_id`; the Dashboard graphs use that response to display trends and metrics.

## Files of interest
- `NodeRed/data/flows.json`: full flow (UI inputs, storage, GET catalog, validation, navigation, dashboard wiring).
- `NodeRed/data/settings.js`: Node-RED settings; data dir mounted at `/data`.

## Running
- Docker Compose: `docker compose up -d --build` then open `http://localhost:1880/ui/`.
- Persistence: `NodeRed/data` is bind-mounted, so flows/credentials survive container restarts.

