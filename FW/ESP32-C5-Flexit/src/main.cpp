#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include "index_html.h"

// ---- Firmware identity ----------------------------------------------------
#define FW_VERSION "0.2.0"
#define FW_BUILD   __DATE__ " " __TIME__

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
// The AP password is generated per-device on first boot (derived from the MAC),
// stored in NVS, and surfaced on the serial console and the web UI under Wi-Fi.
// The user can override it from the UI. Default no longer ships in the
// public source.
static const char* AP_SSID  = "Flexit-Setup";
static const char* HOSTNAME = "flexit";
static constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;
static String apPassword;  // loaded/generated in setup()

// Periodic STA-retry from AP fallback: if we landed in AP mode at boot (router
// was unreachable), retry STA every STA_RETRY_INTERVAL_MS so a transient outage
// doesn't strand the panel in AP forever.
static constexpr uint32_t STA_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 min
static uint32_t lastStaRetryAt = 0;

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

// HTTP OTA bookkeeping.
//   otaActive : true while a multipart /update upload is being processed.
//               Used to suppress button triggers (/trigger and MQTT) so a
//               concurrent press can't keep the opto latched through the upload.
//   otaAborted: sticky abort flag set the moment Update reports an error so
//               subsequent chunks short-circuit and we surface the error early.
static volatile bool otaActive  = false;
static bool          otaAborted = false;
static uint32_t      otaStartMs = 0;
static uint32_t      otaLastByteMs = 0;
static constexpr uint32_t OTA_IDLE_TIMEOUT_MS  = 10000;   // 10 s without bytes -> abort
static constexpr uint32_t OTA_TOTAL_TIMEOUT_MS = 120000;  // 120 s total upload cap

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

