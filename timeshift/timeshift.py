import os
import time
import json
import logging
import threading
from dataclasses import dataclass
from typing import Dict, Tuple, Any, List, Optional
from datetime import datetime

from paho.mqtt.client import Client as MqttClient, MQTTMessage
from common.catalog_client import CatalogClient

try:
    from zoneinfo import ZoneInfo
except Exception:
    ZoneInfo = None

# --------------- Logging ---------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - timeshift - %(levelname)s - %(message)s",
)
log = logging.getLogger("timeshift")

# --------------- Settings ---------------
@dataclass
class TSSettings:
    catalog_url: str    # e.g. http://catalog:9080
    broker_ip: str
    broker_port: int
    service_id: str
    mqtt_pub: Dict[str, str]
    mqtt_sub: Dict[str, str]

    loop_interval_sec: int = 10
    wake_alarm_seconds: int = 30
    light_threshold_fallback: int = 2048
    timezone: str = "Europe/Rome"

    @classmethod
    def load(cls, path: str = "settings.json") -> "TSSettings":
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        si = data["serviceInfo"]
        base = data["catalogURL"].rstrip("/")
        if base.endswith("/catalog"):
            base = base[: -len("/catalog")]
        return cls(
            catalog_url=base,
            broker_ip=data["brokerIP"],
            broker_port=int(data["brokerPort"]),
            service_id=si.get("serviceID", "TimeShift"),
            mqtt_pub=dict(si.get("MQTT_pub", {})),
            mqtt_sub=dict(si.get("MQTT_sub", {})),
            loop_interval_sec=int(data.get("loop_interval_sec", 10)),
            wake_alarm_seconds=int(data.get("wake_alarm_seconds", 30)),
            light_threshold_fallback=int(data.get("light_threshold_fallback", 2048)),
            timezone=data.get("timezone", "Europe/Rome"),
        )

# --------------- Helpers ---------------
def parse_hhmm(s: str) -> Optional[int]:
    if not s or not isinstance(s, str): return None
    s = s.strip()
    try:
        hh, mm = s.split(":")
        h = int(hh); m = int(mm)
        if 0 <= h <= 23 and 0 <= m <= 59:
            return h*60 + m
    except Exception:
        return None
    return None

def in_sleep_window(now_min: int, sleep_min: int, wake_min: int) -> bool:
    if sleep_min is None or wake_min is None:
        return False
    if sleep_min < wake_min:
        return sleep_min <= now_min < wake_min
    else:
        return now_min >= sleep_min or now_min < wake_min

def senml_led_payload(on: bool) -> str:
    return json.dumps([{
        "bn": "stateLed",
        "bt": 0,
        "e": [{"n":"LedL","u":"bool","vb": bool(on)}]
    }])

def canon_id(s: str) -> str:
    s = str(s or "")
    return s if (s.startswith("{") and s.endswith("}")) else "{"+s+"}"

