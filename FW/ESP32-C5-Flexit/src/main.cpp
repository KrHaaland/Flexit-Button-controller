#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include "index_html.h"

// ---- Pin mapping (from KiCad schematic) -----------------------------------
// Optocoupler trigger outputs (drive AQY212S input LEDs through series R):
//   SW1 = IO4, SW2 = IO3, SW3 = IO2, SW4 = IO1, SW5 = IO0
// LED status reads (signals routed from J1 / Flexit panel side):
//   Led1 = IO5, Led2 = IO6, Led3 = IO7, Led4 = IO8, Led5 = IO9
static const uint8_t SW_PINS[5]  = { 4, 3, 2, 1, 0 };
static const uint8_t LED_PINS[5] = { 5, 6, 7, 8, 9 };

// LED polarity: set true if the Flexit drives the line HIGH when the LED is on.
// If your LEDs read inverted, flip this to false.
static constexpr bool LED_ACTIVE_HIGH = true;

// Pulse duration when a button is clicked from the web UI.
// Runtime-tunable via /config and persisted in NVS.
static constexpr uint32_t PULSE_MS_DEFAULT = 300;
static constexpr uint32_t PULSE_MS_MIN = 20;
static constexpr uint32_t PULSE_MS_MAX = 2000;
static uint32_t pulseMs = PULSE_MS_DEFAULT;

// ---- Wi-Fi configuration --------------------------------------------------
// AP fallback (always available when STA fails or no creds stored).
static const char* AP_SSID  = "Flexit-Setup";
static const char* AP_PASS  = "fibonacci";  // min 8 chars, WPA2
static const char* HOSTNAME = "flexit";
static constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;

// ---- Globals --------------------------------------------------------------
WebServer server(80);
Preferences prefs;
WiFiClient netClient;
PubSubClient mqtt(netClient);

enum WifiMode : uint8_t { MODE_AP = 0, MODE_STA = 1 };
static WifiMode currentMode = MODE_AP;

// Per-channel pulse end timestamp (millis). 0 = idle.
volatile uint32_t pulseEndAt[5] = { 0, 0, 0, 0, 0 };

// Deferred actions requested from HTTP handlers (executed in loop()).
static volatile bool pendingReboot = false;
static uint32_t rebootAt = 0;

// ---- MQTT state -----------------------------------------------------------
static String mqttHost;
static uint16_t mqttPort = 1883;
static String mqttUser;
static String mqttPass;
static String mqttBase = "flexit";
static bool ledLastPub[5] = { false, false, false, false, false };
static bool ledStateInit = false;
static uint32_t mqttLastTry = 0;
static int mqttLastState = 0;  // PubSubClient state for diagnostics
static constexpr uint32_t MQTT_RETRY_MS = 5000;

// Periodic republish of LED state (in addition to on-change publishes).
// 0 disables the heartbeat — only transitions are published.
static constexpr uint16_t PUB_INTERVAL_MAX_SEC = 3600;
static uint16_t mqttPubIntervalSec = 0;
static uint32_t mqttLastPubAt = 0;

static inline bool readLed(uint8_t idx) {
  int v = digitalRead(LED_PINS[idx]);
  return LED_ACTIVE_HIGH ? (v == HIGH) : (v == LOW);
}

static void startPulse(uint8_t idx) {
  digitalWrite(SW_PINS[idx], HIGH);
  pulseEndAt[idx] = millis() + pulseMs;
}

static void servicePulses() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 5; i++) {
    if (pulseEndAt[i] != 0 && (int32_t)(now - pulseEndAt[i]) >= 0) {
      digitalWrite(SW_PINS[i], LOW);
      pulseEndAt[i] = 0;
    }
  }
}

// ---- JSON helpers ---------------------------------------------------------
static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// ---- MQTT -----------------------------------------------------------------
// Topic layout (with base = "flexit"):
//   flexit/led/<1..5>/state    -- published, retained: "ON" | "OFF"
//   flexit/sw/<1..5>/trigger   -- subscribed, any payload triggers a pulse
//                                 (or payload "<ms>" overrides pulseMs)
//   flexit/status              -- LWT, retained: "online" | "offline"