// Forcefully drop every in-flight pulse. Called when a long blocking op
// (HTTP OTA upload, ArduinoOTA) is about to take over the main loop and
// servicePulses() won't run again until reboot.
static void clearPulsesNow() {
  for (uint8_t i = 0; i < 5; i++) {
    digitalWrite(SW_PINS[i], LOW);
    pulseEndAt[i] = 0;
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
  // Ignore triggers while an OTA upload is being processed — a press now
  // would latch through the long-blocking upload.
  if (otaActive) {
    Serial.println("[mqtt] trigger ignored: ota in progress");
    return;
  }
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

// Publish a graceful 'offline' on the status topic and tear down the MQTT
// session so subscribers see an immediate state change rather than waiting
// for the broker's keepalive (30 s) to expire after the device reboots.
// Safe to call when MQTT is disabled or not connected — it's a no-op.
static void mqttPublishOfflineAndDisconnect() {
  if (!mqtt.connected()) return;
  String willTopic = mqttBase + "/status";
  mqtt.publish(willTopic.c_str(), "offline", true);
  mqtt.loop();         // flush the publish over the wire
  mqtt.disconnect();
  netClient.stop();    // close the underlying TCP socket
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
  body.reserve(224);
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
  // Surface the AP password so the user can see / copy it from a phone that
  // has joined the setup AP. Only included when currently in AP mode — when
  // joined to the home network we don't echo it.
  body += F("\",\"apSsid\":\"");
  body += jsonEscape(String(AP_SSID));
  if (currentMode == MODE_AP) {
    body += F("\",\"apPass\":\"");
    body += jsonEscape(apPassword);
  }
  body += F("\"}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

static void handleTrigger() {
  if (otaActive) {
    server.send(503, "application/json",
                "{\"ok\":false,\"error\":\"ota in progress\"}");
    return;
  }
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
  // Validate every field BEFORE we touch NVS — otherwise an invalid apPass
  // would land us with the new ssid/pass already persisted but the response
  // reporting failure (a real footgun for the user).
  String apPass;
  if (server.hasArg("apPass")) {
    apPass = server.arg("apPass");
    apPass.trim();
    if (!apPass.isEmpty() && (apPass.length() < 8 || apPass.length() > 63)) {
      server.send(400, "text/plain", "apPass must be 8..63 chars (WPA2)");
      return;
    }
  }

  // All validated; commit atomically-ish (NVS doesn't have transactions, but
  // these three writes share the same call site and execute back-to-back).
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  if (!apPass.isEmpty()) {
    prefs.putString("ap_pass", apPass);
    apPassword = apPass;
    Serial.println("[wifi] AP password updated");
  }
  Serial.printf("[wifi] new credentials saved: ssid=\"%s\"\n", ssid.c_str());
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  mqttPublishOfflineAndDisconnect();
  rebootAt = millis() + 800;
  pendingReboot = true;
}

static void handleWifiClear() {
  prefs.remove("ssid");
  prefs.remove("pass");
  Serial.println("[wifi] credentials cleared");
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  mqttPublishOfflineAndDisconnect();
  rebootAt = millis() + 800;
  pendingReboot = true;
}

static void handleVersionGet() {
  String body;
  body.reserve(192);
  body += F("{\"version\":\"");   body += FW_VERSION;
  body += F("\",\"build\":\"");   body += FW_BUILD;
  body += F("\",\"sdk\":\"");     body += ESP.getSdkVersion();
  body += F("\",\"chip\":\"");    body += ESP.getChipModel();
  body += F("\",\"flashSize\":"); body += ESP.getFlashChipSize();
  body += F(",\"freeHeap\":");    body += ESP.getFreeHeap();
  body += F(",\"sketchSize\":");  body += ESP.getSketchSize();
  body += F(",\"freeSketch\":");  body += ESP.getFreeSketchSpace();
  body += F("}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

// HTTP OTA: end-user firmware upload via the web UI.
//   POST /update with multipart/form-data field "firmware" = <bin file>
//
// Robustness notes:
//   - clearPulsesNow() at UPLOAD_FILE_START drops every opto-coupler line LOW
//     so a press fired right before the upload doesn't stay latched through
//     the (potentially many-seconds-long) multipart parse.
//   - otaActive blocks /trigger and MQTT-fired triggers from re-arming the
//     opto while an upload is in flight.
//   - otaAborted is a sticky flag: once Update reports any error, subsequent
//     chunks short-circuit so we don't pretend to keep writing to a dead
//     Update session.
//   - OTA_IDLE_TIMEOUT_MS / OTA_TOTAL_TIMEOUT_MS protect against slow-loris
//     uploads that would otherwise wedge the main loop forever.
//   - MQTT publishes 'offline' explicitly before the deferred reboot so
//     subscribers see an immediate flap rather than the broker's 30 s
//     keepalive expiry.
static void handleUpdateUploadComplete() {
  server.sendHeader("Connection", "close");
  bool ok = !otaAborted && !Update.hasError();
  if (!ok) {
    String reason = otaAborted ? String("aborted by timeout or short-write")
                                : String(Update.errorString());
    String msg = F("{\"ok\":false,\"error\":\"");
    msg += jsonEscape(reason);
    msg += F("\"}");
    server.send(500, "application/json", msg);
    Serial.printf("[ota-http] error: %s\n", reason.c_str());
    otaActive = false;
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  Serial.println("[ota-http] success — rebooting");
  // Drain MQTT (publish 'offline' retained) before reboot so subscribers
  // see the flap immediately.
  mqttPublishOfflineAndDisconnect();
  rebootAt = millis() + 800;  // matches the wifi-save path; gives Wi-Fi enough
                              // airtime to flush the 200 response.
  pendingReboot = true;
  otaActive = false;
}

static void handleUpdateUploadChunk() {
  HTTPUpload& up = server.upload();
  uint32_t now = millis();

  // Sticky abort: once anything goes wrong, swallow the rest of the multipart
  // body so the client closes cleanly and the completion handler can return
  // an error. Don't call Update.write() again after an error — the underlying
  // esp_ota_write is in a poisoned state.
  if (otaAborted && up.status != UPLOAD_FILE_END &&
      up.status != UPLOAD_FILE_ABORTED) {
    yield();
    return;
  }

  switch (up.status) {
    case UPLOAD_FILE_START: {
      Serial.printf("[ota-http] begin: %s\n", up.filename.c_str());
      clearPulsesNow();             // matches ArduinoOTA.onStart behaviour
      otaActive  = true;
      otaAborted = false;
      otaStartMs = now;
      otaLastByteMs = now;
      // If a client has authenticated by reaching this point, give the socket
      // enough idle time to ride a slow link without the WebServer's default
      // HTTP_MAX_SEND_WAIT (5 s) killing us mid-upload.
      WiFiClient c = server.client();
      if (c) c.setTimeout(15000);
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        otaAborted = true;
      }
      break;
    }
    case UPLOAD_FILE_WRITE: {
      otaLastByteMs = now;
      if ((now - otaStartMs) > OTA_TOTAL_TIMEOUT_MS) {
        Serial.println("[ota-http] total upload timeout — aborting");
        Update.abort();
        otaAborted = true;
        break;
      }
      size_t written = Update.write(up.buf, up.currentSize);
      if (written != up.currentSize) {
        Serial.printf("[ota-http] short write: %u of %u\n",
                      (unsigned)written, (unsigned)up.currentSize);
        Update.printError(Serial);
        Update.abort();
        otaAborted = true;
      }
      break;
    }
    case UPLOAD_FILE_END:
      // If we already aborted, don't run Update.end() — it would log a second
      // (misleading) error. The completion handler will surface the failure.
      if (otaAborted) {
        Serial.println("[ota-http] end after abort; skipping Update.end()");
        break;
      }
      if (Update.end(true)) {
        Serial.printf("[ota-http] received %u bytes\n", up.totalSize);
      } else {
        Update.printError(Serial);
        otaAborted = true;
      }
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      otaAborted = true;
      // The completion handler is NOT called when WebServer's multipart
      // parser aborts (it only fires after a successful parse), so we have
      // to clear the active flag here ourselves — otherwise /trigger and
      // MQTT triggers stay blocked until reboot.
      otaActive = false;
      Serial.println("[ota-http] aborted by client");
      break;
  }

  // Enforce idle timeout even when chunks aren't arriving — the WebServer's
  // per-byte read uses delay(2), so a slow-loris that sends a header byte
  // every ~4.9 s never reaches our handler. The check still helps when bytes
  // come in fits and starts.
  if (otaActive && !otaAborted &&
      (now - otaLastByteMs) > OTA_IDLE_TIMEOUT_MS) {
    Serial.println("[ota-http] idle timeout — aborting");
    Update.abort();
    otaAborted = true;
  }
  yield();
}

static void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

// ---- AP password helpers --------------------------------------------------
// Generate a stable, per-device default AP password derived from the chip MAC.
// 12 chars, alphanumeric (32-char alphabet, excluding I/O/0/1 to avoid OCR
// confusion). Deterministic so the same chip always shows the same password;
// the user can still override it in the web UI (stored under "ap_pass" in NVS).
//
// This trades pure secrecy for deterministic recovery: if you have the chip,
// you can compute the default password without having captured a serial log.
// Anyone *without* physical access can still only guess, so the public-repo
// PSK problem (anyone reading the source knew the password) is gone.
static String defaultApPassword() {
  uint64_t mac = ESP.getEfuseMac();
  // Mix the MAC bytes so the password isn't just a recoded MAC.
  uint64_t mix = mac;
  mix ^= (mix >> 17);
  mix *= 0x9E3779B97F4A7C15ULL;
  mix ^= (mix >> 31);
  mix *= 0xC6BC279692B5C323ULL;
  static const char alphabet[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // 32 unambiguous chars
  char buf[13];
  for (int i = 0; i < 12; i++) {
    buf[i] = alphabet[(mix >> (i * 5)) & 0x1F];
  }
  buf[12] = 0;
  return String(buf);
}

// ---- ArduinoOTA (PlatformIO `pio run -e ...-ota -t upload`) ---------------
static bool arduinoOtaStarted = false;

static void setupArduinoOTA() {
  if (arduinoOtaStarted) {
    // Re-bind after a Wi-Fi reconnect.
    ArduinoOTA.end();
  }
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setRebootOnSuccess(true);
  ArduinoOTA.onStart([]() {
    Serial.printf("[arduino-ota] start: %s\n",
                  ArduinoOTA.getCommand() == U_FLASH ? "flash" : "filesystem");
    clearPulsesNow();
    mqttPublishOfflineAndDisconnect();
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
    static unsigned int lastPct = 255;
    unsigned int pct = total ? (p * 100u) / total : 0;
    if (pct != lastPct && (pct % 10 == 0)) {
      Serial.printf("[arduino-ota] %u%%\n", pct);
      lastPct = pct;
    }
  });
  ArduinoOTA.onEnd([]() { Serial.println("[arduino-ota] complete"); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[arduino-ota] error %u\n", (unsigned)e);
  });
  ArduinoOTA.begin();
  arduinoOtaStarted = true;
  Serial.printf("[arduino-ota] ready on %s.local:3232\n", HOSTNAME);
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
  WiFi.softAP(AP_SSID, apPassword.c_str());
  Serial.printf("[wifi] AP \"%s\" started, IP=%s, password=\"%s\"\n",
                AP_SSID, WiFi.softAPIP().toString().c_str(),
                apPassword.c_str());
}

// Wi-Fi event hook: when STA gets a new IP (initial join or after roam /
// reconnect), re-announce mDNS and re-bind the ArduinoOTA UDP/TCP listeners
// so wireless reflash keeps working after a router reboot. Since this fires
// every time STA picks up an IP, including the very first connect during
// setup(), setup() itself no longer does the initial mDNS / ArduinoOTA bring-
// up for STA — only for the AP-fallback case.
static void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t /*info*/) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi] STA got IP=%s\n",
                    WiFi.localIP().toString().c_str());
      // Bounce mDNS so the new IP gets advertised.
      MDNS.end();
      if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[mdns] http://%s.local/\n", HOSTNAME);
      }
      setupArduinoOTA();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // Auto-reconnect is handled by the ESP-IDF; no action needed here.
      break;
    default:
      break;
  }
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

  // AP password: load from NVS, or generate a per-device default and persist.
  apPassword = prefs.getString("ap_pass", "");
  if (apPassword.length() < 8) {
    apPassword = defaultApPassword();
    prefs.putString("ap_pass", apPassword);
    Serial.println("[wifi] generated per-device AP password and saved to NVS");
  }
  Serial.printf("[wifi] AP fallback password: \"%s\"\n", apPassword.c_str());

  // Register Wi-Fi event handler before we call begin() so STA_GOT_IP fires.
  WiFi.onEvent(wifiEventHandler);

  if (connectSTA(savedSsid, savedPass)) {
    currentMode = MODE_STA;
  } else {
    startAP();
    currentMode = MODE_AP;
  }
  lastStaRetryAt = millis();

  // For STA mode, mDNS + ArduinoOTA come up from wifiEventHandler on
  // STA_GOT_IP (the event fires at the end of connectSTA's polling loop).
  // For AP-only mode (no STA join), do the initial setup here.
  if (currentMode == MODE_AP) {
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mdns] http://%s.local/ (AP)\n", HOSTNAME);
    }
    // ArduinoOTA is intentionally not started on AP-only mode (no useful
    // reach for a network-side update tool).
  }

  Serial.printf("[boot] firmware %s (built %s)\n", FW_VERSION, FW_BUILD);

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
  server.on("/version",   HTTP_GET,  handleVersionGet);
  server.on("/update",    HTTP_POST,
            handleUpdateUploadComplete,
            handleUpdateUploadChunk);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[http] server listening on :80");
}

