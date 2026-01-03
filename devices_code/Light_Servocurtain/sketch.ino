#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ======== WIFI & MQTT ========
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

const char* MQTT_HOST = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = "wokwi-esp32-senml-light";

// ======== IDENTIFICADORES ========
const char* USER = "{User2}";
const char* ROOM = "{Room1}";

// Topics
char TOPIC_SERVO[64];
char TOPIC_SERVO_CURTAIN[64];
char TOPIC_LIGHT_PUB[64];
char TOPIC_LED[64];
char TOPIC_SAMPLING[64];

void buildTopics() {
  snprintf(TOPIC_SERVO,      sizeof(TOPIC_SERVO),      "SC/%s/%s/servoV",  USER, ROOM);
  snprintf(TOPIC_SERVO_CURTAIN,      sizeof(TOPIC_SERVO_CURTAIN),      "SC/%s/%s/servoCurtain",  USER, ROOM);
  snprintf(TOPIC_LIGHT_PUB,  sizeof(TOPIC_LIGHT_PUB),  "SC/%s/%s/Light",   USER, ROOM);
  snprintf(TOPIC_LED,        sizeof(TOPIC_LED),        "SC/%s/%s/LedL",    USER, ROOM);
  snprintf(TOPIC_SAMPLING,   sizeof(TOPIC_SAMPLING),   "SC/%s/%s/sampling",USER, ROOM);
}

// Pines
const int LED_PIN   = 4;
const int SERVO_PIN = 15;
const int POT_PIN   = 34;

// HW
Servo servo;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Pub intervalo
unsigned long lastPub = 0;
const unsigned long PUB_MS = 1000;

// Estado local
int   servoDeg = 0;            // 0=cortina cerrada, 90=abierta
bool  ledOn    = false;
bool  sampling_enabled = false;

// ---------- WIFI/MQTT ----------
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(2048);
  while (!mqtt.connected()) {
    mqtt.connect(MQTT_CLIENT_ID);
    if (!mqtt.connected()) delay(500);
  }
  mqtt.subscribe(TOPIC_SERVO, 1);
  mqtt.subscribe(TOPIC_LED, 1);
  mqtt.subscribe(TOPIC_SAMPLING, 1);
}

// ---------- PUBLICADORES (solo estado/telemetría) ----------
void publishLedSenML() {
  char payload[160];
  snprintf(payload, sizeof(payload),
    "[{\"bn\":\"stateLed\",\"bt\":0,\"e\":[{\"n\":\"LedL\",\"u\":\"bool\",\"vb\":%s}]}]",
    ledOn ? "true" : "false");
  mqtt.publish(TOPIC_LED, payload, false);   // estado (no comando)
  Serial.printf("PUB %s -> %s\n", TOPIC_LED, payload);
}

void publishServoSenML() {
  bool servoOn = (servoDeg == 90);
  char payload[160];
  snprintf(payload, sizeof(payload),
    "[{\"bn\":\"ServoState\",\"bt\":0,"
    "\"e\":[{\"n\":\"servoCurtain\",\"u\":\"bool\",\"vb\":%s}]}]",
    servoOn ? "true" : "false");
  mqtt.publish(TOPIC_SERVO_CURTAIN, payload, false); // estado (no comando)
  Serial.printf("PUB %s -> %s\n", TOPIC_SERVO_CURTAIN, payload);
}

void publishLightSenML(int raw) {
  char payload[160];
  snprintf(payload, sizeof(payload),
    "[{\"bn\":\"lightValue\",\"bt\":0,\"e\":[{\"n\":\"raw\",\"u\":\"lm\",\"v\":%d}]}]", raw);
  mqtt.publish(TOPIC_LIGHT_PUB, payload, false);
  Serial.printf("PUB %s -> %s\n", TOPIC_LIGHT_PUB, payload);
}

// ---------- APLICADORES DE COMANDO (no re-publican para evitar eco) ----------
void applyLedCommand(const char* cmd) {
  // 1) Intentar SenML
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, cmd);
  bool applied = false;

  if (!err && doc.is<JsonArray>()) {
    JsonArray arr = doc.as<JsonArray>();
    if (!arr.isNull() && arr.size() >= 1) {
      JsonObject rec = arr[0];
      if (rec.containsKey("e") && rec["e"].is<JsonArray>()) {
        JsonArray evs = rec["e"].as<JsonArray>();
        if (!evs.isNull() && evs.size() >= 1) {
          JsonObject e0 = evs[0];
          const char* n = e0["n"] | "";
          if (strcmp(n, "LedL") == 0) {
            if (e0.containsKey("vb"))       { ledOn = (bool)(e0["vb"] | false); applied = true; }
            else if (e0.containsKey("v"))   { ledOn = ((int)(e0["v"] | 0) != 0); applied = true; }
          }
        }
      }
    }
  }

  // 2) Fallback: texto "ON"/"OFF"
  if (!applied) {
    String s = String(cmd); s.trim(); s.toUpperCase();
    if (s == "ON" || s == "1" || s == "TRUE")  { ledOn = true;  applied = true; }
    if (s == "OFF"|| s == "0" || s == "FALSE") { ledOn = false; applied = true; }
  }

  if (applied) {
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
    // NO publish aquí -> evita eco
    Serial.printf("LED set -> %s\n", ledOn ? "ON" : "OFF");
  } else {
    Serial.println("LedL payload not understood");
  }
}

void applyServoCommand(const char* cmd) {
  // Si viene nuestro propio estado (SenML), lo ignoramos
  if (cmd && cmd[0] == '[') return;

  int deg = String(cmd).toInt();
  if (deg != 0 && deg != 90) {
    Serial.println("Solo se permiten 0 (cerrada) o 90 (abierta)");
    return;
  }
  servoDeg = deg;
  servo.write(servoDeg);
  // NO publish aquí -> evita eco
  Serial.printf("Servo set -> %d\n", servoDeg);

  publishServoSenML();
}

void handleSamplingPayload(const char* json){
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, json)) return;
  bool en = doc["enable"] | false;
  sampling_enabled = en;
  Serial.printf("Sampling(ESP3) -> %s\n", sampling_enabled ? "ENABLED" : "DISABLED");
}

// ---------- CALLBACK MQTT ----------
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  static char buf[2048];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  if (strcmp(topic, TOPIC_LED) == 0) {
    // Si es nuestro propio estado (SenML) no ejecutar como comando
    if (buf[0] != '[') applyLedCommand(buf);
  } else if (strcmp(topic, TOPIC_SERVO) == 0) {
    applyServoCommand(buf);
  } else if (strcmp(topic, TOPIC_SAMPLING) == 0) {
    handleSamplingPayload(buf);
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(servoDeg);

  analogReadResolution(12);

  buildTopics();
  mqtt.setCallback(onMqttMessage);

  ensureWifi();
  ensureMqtt();

  // Publica estado inicial (solo una vez)
  publishLedSenML();
  publishServoSenML();
}

void loop() {
  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  if (!sampling_enabled) {
    delay(100);
    return;
  }

  unsigned long now = millis();
  if (now - lastPub >= PUB_MS) {
    lastPub = now;
    int raw = analogRead(POT_PIN);
    publishLightSenML(raw);
  }
}