static String mqttClientId() {
  // Use the full 48-bit base MAC so the ID is unique per device.
  uint64_t mac = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "flexit-%012llx",
           (unsigned long long)(mac & 0xFFFFFFFFFFFFULL));
  return String(buf);
}

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  // Expect: <base>/sw/<n>/trigger
  String t(topic);
  String prefix = mqttBase + "/sw/";
  if (!t.startsWith(prefix)) return;
  int slash = t.indexOf('/', prefix.length());
  if (slash < 0) return;
  if (t.substring(slash) != "/trigger") return;
  int n = t.substring(prefix.length(), slash).toInt();
  if (n < 1 || n > 5) return;

  // Optional payload: "<ms>" to override the pulse duration for this trigger.
  String p;
  p.reserve(len);
  for (unsigned int i = 0; i < len; i++) p += (char)payload[i];
  p.trim();

  uint32_t saved = pulseMs;
  if (!p.isEmpty()) {
    long v = p.toInt();
    if (v >= (long)PULSE_MS_MIN && v <= (long)PULSE_MS_MAX) {
      pulseMs = (uint32_t)v;
    }
  }
  Serial.printf("[mqtt] trigger sw=%d (pulse %u ms)\n", n, (unsigned)pulseMs);
  startPulse((uint8_t)(n - 1));
  pulseMs = saved;
}

static void mqttPublishLed(uint8_t i, bool on, bool retained = true) {
  String topic = mqttBase + "/led/" + String(i + 1) + "/state";
  mqtt.publish(topic.c_str(), on ? "ON" : "OFF", retained);
}

