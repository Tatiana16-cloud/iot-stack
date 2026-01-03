#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <DHTesp.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ======== WIFI & MQTT ========
#define WIFI_SSID   "Wokwi-GUEST"
#define WIFI_PASS   ""
#define MQTT_SERVER "test.mosquitto.org"
#define MQTT_PORT   1883
#define USE_WIFI            1   // Enable WiFi (set 0 only if simulating fully offline)

// ======== Catalog config ========
#define DEVICE_ID            "ESP2"                  // Unique device ID, must match Catalog
#define CATALOG_BASE_URL     "https://unfertile-dually-jeana.ngrok-free.dev"   // Base URL (no /catalog needed)
#define CATALOG_WRITE_TOKEN  ""                      // If Catalog enforces X-Write-Token, set it here
#define USE_CATALOG_LOOKUP   1                       // Enable Catalog lookup/patch; set 0 only if offline
#define USE_MQTT             1                       // Enable MQTT; set 0 only if offline

// ======== HW ========
#define DHT_PIN    15
#define SERVO_PIN  18

WiFiClient   espClient;
PubSubClient client(espClient);
DHTesp       dht;
Servo        fan;

// Dynamic identity resolved from Catalog
String user_id   = "";
String room_id   = "";

// Topics (filled after Catalog lookup)
String topic_up;
String topic_servo;
String topic_alert_dht_exact;
String topic_alert_dht_wc;
String topic_down;
String topic_sampling;

bool servo_on = false;
bool sampling_enabled = false;   // <--- por defecto desactivado hasta "bedtime"
unsigned long lastPubMs = 0;
const unsigned long PUB_PERIOD_MS = 2000;

// ---------- Helpers: HTTP + Catalog ----------
bool httpGet(const String& url, String& out) {
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  Serial.printf("[httpGet] url=%s code=%d\n", url.c_str(), code);
  if (code > 0) {
    out = http.getString();
    Serial.printf("[httpGet] resp_len=%d\n", out.length());
  } else {
    Serial.printf("[httpGet] error=%s\n", http.errorToString(code).c_str());
  }
  http.end();
  return (code >= 200 && code < 300);
}

bool httpPatch(const String& url, const String& payload, String& out) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  if (strlen(CATALOG_WRITE_TOKEN) > 0) {
    http.addHeader("X-Write-Token", CATALOG_WRITE_TOKEN);
  }
  int code = http.PATCH((uint8_t*)payload.c_str(), payload.length());
  Serial.printf("[httpPatch] url=%s code=%d payload_len=%d\n", url.c_str(), code, payload.length());
  if (code > 0) {
    out = http.getString();
    Serial.printf("[httpPatch] resp_len=%d\n", out.length());
  } else {
    Serial.printf("[httpPatch] error=%s\n", http.errorToString(code).c_str());
  }
  http.end();
  return (code >= 200 && code < 300);
}

bool resolveRoomAndUser() {
  String body;
  String url = String(CATALOG_BASE_URL) + "/rooms";
  Serial.printf("[catalog] GET %s\n", url.c_str());
  if (!httpGet(url, body)) {
    Serial.println("Catalog GET /rooms failed");
    return false;
  }

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("JSON parse rooms error: ");
    Serial.println(err.c_str());
    return false;
  }
  if (!doc.is<JsonArray>()) return false;

  for (JsonObject room : doc.as<JsonArray>()) {
    JsonArray devs = room["connected_devices"].as<JsonArray>();
    for (JsonObject d : devs) {
      const char* did = d["deviceID"] | "";
      if (strcmp(did, DEVICE_ID) == 0) {
        room_id = String(room["roomID"] | "");
        user_id = String(room["userID"] | "");
        Serial.printf("Catalog room resolved: room=%s user=%s\n", room_id.c_str(), user_id.c_str());
        return (!room_id.isEmpty() && !user_id.isEmpty());
      }
    }
  }

  Serial.println("Device not found in any room");
  return false;
}

void buildTopics() {
  topic_up              = "SC/" + user_id + "/" + room_id + "/dht";
  topic_servo           = "SC/" + user_id + "/" + room_id + "/ServoDHT";
  topic_alert_dht_exact = "SC/alerts/" + user_id + "/" + room_id + "/dht";
  topic_alert_dht_wc    = "SC/alerts/+/+/dht";
  topic_down            = "SC/" + user_id + "/" + room_id + "/down";
  topic_sampling        = "SC/" + user_id + "/" + room_id + "/sampling";
}

