

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ======== WIFI & MQTT ========
#define WIFI_SSID   "Wokwi-GUEST"
#define WIFI_PASS   ""
#define MQTT_SERVER "test.mosquitto.org"
#define MQTT_PORT   1883

// ======== Identidad / Topics ========
#define USER_ID   "{User2}"
#define ROOM_ID   "{Room1}"

#define TOPIC_UP        "SC/" USER_ID "/" ROOM_ID "/dht"        // Telemetría SenML
#define TOPIC_SERVO     "SC/" USER_ID "/" ROOM_ID "/ServoDHT"   // Estado servo SenML
#define TOPIC_ALERT_DHT "SC/alerts/" USER_ID "/" ROOM_ID "/dht" // ALERT IN exacto
#define TOPIC_ALERT_WC  "SC/alerts/+/+/dht"                     // ALERT IN wildcard
#define TOPIC_DOWN      "SC/" USER_ID "/" ROOM_ID "/down"       // Reporte actuador
#define TOPIC_SAMPLING  "SC/" USER_ID "/" ROOM_ID "/sampling"   // Control sampling {"enable":true|false}

// ======== HW ========
#define DHT_PIN    15
#define SERVO_PIN  18

WiFiClient   espClient;
PubSubClient client(espClient);
DHTesp       dht;
Servo        fan;

bool servo_on = false;
bool sampling_enabled = false;   // <--- por defecto desactivado hasta "bedtime"
unsigned long lastPubMs = 0;
const unsigned long PUB_PERIOD_MS = 2000;

// ---------- WIFI/MQTT ----------
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); }
}

void ensureMqtt() {
  if (client.connected()) return;
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setBufferSize(2048);
  while (!client.connected()) {
    client.connect("sc-wokwi-" USER_ID "-" ROOM_ID);
    if (!client.connected()) delay(500);
  }
  client.subscribe(TOPIC_ALERT_DHT, 1);
  client.subscribe(TOPIC_ALERT_WC, 1);
  client.subscribe(TOPIC_SAMPLING, 1);
  Serial.printf("MQTT SUB -> %s\n", TOPIC_ALERT_DHT);
  Serial.printf("MQTT SUB -> %s\n", TOPIC_ALERT_WC);
  Serial.printf("MQTT SUB -> %s\n", TOPIC_SAMPLING);
}

// ---------- Publicaciones ----------
void publishServoSenML() {
  String payload =  "[{\"bn\":\"ServoState\",\"bt\":0,"
                    "\"e\":[{\"n\":\"servoFan\",\"u\":\"bool\",\"vb\":"
                    + String(servo_on ? "true" : "false") + "}]}]";
  client.publish(TOPIC_SERVO, payload.c_str());
  Serial.println("PUB Servo state: " + payload);
}

void publishDown(const char* status_text) {
  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"device\":\"ESP2\",\"type\":\"dht\",\"status\":\"%s\",\"servoFan\":%s,\"sampling\":%s}",
           status_text, servo_on ? "true":"false", sampling_enabled ? "true":"false");
  client.publish(TOPIC_DOWN, buf);
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
  static char buf[2048];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  Serial.printf("MQTT RX [%s] (%u bytes)\n", topic, len);

  if (isAlertDhtTopic(topic)) {
    setFanFromAlert(buf);
  } else if (strcmp(topic, TOPIC_SAMPLING) == 0) {
    handleSamplingPayload(buf);
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  dht.setup(DHT_PIN, DHTesp::DHT22);

  fan.attach(SERVO_PIN, 500, 2400);
  fan.write(0);
  servo_on = false;

  client.setCallback(onMessage);

  ensureWifi();
  ensureMqtt();

  publishServoSenML();  // estado inicial
}

void loop() {
  ensureWifi();
  ensureMqtt();
  client.loop();

  if (!sampling_enabled) {
    delay(200);
    return;  // no publicar telemetría cuando está desactivado
  }

  unsigned long now = millis();
  if (now - lastPubMs >= PUB_PERIOD_MS) {
    lastPubMs = now;

    auto th = dht.getTempAndHumidity();
    if (!isnan(th.temperature) && !isnan(th.humidity)) {
      String payload = "[{\"bn\":\"" USER_ID "/" ROOM_ID "/\",\"bt\":0,\"e\":["
                       "{\"n\":\"temp\",\"u\":\"Cel\",\"v\":" + String(th.temperature, 1) + "},"
                       "{\"n\":\"hum\",\"u\":\"%RH\",\"v\":"   + String(th.humidity, 1)    + "}"
                       "]}]";
      client.publish(TOPIC_UP, payload.c_str());
      Serial.println("PUB SenML: " + payload);
    }
  }
}
