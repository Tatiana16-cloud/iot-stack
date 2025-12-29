import os, time, requests, json

class CatalogClient:
    """Lightweight client with cache; supports full catalog GET and focused user endpoints."""
    def __init__(self, url: str | None = None, ttl: float = 5.0, timeout: float = 5.0, write_token: str | None = None):
        self.url = url or os.getenv("CATALOG_URL", "http://catalog:9080/catalog")
        self.ttl = ttl
        self.timeout = timeout
        self.write_token = write_token or os.getenv("CATALOG_WRITE_TOKEN")
        self._cache = None
        self._last = 0.0

    def _base_url(self) -> str:
        base = self.url.rstrip("/")
        if base.endswith("/catalog"):
            base = base[: -len("/catalog")]
        return base

    def _headers(self) -> dict:
        hdrs = {"Content-Type": "application/json"}
        if self.write_token:
            hdrs["X-Write-Token"] = self.write_token
        return hdrs

    def _fetch(self):
        target = self.url
        if not target.rstrip("/").endswith("/catalog"):
            target = f"{self._base_url()}/catalog"
        r = requests.get(target, timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def get(self, force: bool = False) -> dict:
        now = time.time()
        if force or self._cache is None or now - self._last > self.ttl:
            self._cache = self._fetch()
            self._last = now
        return self._cache

    def get_catalog(self, force: bool = False) -> dict:
        """Alias for get(); returns the full catalog document."""
        return self.get(force=force)

    # -------- helpers de negocio --------
    def broker(self) -> tuple[str, int]:
        c = self.get()
        b = c.get("broker", {})
        return b.get("IP", "test.mosquitto.org"), int(b.get("port", 1883))

    def service(self, service_id: str) -> dict | None:
        for s in self.get().get("servicesList", []):
            if s.get("serviceID") == service_id:
                return s
        return None

    def users_map_api_keys(self) -> dict[tuple[str, str], str]:
        """
        Devuelve {(userID, roomID): write_api_key}
        Toma el primer apikey de usersList[].thingspeak_info.apikeys
        """
        out: dict[tuple[str, str], str] = {}
        for u in self.get().get("usersList", []):
            uid = u.get("userID")
            room = u.get("roomID", "Room1")
            ts = (u.get("thingspeak_info") or {})
            keys = ts.get("apikeys") or []
            if uid and room and keys:
                out[(uid, room)] = keys[0]
        return out

    # -------- REST helpers (avoid fetching full catalog) --------
    def get_user(self, user_id: str) -> dict | None:
        """
        GET /users/{user_id}. Returns None on 404.
        """
        url = f"{self._base_url()}/users/{user_id}"
        r = requests.get(url, timeout=self.timeout)
        if r.status_code == 404:
            return None
        r.raise_for_status()
        return r.json()

    def get_users(self) -> list:
        """GET /users."""
        url = f"{self._base_url()}/users"
        r = requests.get(url, timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def get_rooms(self) -> list:
        """GET /rooms."""
        url = f"{self._base_url()}/rooms"
        r = requests.get(url, timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def get_room(self, room_id: str) -> dict | None:
        """GET /rooms/{room_id}. Returns None on 404."""
        url = f"{self._base_url()}/rooms/{room_id}"
        r = requests.get(url, timeout=self.timeout)
        if r.status_code == 404:
            return None
        r.raise_for_status()
        return r.json()

    def user_thresholds(self, user_id: str) -> dict:
        """
        Returns threshold_parameters for the user, or {} if missing/not found.
        """
        u = self.get_user(user_id)
        if not u:
            return {}
        return u.get("threshold_parameters") or {}

    def patch_user(self, user_id: str, patch: dict) -> dict:
        """
        PATCH /users/{user_id} with given payload.
        """
        url = f"{self._base_url()}/users/{user_id}"
        r = requests.patch(url, data=json.dumps(patch), headers=self._headers(), timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def patch_service(self, service_id: str, patch: dict) -> dict:
        """
        PATCH /services/{service_id} with given payload.
        """
        url = f"{self._base_url()}/services/{service_id}"
        r = requests.patch(url, data=json.dumps(patch), headers=self._headers(), timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def heartbeat_service(self, service_id: str, ts: str | None = None) -> None:
        """
        Best-effort heartbeat: updates service timestamp in catalog.
        If ts is None, uses current UTC in '%Y-%m-%d %H:%M:%S'.
        """
        if ts is None:
            ts = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        try:
            self.patch_service(service_id, {"timestamp": ts})
        except Exception:
            # swallow errors to avoid crashing caller
            pass

    def update_service_topics(self, service_id: str, mqtt_sub: list | None = None,
                              mqtt_pub: list | None = None, ts: str | None = None) -> None:
        """
        Best-effort update of service MQTT_sub / MQTT_pub (and optional timestamp).
        """
        payload = {}
        if mqtt_sub is not None:
            payload["MQTT_sub"] = mqtt_sub
        if mqtt_pub is not None:
            payload["MQTT_pub"] = mqtt_pub
        if ts is None:
            ts = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        payload["timestamp"] = ts

        try:
            self.patch_service(service_id, payload)
        except Exception:
            # swallow errors to avoid crashing caller
            pass

    def upsert_service(self, service: dict, set_timestamp: bool = True) -> None:
        """
        Best-effort PATCH for a service entry (serviceList item).
        Expects 'serviceID' in the dict. Adds timestamp if requested.
        """
        sid = service.get("serviceID")
        if not sid:
            return
        payload = dict(service)
        # Ensure standard structure
        payload.setdefault("REST_endpoint", "")
        payload.setdefault("MQTT_sub", [])
        payload.setdefault("MQTT_pub", [])
        if set_timestamp:
            payload["timestamp"] = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        try:
            self.patch_service(sid, payload)
        except Exception:
            pass

    def find_user_by_phone(self, phone: str) -> dict | None:
        """
        Searches usersList for a matching phone (exact, stripped).
        Uses cached catalog unless ttl expired.
        """
        doc = self.get()
        for u in doc.get("usersList", []):
            info = u.get("user_information", {}) or {}
            if str(info.get("phone", "")).strip() == str(phone).strip():
                return u
        return None