bool patchDeviceMetadata() {
  if (room_id.isEmpty() || user_id.isEmpty()) return false;

  StaticJsonDocument<1536> doc;
  doc["availableServices"] = JsonArray();
  doc["availableServices"].add("MQTT");

  JsonObject svc = doc["servicesDetails"].createNestedObject();
  svc["serviceType"] = "MQTT";

  JsonArray topics = svc.createNestedArray("topic");
  topics.add(topic_up);
  topics.add(topic_servo);
  topics.add(topic_down);

  JsonArray topics_pub = svc.createNestedArray("topic_pub");
  topics_pub.add(topic_up);
  topics_pub.add(topic_servo);
  topics_pub.add(topic_down);

  JsonArray topics_sub = svc.createNestedArray("topic_sub");
  topics_sub.add(topic_alert_dht_exact);
  topics_sub.add(topic_alert_dht_wc);
  topics_sub.add(topic_sampling);

  doc["timestamp"] = "device-local";

  String payload;
  serializeJson(doc, payload);

  String resp;
  String url = String(CATALOG_BASE_URL) + "/devices/" + DEVICE_ID;
  bool ok = httpPatch(url, payload, resp);
  Serial.printf("PATCH %s -> %s\n", url.c_str(), ok ? "ok" : "fail");
  if (!resp.isEmpty()) Serial.println(resp);
  return ok;
}

// ---------- WIFI/MQTT ----------
void ensureWifi() {
  if (!USE_WIFI) return;
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); }
}

void ensureMqtt() {
  if (!USE_MQTT) return;
  if (client.connected()) return;
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setBufferSize(2048);
  while (!client.connected()) {
    client.connect(("sc-wokwi-" + String(DEVICE_ID)).c_str());
    if (!client.connected()) delay(500);
  }
  client.subscribe(topic_alert_dht_exact.c_str(), 1);
  client.subscribe(topic_alert_dht_wc.c_str(), 1);
  client.subscribe(topic_sampling.c_str(), 1);
  Serial.printf("MQTT SUB -> %s\n", topic_alert_dht_exact.c_str());
  Serial.printf("MQTT SUB -> %s\n", topic_alert_dht_wc.c_str());
  Serial.printf("MQTT SUB -> %s\n", topic_sampling.c_str());
}

// ---------- Publicaciones ----------
void publishServoSenML() {
  if (!USE_MQTT) return;
  String payload =  "[{\"bn\":\"ServoState\",\"bt\":0,"
                    "\"e\":[{\"n\":\"servoFan\",\"u\":\"bool\",\"vb\":"
                    + String(servo_on ? "true" : "false") + "}]}]";
  client.publish(topic_servo.c_str(), payload.c_str());
  Serial.println("PUB Servo state: " + payload);
}

void publishDown(const char* status_text) {
  if (!USE_MQTT) return;
  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"device\":\"ESP2\",\"type\":\"dht\",\"status\":\"%s\",\"servoFan\":%s,\"sampling\":%s}",
           status_text, servo_on ? "true":"false", sampling_enabled ? "true":"false");
  client.publish(topic_down.c_str(), buf);
  Serial.printf("PUB DOWN -> %s\n", buf);
}

// ---------- Utilidades ----------
bool isAlertDhtTopic(const char* topic) {
  if (!topic) return false;
  if (strncmp(topic, "SC/alerts/", 10) != 0) return false;
  size_t n = strlen(topic);
  return (n >= 4 && strcmp(topic + (n - 4), "/dht") == 0);
}

bool anyEventIsAlertFromPayload(const char* json) {
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, json);
  bool saw_alert = false;

  if (!err) {
    // Estructura esperada: { "events":[ { "status":"ALERT" | "OK", ... }, ... ] }
    if (doc.containsKey("events") && doc["events"].is<JsonArray>()) {
      JsonArray evs = doc["events"].as<JsonArray>();
      for (JsonObject e : evs) {
        const char* s = e["status"] | nullptr;
        if (s && strcmp(s, "ALERT") == 0) {  // mayúsculas exactas
          saw_alert = true;
          break;
        }
      }
      if (saw_alert) return true;
      // <-- Fallback por texto AUNQUE el parse haya sido OK
      if (strstr(json, "\"status\":\"ALERT\"") || strstr(json, "\"status\": \"ALERT\"")) {
        return true;
      }
      return false;
    }

    // Genérico: { "status":"ALERT" } en la raíz
    const char* sroot = doc["status"] | nullptr;
    if (sroot && strcmp(sroot, "ALERT") == 0) return true;

    // Fallback texto aunque haya parseado
    return (strstr(json, "\"status\":\"ALERT\"") || strstr(json, "\"status\": \"ALERT\""));
  }

  // Si falló el parseo → Fallback texto
  Serial.print("JSON parse error: ");
  Serial.println(err.c_str());
  return (strstr(json, "\"status\":\"ALERT\"") || strstr(json, "\"status\": \"ALERT\""));
}