void loop() {
  server.handleClient();
  servicePulses();

  if (currentMode == MODE_STA) {
    ArduinoOTA.handle();
  }

  mqttReconnect();
  if (mqtt.connected()) {
    mqtt.loop();
    mqttPublishChanges();
  }

  // Periodic STA retry from AP fallback: if we landed in AP at boot because
  // the router was unreachable, retry STA every STA_RETRY_INTERVAL_MS so a
  // transient outage doesn't strand the device on the setup AP forever.
  // Suppress while OTA is active (we don't want to drop Wi-Fi during a flash).
  if (currentMode == MODE_AP && !otaActive &&
      (millis() - lastStaRetryAt) > STA_RETRY_INTERVAL_MS) {
    lastStaRetryAt = millis();
    String savedSsid = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");
    if (!savedSsid.isEmpty()) {
      Serial.println("[wifi] periodic STA retry from AP fallback...");
      WiFi.softAPdisconnect(true);
      if (connectSTA(savedSsid, savedPass)) {
        currentMode = MODE_STA;
        // mDNS + ArduinoOTA come back up via wifiEventHandler on STA_GOT_IP.
      } else {
        // STA still down — return to AP mode.
        startAP();
      }
    }
  }

  if (pendingReboot && (int32_t)(millis() - rebootAt) >= 0) {
    Serial.println("[sys] rebooting");
    // Clean teardown so a slow-Wi-Fi response gets flushed before reset.
    server.close();
    WiFi.disconnect(true, false);
    delay(150);
    ESP.restart();
  }
}