static void mqttReconnect() {
  if (currentMode != MODE_STA) return;
  if (mqttHost.isEmpty()) return;
  if (mqtt.connected()) return;
  uint32_t now = millis();
  if (mqttLastTry != 0 && (now - mqttLastTry) < MQTT_RETRY_MS) return;
  mqttLastTry = now;

  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(512);
  mqtt.setCallback(mqttCallback);

  String cid = mqttClientId();
  String willTopic = mqttBase + "/status";

  Serial.printf("[mqtt] connecting to %s:%u as %s\n",
                mqttHost.c_str(), mqttPort, cid.c_str());

  bool ok;
  if (mqttUser.isEmpty()) {
    ok = mqtt.connect(cid.c_str(), nullptr, nullptr,
                      willTopic.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(cid.c_str(), mqttUser.c_str(), mqttPass.c_str(),
                      willTopic.c_str(), 1, true, "offline");
  }

  mqttLastState = mqtt.state();
  if (!ok) {
    Serial.printf("[mqtt] connect failed, state=%d\n", mqttLastState);
    return;
  }
  Serial.println("[mqtt] connected");
  mqtt.publish(willTopic.c_str(), "online", true);

  String subTopic = mqttBase + "/sw/+/trigger";
  mqtt.subscribe(subTopic.c_str());
  Serial.printf("[mqtt] subscribed %s\n", subTopic.c_str());

  // Publish current LED states (retained) on (re)connect.
  for (uint8_t i = 0; i < 5; i++) {
    bool on = readLed(i);
    mqttPublishLed(i, on, true);
    ledLastPub[i] = on;
  }
  ledStateInit = true;
}

static void mqttPublishChanges() {
  if (!mqtt.connected()) return;
  uint32_t now = millis();
  bool heartbeatDue = (mqttPubIntervalSec > 0) &&
                      ((now - mqttLastPubAt) >= (uint32_t)mqttPubIntervalSec * 1000UL);
  for (uint8_t i = 0; i < 5; i++) {
    bool on = readLed(i);
    if (!ledStateInit || on != ledLastPub[i] || heartbeatDue) {
      mqttPublishLed(i, on, true);
      ledLastPub[i] = on;
    }
  }
  if (heartbeatDue || !ledStateInit) mqttLastPubAt = now;
  ledStateInit = true;
}

static void mqttApplyConfig() {
  // Called after settings change. Drop existing connection so reconnect
  // picks up the new server/credentials.
  if (mqtt.connected()) {
    String willTopic = mqttBase + "/status";
    mqtt.publish(willTopic.c_str(), "offline", true);
    mqtt.disconnect();
  }
  mqttLastTry = 0;  // try again immediately
  ledStateInit = false;
}

// ---- HTTP handlers --------------------------------------------------------
static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleStatus() {
  String body;
  body.reserve(96);
  body += F("{\"leds\":[");
  for (uint8_t i = 0; i < 5; i++) {
    body += readLed(i) ? '1' : '0';
    if (i < 4) body += ',';
  }
  body += F("],\"pulses\":[");
  uint32_t now = millis();
  for (uint8_t i = 0; i < 5; i++) {
    bool active = (pulseEndAt[i] != 0) && ((int32_t)(now - pulseEndAt[i]) < 0);
    body += active ? '1' : '0';
    if (i < 4) body += ',';
  }
  body += F("]}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

static void handleNetinfo() {
  String savedSsid = prefs.getString("ssid", "");
  String body;
  body.reserve(160);
  body += F("{\"mode\":\"");
  body += (currentMode == MODE_STA ? "STA" : "AP");
  body += F("\",\"ssid\":\"");
  if (currentMode == MODE_STA) {
    body += jsonEscape(WiFi.SSID());
  } else {
    body += jsonEscape(String(AP_SSID));
  }
  body += F("\",\"ip\":\"");
  body += (currentMode == MODE_STA
           ? WiFi.localIP().toString()
           : WiFi.softAPIP().toString());
  body += F("\",\"rssi\":");
  body += (currentMode == MODE_STA ? String(WiFi.RSSI()) : String(0));
  body += F(",\"savedSsid\":\"");
  body += jsonEscape(savedSsid);
  body += F("\"}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

static void handleTrigger() {
  if (!server.hasArg("sw")) {
    server.send(400, "text/plain", "missing sw");
    return;
  }
  int sw = server.arg("sw").toInt();
  if (sw < 1 || sw > 5) {
    server.send(400, "text/plain", "sw out of range");
    return;
  }
  startPulse((uint8_t)(sw - 1));
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleConfigGet() {
  String body;
  body.reserve(48);
  body += F("{\"pulseMs\":");
  body += pulseMs;
  body += F(",\"min\":");
  body += PULSE_MS_MIN;
  body += F(",\"max\":");
  body += PULSE_MS_MAX;
  body += F("}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

static void handleConfigSet() {
  if (!server.hasArg("pulseMs")) {
    server.send(400, "text/plain", "missing pulseMs");
    return;
  }
  long v = server.arg("pulseMs").toInt();
  if (v < (long)PULSE_MS_MIN || v > (long)PULSE_MS_MAX) {
    server.send(400, "text/plain", "pulseMs out of range");
    return;
  }
  pulseMs = (uint32_t)v;
  prefs.putULong("pulse_ms", pulseMs);
  Serial.printf("[cfg] pulse_ms = %u\n", (unsigned)pulseMs);
  server.send(200, "application/json", "{\"ok\":true}");
}

static const char* mqttStateStr(int s) {
  switch (s) {
    case -4: return "timeout";
    case -3: return "lost";
    case -2: return "failed";
    case -1: return "disconnected";
    case 0:  return "connected";
    case 1:  return "bad-protocol";
    case 2:  return "bad-client-id";
    case 3:  return "unavailable";
    case 4:  return "bad-credentials";
    case 5:  return "unauthorized";
    default: return "unknown";
  }
}

static void handleMqttGet() {
  bool connected = mqtt.connected();
  int state = connected ? 0 : mqttLastState;
  String body;
  body.reserve(256);
  body += F("{\"host\":\"");   body += jsonEscape(mqttHost);
  body += F("\",\"port\":");   body += mqttPort;
  body += F(",\"user\":\"");   body += jsonEscape(mqttUser);
  body += F("\",\"base\":\"");  body += jsonEscape(mqttBase);
  body += F("\",\"pubIntervalSec\":"); body += mqttPubIntervalSec;
  body += F(",\"pubIntervalMax\":"); body += PUB_INTERVAL_MAX_SEC;
  body += F(",\"connected\":"); body += (connected ? "true" : "false");
  body += F(",\"state\":\""); body += mqttStateStr(state);
  body += F("\",\"clientId\":\""); body += jsonEscape(mqttClientId());
  body += F("\",\"enabled\":"); body += (mqttHost.isEmpty() ? "false" : "true");
  body += F("}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

static void handleMqttSet() {
  // host="" disables MQTT.
  String host = server.hasArg("host") ? server.arg("host") : String("");
  long port   = server.hasArg("port") ? server.arg("port").toInt() : 1883;
  String user = server.hasArg("user") ? server.arg("user") : String("");
  String pass = server.hasArg("pass") ? server.arg("pass") : String("");
  String base = server.hasArg("base") ? server.arg("base") : String("flexit");
  host.trim(); user.trim(); base.trim();

  if (port < 1 || port > 65535) {
    server.send(400, "text/plain", "port out of range");
    return;
  }
  if (host.length() > 64 || user.length() > 64 || pass.length() > 64 ||
      base.length() > 32 || base.isEmpty()) {
    server.send(400, "text/plain", "field length out of range");
    return;
  }
  // Disallow leading/trailing slashes in base.
  while (base.startsWith("/")) base.remove(0, 1);
  while (base.endsWith("/"))   base.remove(base.length() - 1);
  if (base.isEmpty()) base = "flexit";

  long pubInterval = server.hasArg("pubIntervalSec")
                        ? server.arg("pubIntervalSec").toInt()
                        : (long)mqttPubIntervalSec;
  if (pubInterval < 0 || pubInterval > (long)PUB_INTERVAL_MAX_SEC) {
    server.send(400, "text/plain", "pubIntervalSec out of range");
    return;
  }

  prefs.putString("mq_host", host);
  prefs.putUShort("mq_port", (uint16_t)port);
  prefs.putString("mq_user", user);
  prefs.putString("mq_pass", pass);
  prefs.putString("mq_base", base);
  prefs.putUShort("mq_pubint", (uint16_t)pubInterval);

  mqttHost = host;
  mqttPort = (uint16_t)port;
  mqttUser = user;
  mqttPass = pass;
  mqttBase = base;
  mqttPubIntervalSec = (uint16_t)pubInterval;
  mqttLastPubAt = millis();  // reset heartbeat phase

  Serial.printf("[mqtt] saved host=%s port=%u base=%s user=%s\n",
                mqttHost.c_str(), mqttPort, mqttBase.c_str(),
                mqttUser.isEmpty() ? "(none)" : mqttUser.c_str());

  mqttApplyConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleWifiSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "missing ssid");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : String("");
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32) {
    server.send(400, "text/plain", "ssid empty or too long");
    return;
  }
  if (pass.length() > 64) {
    server.send(400, "text/plain", "pass too long");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  Serial.printf("[wifi] new credentials saved: ssid=\"%s\"\n", ssid.c_str());
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  rebootAt = millis() + 800;
  pendingReboot = true;
}

static void handleWifiClear() {
  prefs.remove("ssid");
  prefs.remove("pass");
  Serial.println("[wifi] credentials cleared");
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  rebootAt = millis() + 800;
  pendingReboot = true;
}

static void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

// ---- Wi-Fi bring-up -------------------------------------------------------
static bool connectSTA(const String& ssid, const String& pass) {
  if (ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[wifi] joining \"%s\"", ssid.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - t0) < STA_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] STA connected, IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("[wifi] STA connect timed out");
  WiFi.disconnect(true, true);
  return false;
}

static void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[wifi] AP \"%s\" started, IP=%s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
}

// ---- Arduino setup / loop -------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] knappepanel_flexit / ESP32-C5");

  for (uint8_t i = 0; i < 5; i++) {
    pinMode(SW_PINS[i], OUTPUT);
    digitalWrite(SW_PINS[i], LOW);
  }
  for (uint8_t i = 0; i < 5; i++) {
    pinMode(LED_PINS[i],
            LED_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  }

  prefs.begin("wifi", false);
  String savedSsid = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  pulseMs = prefs.getULong("pulse_ms", PULSE_MS_DEFAULT);
  if (pulseMs < PULSE_MS_MIN || pulseMs > PULSE_MS_MAX) pulseMs = PULSE_MS_DEFAULT;
  Serial.printf("[cfg] pulse_ms = %u\n", (unsigned)pulseMs);

  mqttHost = prefs.getString("mq_host", "");
  mqttPort = prefs.getUShort("mq_port", 1883);
  mqttUser = prefs.getString("mq_user", "");
  mqttPass = prefs.getString("mq_pass", "");
  mqttBase = prefs.getString("mq_base", "flexit");
  if (mqttBase.isEmpty()) mqttBase = "flexit";
  mqttPubIntervalSec = prefs.getUShort("mq_pubint", 0);
  if (mqttPubIntervalSec > PUB_INTERVAL_MAX_SEC) mqttPubIntervalSec = 0;
  Serial.printf("[cfg] mqtt host=%s port=%u base=%s heartbeat=%us\n",
                mqttHost.isEmpty() ? "(disabled)" : mqttHost.c_str(),
                mqttPort, mqttBase.c_str(), (unsigned)mqttPubIntervalSec);

  if (connectSTA(savedSsid, savedPass)) {
    currentMode = MODE_STA;
  } else {
    startAP();
    currentMode = MODE_AP;
  }

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mdns] http://%s.local/\n", HOSTNAME);
  }

  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/status",    HTTP_GET,  handleStatus);
  server.on("/netinfo",   HTTP_GET,  handleNetinfo);
  server.on("/trigger",   HTTP_POST, handleTrigger);
  server.on("/wifi",      HTTP_POST, handleWifiSave);
  server.on("/wifi/clear",HTTP_POST, handleWifiClear);
  server.on("/config",    HTTP_GET,  handleConfigGet);
  server.on("/config",    HTTP_POST, handleConfigSet);
  server.on("/mqtt",      HTTP_GET,  handleMqttGet);
  server.on("/mqtt",      HTTP_POST, handleMqttSet);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[http] server listening on :80");
}

void loop() {
  server.handleClient();
  servicePulses();

  mqttReconnect();
  if (mqtt.connected()) {
    mqtt.loop();
    mqttPublishChanges();
  }

  if (pendingReboot && (int32_t)(millis() - rebootAt) >= 0) {
    Serial.println("[sys] rebooting");
    delay(50);
    ESP.restart();
  }
}