void setFanFromAlert(const char* payload_cstr) {
  if (!sampling_enabled) {
    // ignorar alertas cuando no estamos monitoreando
    return;
  }
  bool want_on = anyEventIsAlertFromPayload(payload_cstr);
  Serial.printf("ALERT RX -> want_on=%d (servo_on=%d)\n", want_on, servo_on);

  if (want_on != servo_on) {
    servo_on = want_on;
    fan.attach(SERVO_PIN, 500, 2400);
    fan.write(servo_on ? 180 : 0);
    publishServoSenML();
    publishDown(servo_on ? "ALERT" : "OK");
    Serial.printf("Actuator -> fan %s\n", servo_on ? "ON" : "OFF");
  }
}

void handleSamplingPayload(const char* json) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;
  bool en = doc["enable"] | false;

  if (en != sampling_enabled) {
    sampling_enabled = en;
    if (!sampling_enabled) {
      // apagamos ventilador y publicamos estado
      if (servo_on) {
        servo_on = false;
        fan.attach(SERVO_PIN, 500, 2400);
        fan.write(0);
        publishServoSenML();
      }
    }
    publishDown(sampling_enabled ? "MONITORING_ON" : "MONITORING_OFF");
    Serial.printf("Sampling -> %s\n", sampling_enabled ? "ENABLED" : "DISABLED");
  }
}

// ---------- Callback MQTT ----------
void onMessage(char* topic, byte* payload, unsigned int len) {
  if (!USE_MQTT) return;
  static char buf[2048];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  Serial.printf("MQTT RX [%s] (%u bytes)\n", topic, len);

  if (isAlertDhtTopic(topic)) {
    setFanFromAlert(buf);
  } else if (strcmp(topic, topic_sampling.c_str()) == 0) {
    handleSamplingPayload(buf);
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  Serial.printf("Booting (WIFI=%d, CATALOG=%d, MQTT=%d)\n", USE_WIFI, USE_CATALOG_LOOKUP, USE_MQTT);
  dht.setup(DHT_PIN, DHTesp::DHT22);

  fan.attach(SERVO_PIN, 500, 2400);
  fan.write(0);
  servo_on = false;

  client.setCallback(onMessage);

  ensureWifi();
#if USE_CATALOG_LOOKUP
  if (resolveRoomAndUser()) {
    buildTopics();
    patchDeviceMetadata();
  } else {
    Serial.println("Falling back to hardcoded IDs; Catalog lookup failed.");
    user_id = "{User2}";
    room_id = "{Room1}";
    buildTopics();
  }
#else
  // Wokwi/offline mode: skip Catalog HTTP to avoid resets/timeouts
  user_id = "{User2}";
  room_id = "{Room1}";
  buildTopics();
#endif
  if (USE_MQTT) ensureMqtt();

  publishServoSenML();  // estado inicial
}

void loop() {
  ensureWifi();
  if (USE_MQTT) {
    ensureMqtt();
    client.loop();
  }

  if (!sampling_enabled) {
    delay(200);
    return;  // no publicar telemetría cuando está desactivado
  }

  unsigned long now = millis();
  if (now - lastPubMs >= PUB_PERIOD_MS) {
    lastPubMs = now;

    auto th = dht.getTempAndHumidity();
    if (!isnan(th.temperature) && !isnan(th.humidity)) {
      String bn = user_id + "/" + room_id + "/";
      String payload = "[{\"bn\":\"" + bn + "\",\"bt\":0,\"e\":["
                       "{\"n\":\"temp\",\"u\":\"Cel\",\"v\":" + String(th.temperature, 1) + "},"
                       "{\"n\":\"hum\",\"u\":\"%RH\",\"v\":"   + String(th.humidity, 1)    + "}"
                       "]}]";
      if (USE_MQTT) {
        client.publish(topic_up.c_str(), payload.c_str());
      }
      Serial.println("PUB SenML (sim/offline ok): " + payload);
    }
  }
}