# --------------- TimeShift core ---------------
class TimeShiftService:
    def __init__(self, settings: TSSettings):
        self.S = settings
        self.cat = CatalogClient(self.S.catalog_url)

        self.last_light: Dict[Tuple[str,str], int] = {}
        self.last_phase: Dict[Tuple[str,str], str] = {}
        self.known_pairs: set[Tuple[str,str]] = set()

        self.light_min = 0
        self.light_max = self.S.light_threshold_fallback * 2  # ~4096
        self._load_thresholds()

        if ZoneInfo is not None:
            try:
                self.tz = ZoneInfo(self.S.timezone)
            except Exception:
                log.warning("Invalid timezone '%s', fallback to UTC", self.S.timezone)
                self.tz = ZoneInfo("UTC")
        else:
            self.tz = None

        self.mqtt = MqttClient(client_id="timeshift", clean_session=True)
        self.mqtt.on_connect = self.on_connect
        self.mqtt.on_message = self.on_message

        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()

        # Best-effort: update service entry in Catalog
        self._upsert_service()

    # ---------- Catalog ----------
    def _load_thresholds(self):
        """
        Set global fallbacks for light thresholds (used if user thresholds are missing).
        No catalog calls here. Defaults: 0..4095.
        """
        self.light_min = 0
        self.light_max = 4095
        log.info("Thresholds (fallback) pot_min=%s pot_max=%s", self.light_min, self.light_max)

    def _target_pairs(self) -> List[Tuple[str,str]]:
        # No bulk fetch; use pairs discovered via incoming Light messages
        return list(self.known_pairs)

    def _user_times(self, user_id: str) -> Tuple[Optional[int], Optional[int]]:
        try:
            u = self.cat.get_user(user_id) or {}
            info = u.get("user_information", {}) or {}
            ts = parse_hhmm(info.get("timesleep"))
            ta = parse_hhmm(info.get("timeawake"))
            return ts, ta
        except Exception:
            log.exception("Error leyendo times para user %s", user_id)
            return None, None

    # ---------- MQTT ----------
    def connect_mqtt(self):
        self.mqtt.connect(self.S.broker_ip, self.S.broker_port, keepalive=30)
        self._thread = threading.Thread(target=self.mqtt.loop_forever, daemon=True)
        self._thread.start()
        log.info("MQTT loop thread started.")

    def on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            log.error("MQTT connect rc=%s", rc); return
        try:
            topics = list(self.S.mqtt_sub.values()) if self.S.mqtt_sub else []
            if not topics:
                topics = ["SC/+/+/Light"]
            for t in topics:
                sub = self._normalize_sub(t)
                client.subscribe(sub, qos=1)
                log.info("SUB %s (from %s)", sub, t)
        except Exception:
            log.exception("subscribe topics failed")

    def on_message(self, client, userdata, msg: MQTTMessage):
        try:
            topic = msg.topic  # SC/<user>/<room>/Light
            parts = topic.split("/")
            if len(parts) == 4 and parts[0] == "SC" and parts[3] == "Light":
                user_raw, room_raw = parts[1], parts[2]
                user, room = canon_id(user_raw), canon_id(room_raw)
                log.info("[light] msg from user=%s room=%s topic=%s", user, room, topic)
                # Register pair from topic
                self.known_pairs.add((user, room))

                # Best-effort: fetch user to get authoritative roomID, then fetch that room
                try:
                    u = self.cat.get_user(user_raw) or {}
                    room_id = u.get("roomID")
                    if room_id:
                        room_canon = canon_id(room_id)
                        self.known_pairs.add((canon_id(user_raw), room_canon))
                        try:
                            _ = self.cat.get_room(str(room_id))
                        except Exception:
                            pass
                except Exception:
                    log.exception("Error fetching user/room for light topic %s", topic)

                raw = self._parse_light_senml(msg.payload.decode("utf-8","ignore"))
                if raw is not None:
                    self.last_light[(user,room)] = raw
                    log.info("[light] cached raw=%s for %s/%s", raw, user, room)
            elif len(parts) == 4 and parts[0] == "SC" and parts[3] == "initTimeshift":
                user_raw, room_raw = parts[1], parts[2]
                user, room = canon_id(user_raw), canon_id(room_raw)
                payload_txt = msg.payload.decode("utf-8","ignore")
                log.info("[initTimeshift] msg user=%s room=%s topic=%s payload=%s", user, room, topic, payload_txt)
                self.known_pairs.add((user, room))
                # Best-effort: fetch user/room to ensure they exist
                try:
                    u = self.cat.get_user(user_raw) or {}
                    room_id = u.get("roomID") or room_raw
                    if room_id:
                        self.cat.get_room(str(room_id))
                        self.known_pairs.add((canon_id(user_raw), canon_id(room_id)))
                    # Seed last_phase with current phase to avoid immediate false transitions
                    phase, ts, ta = self.desired_phase(user_raw)
                    key = (canon_id(user_raw), canon_id(room_id or room_raw))
                    if phase is not None:
                        self.last_phase[key] = phase
                        log.info("[initTimeshift] registered pair user=%s room=%s phase=%s ts=%s ta=%s", key[0], key[1], phase, ts, ta)
                    else:
                        log.info("[initTimeshift] registered pair user=%s room=%s but missing times", key[0], key[1])
                except Exception:
                    log.exception("Error processing initTimeshift for %s/%s", user, room)
        except Exception:
            log.exception("on_message error")

    @staticmethod
    def _parse_light_senml(payload: str) -> Optional[int]:
        try:
            arr = json.loads(payload)
            if isinstance(arr, list) and arr:
                rec = arr[0]
                e = rec.get("e", [])
                if isinstance(e, list):
                    for ent in e:
                        if ent.get("n") == "raw":
                            v = ent.get("v")
                            if isinstance(v, (int, float)):
                                return int(v)
        except Exception:
            return None
        return None

    # ---------- Publish helper ----------
    def _pub(self, topic: str, payload: str | bytes, *, qos: int = 1, retain: bool = False):
        try:
            res = self.mqtt.publish(topic, payload=payload, qos=qos, retain=retain)
            res.wait_for_publish()
            log.info("PUB %s (qos=%d retain=%s) -> %s", topic, qos, retain,
                     payload if isinstance(payload, str) else "<bytes>")
        except Exception:
            log.exception("Publish failed: %s", topic)

    # ---------- Publicadores ----------
    @staticmethod
    def _fmt_topic(template: str, user: str, room: str) -> str:
        return (template
                .replace("{User}", user)
                .replace("{Room}", room))

    @staticmethod
    def _normalize_sub(template: str) -> str:
        """Replace placeholders with wildcards for subscriptions."""
        t = (template or "").replace("{User}", "+").replace("{Room}", "+")
        while "//" in t:
            t = t.replace("//", "/")
        return t

    def pub_sampling(self, user: str, room: str, enable: bool):
        user, room = canon_id(user), canon_id(room)
        tpl = self.S.mqtt_pub.get("sampling", "SC/{User}/{Room}/sampling")
        topic = self._fmt_topic(tpl, user, room)
        payload = json.dumps({"enable": bool(enable)})
        self._pub(topic, payload, qos=1, retain=True)   # ESTADO

    def pub_bedtime(self, user: str, room: str):
        user, room = canon_id(user), canon_id(room)
        tpl = self.S.mqtt_pub.get("bedtime", "SC/{User}/{Room}/bedtime")
        topic = self._fmt_topic(tpl, user, room)
        payload = json.dumps({"ts": int(time.time())})
        self._pub(topic, payload, qos=1, retain=False)  # EVENTO

    def pub_wakeup(self, user: str, room: str):
        user, room = canon_id(user), canon_id(room)
        tpl = self.S.mqtt_pub.get("wakeup", "SC/{User}/{Room}/wakeup")
        topic = self._fmt_topic(tpl, user, room)
        payload = json.dumps({"seconds": int(self.S.wake_alarm_seconds)})
        self._pub(topic, payload, qos=1, retain=False)  # EVENTO

    def pub_led_senml(self, user: str, room: str, on: bool):
        user, room = canon_id(user), canon_id(room)
        tpl = self.S.mqtt_pub.get("LedL", "SC/{User}/{Room}/LedL")
        topic = self._fmt_topic(tpl, user, room)
        payload = senml_led_payload(on)
        self._pub(topic, payload, qos=1, retain=True)   # ESTADO

    def pub_servo(self, user: str, room: str, deg: int):
        user, room = canon_id(user), canon_id(room)
        tpl = self.S.mqtt_pub.get("servoV", "SC/{User}/{Room}/servoV")
        topic = self._fmt_topic(tpl, user, room)
        payload = str(int(deg))  # "0" ó "90"
        self._pub(topic, payload, qos=1, retain=True)   # ESTADO

    # ---------- Lógica principal ----------
    def desired_phase(self, user: str) -> Tuple[Optional[str], Optional[int], Optional[int]]:
        ts, ta = self._user_times(user)
        if ts is None or ta is None:
            return None, ts, ta
        now = datetime.now(self.tz) if self.tz is not None else datetime.now()
        now_min = now.hour*60 + now.minute
        night = in_sleep_window(now_min, ts, ta)
        return ("night" if night else "day"), ts, ta

    def light_needs_led(self, user: str, room: str) -> bool:
        user, room = canon_id(user), canon_id(room)
        # Per-user thresholds from catalog; fallback to global defaults
        thr = self.cat.user_thresholds(user)
        pot_min = thr.get("pot_min", self.light_min)
        pot_max = thr.get("pot_max", self.light_max)
        log.info("[thr] user=%s room=%s pot_min=%s pot_max=%s", user, room, pot_min, pot_max)

        raw = self.last_light.get((user, room))
        if raw is None:
            log.info("No light cached for %s/%s -> LED ON by default", user, room)
            return True
        thr = (pot_min + pot_max) / 2.0
        need = raw < thr
        log.info("[decision] light %s/%s raw=%s thr=%.1f below=%s -> LED %s",
                 user, room, raw, thr, raw < thr, "ON" if need else "OFF")
        return need

    def do_bedtime(self, user: str, room: str):
        self.pub_bedtime(user, room)          # evento
        self.pub_sampling(user, room, True)   # estado
        self.pub_servo(user, room, 0)         # estado
        self.pub_led_senml(user, room, False) # estado

    def do_wakeup(self, user: str, room: str):
        self.pub_wakeup(user, room)           # evento
        led_on = self.light_needs_led(user, room)
        self.pub_led_senml(user, room, led_on) # estado
        self.pub_servo(user, room, 90)         # estado
        self.pub_sampling(user, room, False)   # estado

    def _upsert_service(self):
        mqtt_sub_list = list(self.S.mqtt_sub.values()) if self.S.mqtt_sub else []
        mqtt_pub_list = list(self.S.mqtt_pub.values()) if self.S.mqtt_pub else []
        try:
            self.cat.upsert_service({
                "serviceID": self.S.service_id,
                "REST_endpoint": "",
                "MQTT_sub": mqtt_sub_list,
                "MQTT_pub": mqtt_pub_list,
            })
        except Exception:
            log.exception("Catalog upsert service failed")

    def run(self):
        self.connect_mqtt()
        log.info("TimeShift running every %ss (TZ=%s)", self.S.loop_interval_sec, self.S.timezone)

        while not self._stop.is_set():
            try:
                pairs = self._target_pairs()
                for (user_raw, room_raw) in pairs:
                    user, room = canon_id(user_raw), canon_id(room_raw)
                    phase, ts, ta = self.desired_phase(user_raw)
                    if phase is None:
                        continue

                    key = (user, room)
                    last = self.last_phase.get(key)

                    if last != phase:
                        self.last_phase[key] = phase
                        if phase == "night":
                            log.info("[%s/%s] Transition -> NIGHT", user, room)
                            self.do_bedtime(user, room)
                        else:
                            log.info("[%s/%s] Transition -> DAY", user, room)
                            self.do_wakeup(user, room)

                self._stop.wait(self.S.loop_interval_sec)

            except Exception:
                log.exception("loop error")
                self._stop.wait(self.S.loop_interval_sec)

    def stop(self):
        self._stop.set()
        try:
            self.mqtt.disconnect()
        except Exception:
            pass

# --------------- Bootstrap ---------------
def main():
    S = TSSettings.load("settings.json")
    svc = TimeShiftService(S)
    try:
        svc.run()
    except KeyboardInterrupt:
        svc.stop()

if __name__ == "__main__":
    main()
