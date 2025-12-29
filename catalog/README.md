# Catalog Service

Lightweight CherryPy REST API that exposes the system catalog for devices, services, rooms, and users. Data is stored in `catalog.json` (mounted via Docker volume).

## Run (Docker)
- Defined in `docker-compose.yml` as service `catalog`, port `9080`.
- Recommended volume: `./catalog/catalog.json:/data/catalog.json:rw`.
- Relevant env vars:
  - `CATALOG_PATH` (default: `catalog.json`)
  - `CATALOG_WRITE_TOKEN` (optional; requires `X-Write-Token` for write operations)
  - `CATALOG_READ_ONLY` (`true`/`false`; if true, only GET is allowed)
  - `CATALOG_CACHE_TTL` (seconds for internal cache; default 2.0)

## Minimum catalog structure
Required root fields in the JSON:
`catalog_url`, `projectOwners`, `project_name`, `broker`, `servicesList`, `devicesList`, `roomsList`, `usersList` (lists must be arrays).

## Endpoints (base: http://catalog:9080)
- `GET /health` → simple health check.
- `GET /` → hint `{"see": "/catalog"}`.
- `/catalog`
  - `GET /catalog` → returns the full JSON.
  - `PUT|POST /catalog` → replaces the entire catalog. Requires token if configured; blocked in read-only.

CRUD collections (same pattern):
- `/services`, `/devices`, `/rooms`, `/users`
  - `GET /<col>` → full list.
  - `GET /<col>/{id}` → item by ID (`serviceID`, `deviceID`, `roomID`, `userID`).
  - `POST /<col>` → create (ID required in body). 409 if it already exists.
  - `PUT /<col>/{id}` → replace. Injects the URL ID if missing in body.
  - `PATCH /<col>/{id}` → partial update (merge).
  - `DELETE /<col>/{id}` → delete.
  - All writes blocked if `CATALOG_READ_ONLY=true` or token mismatch when `CATALOG_WRITE_TOKEN` is set.

## Headers and content-type
- JSON requests/responses.
- Open CORS: `Access-Control-Allow-Origin: *`, methods `GET, POST, PUT, PATCH, DELETE, OPTIONS`; `OPTIONS` returns 204.

## Common errors
- 400 invalid payload or missing required fields.
- 401 wrong token (when `CATALOG_WRITE_TOKEN` is set).
- 403 read-only mode.
- 404 resource not found.
- 405 method not allowed.
- 409 duplicate on create.

## Local dev (without Docker)
```bash
pip install cherrypy
python catalog.py   # uses CATALOG_PATH env or catalog.json in cwd
```


