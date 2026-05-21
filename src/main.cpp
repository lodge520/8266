#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <BH1750.h>

// ===================== 传感器 / 外设 =====================
BH1750 lightMeter;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
WebSocketsClient webSocket;
ESP8266WebServer server(80);
WiFiUDP udp;

#define nanoSerial Serial1
#define DEBUG_SERIAL Serial

// ===================== 引脚定义 =====================
#define LED_COLD_PIN D2
#define LED_WARM_PIN D1
#define BLUR         D7
#define TOF_SDA_PIN  D5
#define TOF_SCL_PIN  D6

// ===================== 固件信息 =====================
#define FW_DEVICE_TYPE  "lamp"
#define FW_VERSION      "1.0.2"
#define FW_VERSION_CODE 10002
#define FW_CHANNEL      "stable"

// ===================== 命名常量（原魔法数字） =====================
const float   WAVE_FREQ_FACTOR          = 0.32f;
const float   TOF_TRANSITION_MS         = 2000.0f;
const uint16_t TOF_MAX_RANGE_MM         = 8200;
const unsigned long TOF_DEBOUNCE_MS     = 1000;
const unsigned long TOF_READ_INTERVAL_MS = 50;
const char*   AP_DEFAULT_PASSWORD       = "12345678";
const char*   WS_PATH                   = "/ws/device";
const int     LOCATE_MIN_BRIGHTNESS     = 5;
const int     LOCATE_MAX_BRIGHTNESS     = 100;
const int     LOCATE_STEPS              = 36;

// ===================== 默认服务器配置 =====================
const char* DEFAULT_SERVER_HOST = "device.genius.show";
const uint16_t DEFAULT_HTTP_PORT = 80;
const uint16_t DEFAULT_WS_PORT   = 80;

// ===================== 定时参数 =====================
const unsigned long lightSendInterval    = 30000;
const unsigned long lightUpdateInterval  = 50;
const unsigned long wifiConnectTimeout   = 15000;
const unsigned long smartConfigTimeout   = 60000;
const unsigned long announceInterval     = 5000;
const unsigned long broadcastInterval    = 5000;
const unsigned long wsPingInterval       = 5000;
const unsigned long otaProgressReportMinIntervalMs = 3000;
const int           otaProgressReportMinStep       = 5;

const int udpPort = 4210;
static const uint32_t NANO_BAUD = 57600;

// ===================== 运行状态 =====================
bool bh1750Ready = false;
bool tofReady = false;
bool enableBroadcast = true;
bool enableAnnounce = true;
bool portalMode = false;
bool otaInProgress = false;
String firmwareChannel = FW_CHANNEL;
String otaStatus = "idle";
int otaProgress = 0;
int lastOtaProgressLog = -1;
int lastOtaProgressReport = -1;
unsigned long lastOtaProgressReportMs = 0;

unsigned long lastLightSend = 0;
unsigned long lastLightUpdate = 0;
unsigned long lastAnnounce = 0;
unsigned long lastBroadcast = 0;
unsigned long lastPing = 0;
unsigned long lastToFRead = 0;

// 广播IP缓存
IPAddress cachedBroadcastIP;
bool broadcastIPCached = false;

// ===================== 灯光控制参数 =====================
int brightness = 80;
int temp = 4000;
bool autoMode = true;
int recommendedBrightness = 80;
int recommendedTemp = 4000;
char fabric[16] = "unknown";

bool effectWaveEnabled = false;
int effectBaseTemp = 3800;
int effectRange = 500;
float effectSpeed = 1.0f;
int effectBrightness = 80;
float effectPhaseOffset = 0.0f;
int effectRestoreBrightness = 80;
int effectRestoreTemp = 4000;
unsigned long effectStartMs = 0;
unsigned long lastEffectUpdateMs = 0;
const unsigned long WAVE_UPDATE_INTERVAL_MS = 80;

// ===================== Nano 云台 / 滑轨控制参数 =====================
static const int PAN_MIN = -90;
static const int PAN_MAX = 90;
static const int TILT_MIN = -45;
static const int TILT_MAX = 45;
static const int SLIDER_MIN = 0;
static const int SLIDER_MAX = 1200;

int panDeg = 0;
int tiltDeg = 0;
int sliderMm = 0;
int angleStep = 5;
int sliderStep = 50;
int panSpeedDeg = 60;
int tiltSpeedDeg = 60;
int sliderSpeedMm = 100;

// ===================== 设备配置 =====================
struct DeviceConfig {
  String ssid;
  String password;
  String serverHost;
  uint16_t httpPort;
  uint16_t wsPort;
};

DeviceConfig cfg;

// 设备唯一ID：默认用 lamp-芯片ID
String deviceId;

void beginWebSocketClient();
void locateBreath(int times, int cycleMs);
void sendDeviceStateReport();
void handleArmAction(const String& action, const String& speed);
void pollNano();
void applyLightSettings(int br, int tp);
void stopEffectWaveForManualControl();
void updateEffectLoop();
void addCorsHeaders();
void addCorsHeadersWithMethods();
int  postJsonToServer(const String& path, const String& jsonBody);
void ensureConfigDefaults(DeviceConfig& c);
void refreshBroadcastIP();
// ===================== 工具函数 =====================
String configPath() {
  return "/config.json";
}

String makeDeviceId() {
  String id = "lamp-";
  id += String(ESP.getChipId(), HEX);
  id.toUpperCase();
  return id;
}

String httpUrl(const String& path) {
  return "http://" + cfg.serverHost + ":" + String(cfg.httpPort) + path;
}

IPAddress calcBroadcastIP() {
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; i++) {
    broadcast[i] = ip[i] | (~mask[i]);
  }
  return broadcast;
}

void safeCopyFabric(const char* src) {
  if (!src || strlen(src) == 0) return;
  snprintf(fabric, sizeof(fabric), "%s", src);
}

int compareVersion(const String& a, const String& b) {
  int a1 = 0, a2 = 0, a3 = 0;
  int b1 = 0, b2 = 0, b3 = 0;
  sscanf(a.c_str(), "%d.%d.%d", &a1, &a2, &a3);
  sscanf(b.c_str(), "%d.%d.%d", &b1, &b2, &b3);

  if (a1 != b1) return (a1 > b1) ? 1 : -1;
  if (a2 != b2) return (a2 > b2) ? 1 : -1;
  if (a3 != b3) return (a3 > b3) ? 1 : -1;
  return 0;
}

// ---- 通用 HTTP / CORS / 配置 辅助 ----

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

void addCorsHeadersWithMethods() {
  addCorsHeaders();
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

int postJsonToServer(const String& path, const String& jsonBody) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, httpUrl(path));
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(jsonBody);
  if (httpCode > 0) {
    DEBUG_SERIAL.printf("[HTTP] POST %s -> %d\n", path.c_str(), httpCode);
    DEBUG_SERIAL.println(http.getString());
  } else {
    DEBUG_SERIAL.printf("[HTTP] POST %s failed: %s\n", path.c_str(), http.errorToString(httpCode).c_str());
  }
  http.end();
  return httpCode;
}

void ensureConfigDefaults(DeviceConfig& c) {
  if (c.serverHost.length() == 0) c.serverHost = DEFAULT_SERVER_HOST;
  if (c.httpPort == 0) c.httpPort = DEFAULT_HTTP_PORT;
  if (c.wsPort == 0) c.wsPort = DEFAULT_WS_PORT;
}

void refreshBroadcastIP() {
  cachedBroadcastIP = calcBroadcastIP();
  broadcastIPCached = true;
}

// ===================== 配置读写 =====================
bool saveConfig(const DeviceConfig& c) {
  StaticJsonDocument<256> doc;
  doc["ssid"] = c.ssid;
  doc["password"] = c.password;
  doc["serverHost"] = c.serverHost;
  doc["httpPort"] = c.httpPort;
  doc["wsPort"] = c.wsPort;

  File f = LittleFS.open(configPath(), "w");
  if (!f) return false;

  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

bool loadConfig() {
  cfg.ssid = "";
  cfg.password = "";
  cfg.serverHost = DEFAULT_SERVER_HOST;
  cfg.httpPort = DEFAULT_HTTP_PORT;
  cfg.wsPort = DEFAULT_WS_PORT;

  if (!LittleFS.exists(configPath())) return false;

  File f = LittleFS.open(configPath(), "r");
  if (!f) return false;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  cfg.ssid = doc["ssid"] | "";
  cfg.password = doc["password"] | "";
  cfg.serverHost = doc["serverHost"] | DEFAULT_SERVER_HOST;
  cfg.httpPort = doc["httpPort"] | DEFAULT_HTTP_PORT;
  cfg.wsPort = doc["wsPort"] | DEFAULT_WS_PORT;

  return cfg.ssid.length() > 0;
}

void clearConfig() {
  if (LittleFS.exists(configPath())) {
    LittleFS.remove(configPath());
  }
}

// ===================== 配网页面 =====================
String getPortalHtml() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP8266 配网</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body{font-family:Arial;padding:20px;max-width:560px;margin:auto;}
    input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;}
    button{padding:12px 18px;margin-top:10px;}
    .box{border:1px solid #ddd;border-radius:12px;padding:16px;}
    .tip{color:#666;font-size:14px;}
  </style>
</head>
<body>
  <div class="box">
    <h2>灯节点配网</h2>
    <p>设备ID：__DEVICE_ID__</p>
    <p class="tip">先尝试 SmartConfig。失败后可手动填 Wi-Fi 和服务器信息。</p>

    <form action="/saveWifi" method="POST">
      <label>Wi-Fi 名称</label>
      <input name="ssid" placeholder="请输入 Wi-Fi 名称">

      <label>Wi-Fi 密码</label>
      <input name="password" type="password" placeholder="请输入 Wi-Fi 密码">

      <label>服务器 Host/IP</label>
      <input name="serverHost" value="__SERVER_HOST__">

      <label>HTTP 端口</label>
      <input name="httpPort" value="__HTTP_PORT__">

      <label>WebSocket 端口</label>
      <input name="wsPort" value="__WS_PORT__">

      <button type="submit">保存并重启</button>
    </form>

    <form action="/resetWifi" method="POST">
      <button type="submit">清除配置并重启</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

  html.replace("__DEVICE_ID__", deviceId);
  html.replace("__SERVER_HOST__", cfg.serverHost.length() ? cfg.serverHost : String(DEFAULT_SERVER_HOST));
  html.replace("__HTTP_PORT__", String(cfg.httpPort ? cfg.httpPort : DEFAULT_HTTP_PORT));
  html.replace("__WS_PORT__", String(cfg.wsPort ? cfg.wsPort : DEFAULT_WS_PORT));
  return html;
}

// ===================== WiFi =====================
bool connectWiFi(const String& ssid, const String& password, unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  DEBUG_SERIAL.println("\n[WiFi] 正在连接: " + ssid);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    DEBUG_SERIAL.print(".");
    yield();
  }
  DEBUG_SERIAL.println();

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_SERIAL.println("[WiFi] 连接成功: " + WiFi.localIP().toString());
    return true;
  }

  DEBUG_SERIAL.println("[WiFi] 连接失败");
  return false;
}

bool connectSavedWiFi() {
  if (cfg.ssid.length() == 0) return false;
  return connectWiFi(cfg.ssid, cfg.password, wifiConnectTimeout);
}

bool smartConfigProvision(unsigned long timeoutMs) {
  DEBUG_SERIAL.println("[SmartConfig] 等待批量配网...");
  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();

  unsigned long start = millis();
  while (!WiFi.smartConfigDone() && millis() - start < timeoutMs) {
    delay(500);
    DEBUG_SERIAL.print("#");
    yield();
  }
  DEBUG_SERIAL.println();

  if (!WiFi.smartConfigDone()) {
    DEBUG_SERIAL.println("[SmartConfig] 超时");

    // 关键：停止 SmartConfig，否则后面切 AP 可能失败
    WiFi.stopSmartConfig();
    delay(300);
    WiFi.disconnect(true);
    delay(300);

    return false;
  }

  DEBUG_SERIAL.println("[SmartConfig] 已收到配网信息，等待联网...");
  start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    DEBUG_SERIAL.print(".");
    yield();
  }
  DEBUG_SERIAL.println();

  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_SERIAL.println("[SmartConfig] 联网失败");

    // 这里也要停
    WiFi.stopSmartConfig();
    WiFi.disconnect(true);
    delay(300);

    return false;
  }

  DeviceConfig newCfg;
  newCfg.ssid = WiFi.SSID();
  newCfg.password = WiFi.psk();
  newCfg.serverHost = cfg.serverHost;
  newCfg.httpPort = cfg.httpPort;
  newCfg.wsPort = cfg.wsPort;
  ensureConfigDefaults(newCfg);

  cfg = newCfg;
  saveConfig(cfg);

  WiFi.stopSmartConfig();

  DEBUG_SERIAL.println("[SmartConfig] 成功，已保存配置");
  return true;
}

void startConfigPortal() {
  portalMode = true;

  WiFi.persistent(false);

  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_OFF);
  delay(500);

  WiFi.mode(WIFI_AP);
  delay(300);

  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  bool configOk = WiFi.softAPConfig(apIP, gateway, subnet);
  DEBUG_SERIAL.println(configOk ? "[AP] IP配置成功" : "[AP] IP配置失败");

  String apName = "LightConfig_" + deviceId;

  bool apOk = WiFi.softAP(apName.c_str(), AP_DEFAULT_PASSWORD, 1, false, 4);

  DEBUG_SERIAL.println("[AP] 进入网页配网模式");
  DEBUG_SERIAL.println("[AP] 热点名称: " + apName);
  DEBUG_SERIAL.println("[AP] 密码: " + String(AP_DEFAULT_PASSWORD));

  if (!apOk) {
    DEBUG_SERIAL.println("[AP] 热点启动失败！");
    return;
  }

  delay(500);

  DEBUG_SERIAL.println("[AP] IP: " + WiFi.softAPIP().toString());
  DEBUG_SERIAL.println("[AP] 打开: http://" + WiFi.softAPIP().toString());

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", getPortalHtml());
  });

  server.on("/saveWifi", HTTP_POST, []() {
    DeviceConfig newCfg;
    newCfg.ssid = server.arg("ssid");
    newCfg.password = server.arg("password");
    newCfg.serverHost = server.arg("serverHost");
    newCfg.httpPort = (uint16_t)server.arg("httpPort").toInt();
    newCfg.wsPort = (uint16_t)server.arg("wsPort").toInt();

    if (newCfg.ssid.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "Wi-Fi 名称不能为空");
      return;
    }

    ensureConfigDefaults(newCfg);

    if (!saveConfig(newCfg)) {
      server.send(500, "text/plain; charset=utf-8", "保存失败");
      return;
    }

    server.send(200, "text/html; charset=utf-8", "<h3>保存成功，设备即将重启...</h3>");
    delay(1200);
    ESP.restart();
  });

  server.on("/resetWifi", HTTP_POST, []() {
    clearConfig();
    server.send(200, "text/html; charset=utf-8", "<h3>已清除配置，设备即将重启...</h3>");
    delay(1200);
    ESP.restart();
  });

  server.begin();
}

// ===================== OTA =====================
void otaStarted() {
  DEBUG_SERIAL.println("[OTA] 开始升级");
}
void otaFinished() {
  DEBUG_SERIAL.println("[OTA] 升级完成");
}
void otaProgressCallback(int current, int total) {
  if (total <= 0) return;

  int percent = (current * 100) / total;
  percent = constrain(percent, 0, 100);

  otaProgress = percent;

  if (lastOtaProgressLog < 0 || percent - lastOtaProgressLog >= 5 || percent == 100) {
    lastOtaProgressLog = percent;
    DEBUG_SERIAL.printf("[OTA] progress %d%% (%d/%d)\n", percent, current, total);
  }

  unsigned long now = millis();
  if (
    lastOtaProgressReport < 0 ||
    percent - lastOtaProgressReport >= otaProgressReportMinStep ||
    now - lastOtaProgressReportMs >= otaProgressReportMinIntervalMs ||
    percent == 100
  ) {
    lastOtaProgressReport = percent;
    lastOtaProgressReportMs = now;
    sendDeviceStateReport();
  }

  yield();
}
void otaError(int err) {
  DEBUG_SERIAL.printf("[OTA] 错误码: %d\n", err);
}

void doOtaUpdate(const String& url, const String& version, int versionCode, const String& channel, const String& md5) {
  if (otaInProgress) return;
  otaInProgress = true;
  otaStatus = "updating";
  otaProgress = 0;
  lastOtaProgressLog = -1;
  lastOtaProgressReport = -1;
  lastOtaProgressReportMs = 0;
  firmwareChannel = FW_CHANNEL;
  sendDeviceStateReport();

  DEBUG_SERIAL.println("[OTA] 收到升级通知");
  DEBUG_SERIAL.println("[OTA] 当前版本: " + String(FW_VERSION));
  DEBUG_SERIAL.println("[OTA] 目标版本: " + version);
  DEBUG_SERIAL.println("[OTA] 下载地址: " + url);

  webSocket.disconnect();
  delay(200);

  WiFiClient client;
  ESPhttpUpdate.setClientTimeout(12000);
  ESPhttpUpdate.rebootOnUpdate(false);
  ESPhttpUpdate.onStart(otaStarted);
  ESPhttpUpdate.onEnd(otaFinished);
  ESPhttpUpdate.onProgress(otaProgressCallback);
  ESPhttpUpdate.onError(otaError);
  if (md5.length() > 0) {
    ESPhttpUpdate.setMD5sum(md5.c_str());
  }

  t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      DEBUG_SERIAL.printf("[OTA] 升级失败 (%d): %s\n",
                    ESPhttpUpdate.getLastError(),
                    ESPhttpUpdate.getLastErrorString().c_str());
      otaStatus = "failed";
      sendDeviceStateReport();
      otaInProgress = false;
      beginWebSocketClient();
      break;

    case HTTP_UPDATE_NO_UPDATES:
      otaStatus = "idle";
      otaProgress = 0;
      sendDeviceStateReport();
      DEBUG_SERIAL.println("[OTA] 没有更新");
      otaInProgress = false;
      beginWebSocketClient();
      break;

    case HTTP_UPDATE_OK:
      otaStatus = "success";
      otaProgress = 100;
      sendDeviceStateReport();
      delay(300);
      DEBUG_SERIAL.println("[OTA] 升级成功，设备将自动重启");
      ESP.restart();
      break;
  }
}

// ===================== HTTP 接口 =====================
void doOtaUpdate(const String& url, const String& version) {
  doOtaUpdate(url, version, 0, FW_CHANNEL, "");
}

void handleStatus() {
  addCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"已配网\"}");
}

void handleSetLight() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"缺少 body\"}");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  auto err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  stopEffectWaveForManualControl();

  brightness = doc["brightness"] | brightness;
  temp = doc["temp"] | temp;
  autoMode = doc["auto"] | autoMode;
  recommendedBrightness = doc["recommendedBrightness"] | recommendedBrightness;
  recommendedTemp = doc["recommendedTemp"] | recommendedTemp;
  safeCopyFabric(doc["fabric"]);

  DEBUG_SERIAL.printf("收到 HTTP 控制: bri=%d temp=%d auto=%d recB=%d recT=%d fabric=%s\n",
                brightness, temp, autoMode,
                recommendedBrightness, recommendedTemp, fabric);

  applyLightSettings(autoMode ? recommendedBrightness : brightness, autoMode ? recommendedTemp : temp);
  lastLightUpdate = millis();
  sendDeviceStateReport();

  server.send(200, "application/json", "{\"result\":\"OK\"}");
}

void handleResumeBroadcast() {
  enableBroadcast = true;
  enableAnnounce = true;
  addCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"resumed\"}");
  DEBUG_SERIAL.println("接收到网页指令：恢复广播");
}

void handleStopBroadcast() {
  enableBroadcast = false;
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"Broadcast stopped\"}");
  DEBUG_SERIAL.println("接收到网页指令：停止广播");
}

void handleStopAnnounce() {
  enableAnnounce = false;
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"Announce stopped\"}");
  DEBUG_SERIAL.println("接收到网页指令：停止上报");
}

void handleResetWifi() {
  clearConfig();
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"WiFi config cleared, restarting\"}");
  delay(800);
  ESP.restart();
}

void setupDeviceHttpServer() {
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      addCorsHeadersWithMethods();
      server.send(204);
    } else {
      server.send(404);
    }
  });

  server.on("/status", handleStatus);
  server.on("/setLight", handleSetLight);
  server.on("/stopBroadcast", handleStopBroadcast);
  server.on("/resumeBroadcast", handleResumeBroadcast);
  server.on("/stopAnnounce", handleStopAnnounce);
  server.on("/resetWifi", HTTP_POST, handleResetWifi);

  server.begin();
}

// ===================== Nano 云台 / 滑轨控制 =====================
void sendNano(char cmd, const String& value = "") {
  nanoSerial.print(cmd);
  if (value.length() > 0) {
    nanoSerial.print(value);
  }
  nanoSerial.print('\n');

  delay(25);
  pollNano();


  DEBUG_SERIAL.print("[NANO] TX ");
  DEBUG_SERIAL.print(cmd);
  DEBUG_SERIAL.println(value);
}

void pollNano() {
  static String line;

  while (nanoSerial.available() > 0) {
    char ch = (char)nanoSerial.read();
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (line.length() > 0) {
        DEBUG_SERIAL.println("[NANO] RX " + line);
        line = "";
      }
      continue;
    }

    if (line.length() < 120) {
      line += ch;
    } else {
      line = "";
      DEBUG_SERIAL.println("[NANO] RX line too long, dropped");
    }
  }
}

void sendPanTilt() {
  panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
  tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
  sendNano('p', String(panDeg));
  sendNano('t', String(tiltDeg));
}

void sendSlider() {
  sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
  sendNano('x', String(sliderMm));
}

void applyArmSpeed(const String& speed) {
  String normalized = speed;
  normalized.toLowerCase();

  if (normalized == "slow") {
    angleStep = 2;
    sliderStep = 20;
    panSpeedDeg = 20;
    tiltSpeedDeg = 20;
    sliderSpeedMm = 40;
  } else if (normalized == "fast") {
    angleStep = 10;
    sliderStep = 100;
    panSpeedDeg = 120;
    tiltSpeedDeg = 120;
    sliderSpeedMm = 200;
  } else {
    angleStep = 5;
    sliderStep = 50;
    panSpeedDeg = 60;
    tiltSpeedDeg = 60;
    sliderSpeedMm = 100;
  }

  sendNano('s', String(panSpeedDeg));
  sendNano('S', String(tiltSpeedDeg));
  sendNano('X', String(sliderSpeedMm));
}

void handleArmAction(const String& action, const String& speed) {
  String normalizedAction = action;
  normalizedAction.trim();
  normalizedAction.toLowerCase();

  String normalizedSpeed = speed;
  normalizedSpeed.trim();
  if (normalizedSpeed.length() == 0) {
    normalizedSpeed = "normal";
  }

  if (normalizedAction == "slider_position") {
    DEBUG_SERIAL.println("[ARM] slider_position ignored by lamp firmware");
    return;
  }

  applyArmSpeed(normalizedSpeed);

  if (normalizedAction == "up") {
    tiltDeg += angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', String(tiltDeg));
  } else if (normalizedAction == "down") {
    tiltDeg -= angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', String(tiltDeg));
  } else if (normalizedAction == "left") {
    panDeg -= angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', String(panDeg));
  } else if (normalizedAction == "right") {
    panDeg += angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', String(panDeg));
  } else if (normalizedAction == "center") {
    panDeg = 0;
    tiltDeg = 0;
    sendPanTilt();
  } else if (normalizedAction == "home") {
    sendNano('A');
  } else if (normalizedAction == "stop") {
    DEBUG_SERIAL.println("[ARM] stop: keep current pan/tilt");
    sendPanTilt();
  } else if (normalizedAction == "aim_person") {
    panDeg = 0;
    tiltDeg = -10;
    sendPanTilt();
  } else if (normalizedAction == "aim_cloth") {
    panDeg = 0;
    tiltDeg = 20;
    sendPanTilt();
  } else {
    DEBUG_SERIAL.println("[ARM] unsupported lamp action: " + normalizedAction);
    return;
  }

  DEBUG_SERIAL.printf(
    "[ARM] action=%s speed=%s pan=%d tilt=%d slider=%d angleStep=%d sliderStep=%d\n",
    normalizedAction.c_str(),
    normalizedSpeed.c_str(),
    panDeg,
    tiltDeg,
    sliderMm,
    angleStep,
    sliderStep
  );
}

// ===================== WebSocket =====================
void sendWsRegister() {
  StaticJsonDocument<320> doc;
  doc["type"] = "register";
  doc["id"] = deviceId;
  doc["chipId"] = deviceId;
  doc["deviceType"] = FW_DEVICE_TYPE;
  doc["fwVersion"] = FW_VERSION;
  doc["fwVersionCode"] = FW_VERSION_CODE;
  doc["firmwareChannel"] = FW_CHANNEL;
  doc["otaStatus"] = otaStatus;
  doc["otaProgress"] = otaProgress;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();

  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  DEBUG_SERIAL.println("[WS] register: " + msg);
}

void handleWsMessage(const String& text) {
  DEBUG_SERIAL.println("[WS] 收到消息: " + text);

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, text);
  if (err) {
    DEBUG_SERIAL.println("[WS] JSON解析失败");
    return;
  }

  JsonObject root = doc.as<JsonObject>();
  JsonObject payload = root;

  if (root["payload"].is<JsonObject>()) {
    payload = root["payload"].as<JsonObject>();
  } else if (root["data"].is<JsonObject>()) {
    payload = root["data"].as<JsonObject>();
  }

  String type = root["type"] | payload["type"] | "";

  if (type == "state" || type == "control") {
    String targetId = payload["id"] | "";
    String chipId = payload["chipId"] | "";

    if (targetId != deviceId && chipId != deviceId) {
      DEBUG_SERIAL.println("[WS] state 不是发给本设备，忽略");
      return;
    }

    stopEffectWaveForManualControl();

    brightness = payload["brightness"] | brightness;
    temp = payload["temp"] | temp;
    recommendedBrightness = payload["recommendedBrightness"] | recommendedBrightness;
    recommendedTemp = payload["recommendedTemp"] | recommendedTemp;

    if (payload.containsKey("autoMode")) {
      autoMode = payload["autoMode"];
    } else {
      autoMode = payload["auto"] | autoMode;
    }

    safeCopyFabric(payload["fabric"]);
    sendDeviceStateReport();

    DEBUG_SERIAL.printf("WS控制：亮度=%d 色温=%d 自动=%d 推荐亮度=%d 推荐色温=%d 面料=%s\n",
                  brightness, temp, autoMode,
                  recommendedBrightness, recommendedTemp, fabric);
    return;
  }

  if (type == "effect") {
    String effect = payload["effect"] | "";
    bool enabled = payload["enabled"] | false;

    if (effect != "wave") {
      DEBUG_SERIAL.println("[EFFECT] unsupported effect: " + effect);
      return;
    }

    if (enabled) {
      effectRestoreBrightness = autoMode ? recommendedBrightness : brightness;
      effectRestoreTemp = autoMode ? recommendedTemp : temp;
      effectBaseTemp = constrain(payload["baseTemp"] | effectBaseTemp, 2700, 6500);
      int nextAmplitude = payload["range"] | effectRange;
      if (payload.containsKey("amplitude")) {
        nextAmplitude = payload["amplitude"] | nextAmplitude;
      }
      effectRange = constrain(nextAmplitude, 0, 1900);
      effectSpeed = constrain(payload["speed"] | effectSpeed, 0.2f, 5.0f);
      effectBrightness = constrain(payload["brightness"] | effectBrightness, 0, 100);
      effectPhaseOffset = payload["phaseOffset"] | effectPhaseOffset;
      effectStartMs = millis();
      lastEffectUpdateMs = 0;
      effectWaveEnabled = true;
      autoMode = false;

      int initialTemp = effectBaseTemp + (int)round(sin(effectPhaseOffset) * effectRange);
      if (payload.containsKey("initialTemp")) {
        initialTemp = payload["initialTemp"] | initialTemp;
      }
      initialTemp = constrain(initialTemp, 2700, 6500);
      brightness = effectBrightness;
      temp = initialTemp;
      recommendedBrightness = effectBrightness;
      recommendedTemp = initialTemp;
      applyLightSettings(brightness, temp);
      lastLightUpdate = millis();
      sendDeviceStateReport();

      DEBUG_SERIAL.printf(
        "[EFFECT] wave start baseTemp=%d range=%d speed=%.2f brightness=%d phaseOffset=%.2f restoreB=%d restoreT=%d\n",
        effectBaseTemp,
        effectRange,
        effectSpeed,
        effectBrightness,
        effectPhaseOffset,
        effectRestoreBrightness,
        effectRestoreTemp
      );
    } else {
      effectWaveEnabled = false;
      applyLightSettings(brightness, temp);
      lastLightUpdate = millis();
      sendDeviceStateReport();
      DEBUG_SERIAL.printf("[EFFECT] wave stop restoreB=%d restoreT=%d\n", effectRestoreBrightness, effectRestoreTemp);
    }

    return;
  }

  if (type == "locate") {
    int times = root["times"] | 3;

    int cycleMs = root["duration"] | 1200;
    if (payload.containsKey("duration")) {
      cycleMs = payload["duration"] | cycleMs;
    } else if (payload.containsKey("interval")) {
      cycleMs = payload["interval"] | cycleMs;
    }

    DEBUG_SERIAL.printf("[LOCATE] 收到呼吸定位指令 times=%d cycleMs=%d\n", times, cycleMs);

    locateBreath(times, cycleMs);
    return;
  }

  if (type == "arm") {
    String action = payload["action"] | "";
    String speed = payload["speed"] | "normal";

    if (action.length() == 0) {
      action = payload["direction"] | "";
    }

    if (action.length() == 0) {
      DEBUG_SERIAL.println("[ARM] missing action");
      return;
    }

    handleArmAction(action, speed);
    return;
  }

  if (type == "command") {
    String cmd = root["cmd"] | payload["cmd"] | "";

    if (cmd == "resume_broadcast" || cmd == "resumeBroadcast") {
      enableBroadcast = true;
      enableAnnounce = true;

      lastBroadcast = 0;
      lastAnnounce = 0;

      DEBUG_SERIAL.println("[WS] resume broadcast command received");
      DEBUG_SERIAL.println("[WS] UDP broadcast resumed");
    } else {
      DEBUG_SERIAL.println("[WS] unknown command: " + cmd);
    }
    return;
  }

  if (type == "ota_update" || type == "ota:update") {
    String version = payload["version"] | "";
    String url = payload["url"] | "";
    int versionCode = payload["versionCode"] | 0;
    String channel = payload["channel"] | "";
    String md5 = payload["md5"] | "";

    if (channel.length() == 0) {
      channel = FW_CHANNEL;
    }
    channel.toLowerCase();

    if (version.length() == 0 || url.length() == 0 || versionCode <= 0) {
      DEBUG_SERIAL.println("[OTA] OTA message missing version/url/versionCode");
      otaStatus = "failed";
      sendDeviceStateReport();
      return;
    }

    bool sameChannel = (channel == String(FW_CHANNEL));

    if (sameChannel && versionCode <= FW_VERSION_CODE) {
      DEBUG_SERIAL.println("[OTA] Same channel and target versionCode is not newer, ignore");
      otaStatus = "idle";
      otaProgress = 0;
      sendDeviceStateReport();
      return;
    }

    if (!sameChannel) {
      DEBUG_SERIAL.println("[OTA] Cross-channel OTA allowed");
    }

    doOtaUpdate(url, version, versionCode, channel, md5);
    return;
  }


  DEBUG_SERIAL.println("[WS] 未处理消息类型: " + type);
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      DEBUG_SERIAL.println("[WS] 已断开");
      break;

    case WStype_CONNECTED:
      DEBUG_SERIAL.printf("[WS] 已连接: %s\n", payload);
      sendWsRegister();
      break;

    case WStype_TEXT:
      handleWsMessage(String((char*)payload));
      break;

    case WStype_PONG:
      DEBUG_SERIAL.println("[WS] PONG");
      break;

    default:
      break;
  }
}

void beginWebSocketClient() {
  webSocket.disconnect();
  delay(100);

  DEBUG_SERIAL.println("[WS] 准备连接:");
  DEBUG_SERIAL.println("host = " + cfg.serverHost);
  DEBUG_SERIAL.println("port = " + String(cfg.wsPort));
  DEBUG_SERIAL.println("path = " + String(WS_PATH));
  DEBUG_SERIAL.println("url  = ws://" + cfg.serverHost + ":" + String(cfg.wsPort) + String(WS_PATH));

  webSocket.begin(cfg.serverHost.c_str(), cfg.wsPort, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);
}

// ===================== 灯控逻辑 =====================
void applyLightSettings(int br, int tp) {
  tp = constrain(tp, 2700, 6500);

  int tempVal = map(tp, 2700, 6500, 0, 1024);
  int briVal  = map(br, 0, 100, 0, 1024);

  int pwmCold = (long)tempVal * briVal / 1024;
  int pwmWarm = (long)(1024 - tempVal) * briVal / 1024;

  analogWrite(LED_COLD_PIN, 1024 - pwmCold);
  analogWrite(LED_WARM_PIN, 1024 - pwmWarm);
}

void stopEffectWaveForManualControl() {
  if (!effectWaveEnabled) return;
  effectWaveEnabled = false;
  DEBUG_SERIAL.println("[EFFECT] wave stopped by manual control");
}

void locateBreath(int times, int cycleMs) {
  times = constrain(times, 1, 8);
  cycleMs = constrain(cycleMs, 800, 3000);

  int oldBrightness = autoMode ? recommendedBrightness : brightness;
  int oldTemp = autoMode ? recommendedTemp : temp;

  int locateTemp = 4500;
  int stepDelay = cycleMs / LOCATE_STEPS;

  DEBUG_SERIAL.printf(
    "[LOCATE] 呼吸灯开始 times=%d cycleMs=%d restoreB=%d restoreT=%d\n",
    times,
    cycleMs,
    oldBrightness,
    oldTemp
  );

  for (int round = 0; round < times; round++) {
    for (int i = 0; i <= LOCATE_STEPS; i++) {
      float phase = (float)i / (float)LOCATE_STEPS * PI;

      int br = LOCATE_MIN_BRIGHTNESS + int(sin(phase) * (LOCATE_MAX_BRIGHTNESS - LOCATE_MIN_BRIGHTNESS));

      applyLightSettings(br, locateTemp);

      delay(stepDelay);
      yield();

      webSocket.loop();
      server.handleClient();
    }
  }

  applyLightSettings(oldBrightness, oldTemp);

  DEBUG_SERIAL.println("[LOCATE] 呼吸定位结束，已恢复原灯光");
}

void updateEffectLoop() {
  if (!effectWaveEnabled) return;

  unsigned long now = millis();
  if (now - lastEffectUpdateMs < WAVE_UPDATE_INTERVAL_MS) return;
  lastEffectUpdateMs = now;

  float elapsedSec = (now - effectStartMs) / 1000.0f;
  float phase = elapsedSec * WAVE_FREQ_FACTOR * effectSpeed + effectPhaseOffset;
  int targetTemp = (int)round(effectBaseTemp + sin(phase) * effectRange);
  targetTemp = constrain(targetTemp, 2700, 6500);
  int targetBrightness = constrain(effectBrightness, 0, 100);

  brightness = targetBrightness;
  temp = targetTemp;
  recommendedBrightness = targetBrightness;
  recommendedTemp = targetTemp;
  applyLightSettings(targetBrightness, targetTemp);
}

void sendStayRecordToServer(unsigned long durationSeconds) {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<192> doc;
  doc["chipId"] = deviceId;
  doc["durationValue"] = durationSeconds * 1000UL;

  String payload;
  serializeJson(doc, payload);
  postJsonToServer("/admin/duration/create", payload);
}

void updateLightingByToF() {
  if (!tofReady) {
    int br = autoMode ? recommendedBrightness : brightness;
    int tp = autoMode ? recommendedTemp : temp;
    unsigned long now = millis();

    if (autoMode && strcmp(fabric, "polyester") == 0) {
      digitalWrite(BLUR, LOW);
    } else {
      digitalWrite(BLUR, HIGH);
    }

    if (now - lastLightUpdate > lightUpdateInterval) {
      applyLightSettings(br, tp);
      lastLightUpdate = now;
    }
    return;
  }

  unsigned long now = millis();
  VL53L0X_RangingMeasurementData_t measure;

  if (now - lastToFRead >= TOF_READ_INTERVAL_MS) {
    lox.rangingTest(&measure, false);
    lastToFRead = now;
  } else {
    return;
  }

  if (measure.RangeMilliMeter > TOF_MAX_RANGE_MM) return;

  static bool wasNearby = false;
  static unsigned long transitionStart = 0;
  static unsigned long detectedStart = 0;
  static unsigned long leftStart = 0;

  bool currentNearby = (measure.RangeMilliMeter < 2000);

  DEBUG_SERIAL.printf("测距: %d mm\n", measure.RangeMilliMeter);

  if (autoMode) {
    if (currentNearby && !wasNearby) {
      if (detectedStart == 0) {
        detectedStart = now;
      } else if (now - detectedStart >= TOF_DEBOUNCE_MS) {
        transitionStart = now;
        wasNearby = true;
        leftStart = 0;
      }
    } else if (!currentNearby && wasNearby) {
      if (leftStart == 0) {
        leftStart = now;
      } else if (now - leftStart >= TOF_DEBOUNCE_MS) {
        transitionStart = now;
        wasNearby = false;
        unsigned long stayDurationSeconds = (now - detectedStart) / 1000;
        sendStayRecordToServer(stayDurationSeconds);
        detectedStart = 0;
      }
    } else {
      if (currentNearby) leftStart = 0;
      else detectedStart = 0;
    }
  }

  float ratio = float(now - transitionStart) / TOF_TRANSITION_MS;
  if (ratio > 1.0f) ratio = 1.0f;

  int br = brightness;
  int tp = temp;

  if (autoMode) {
    if (wasNearby) {
      br = brightness + int((recommendedBrightness - brightness) * ratio);
      tp = temp + int((recommendedTemp - temp) * ratio);

      if (strcmp(fabric, "polyester") == 0) {
        digitalWrite(BLUR, LOW);
      } else {
        digitalWrite(BLUR, HIGH);
      }
    } else {
      br = recommendedBrightness + int((brightness - recommendedBrightness) * ratio);
      tp = recommendedTemp + int((temp - recommendedTemp) * ratio);
      digitalWrite(BLUR, HIGH);
    }
  }

  if (now - lastLightUpdate > lightUpdateInterval) {
    applyLightSettings(br, tp);
    lastLightUpdate = now;
  }
}

// ===================== 设备上报 =====================
void broadcastDevice() {
  if (!enableBroadcast || WiFi.status() != WL_CONNECTED) return;

  if (millis() - lastBroadcast > broadcastInterval) {
    lastBroadcast = millis();

    if (!broadcastIPCached) refreshBroadcastIP();

    String msg = "{\"type\":\"announce\",\"device\":\"" + String(FW_DEVICE_TYPE) + "\",\"id\":\"" + deviceId +
                 "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";

    udp.beginPacket(cachedBroadcastIP, udpPort);
    udp.write((const uint8_t*)msg.c_str(), msg.length());
    udp.endPacket();

    DEBUG_SERIAL.println("广播: " + msg);
  }
}

void sendDeviceStateReport() {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<448> doc;
  doc["chipId"] = deviceId;
  doc["deviceType"] = FW_DEVICE_TYPE;
  doc["ip"] = WiFi.localIP().toString();
  doc["brightness"] = brightness;
  doc["temp"] = temp;
  doc["autoMode"] = autoMode;
  doc["recommendedBrightness"] = recommendedBrightness;
  doc["recommendedTemp"] = recommendedTemp;
  doc["fabric"] = fabric;
  doc["firmwareVersion"] = FW_VERSION;
  doc["firmwareVersionCode"] = FW_VERSION_CODE;
  doc["firmwareChannel"] = FW_CHANNEL;
  doc["otaStatus"] = otaStatus;
  doc["otaProgress"] = otaProgress;

  String json;
  serializeJson(doc, json);
  postJsonToServer("/admin/device/state-report", json);
}

void sendAnnounce() {
  if (!enableAnnounce || WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<256> doc;
  doc["chipId"] = deviceId;
  doc["ip"] = WiFi.localIP().toString();
  doc["deviceType"] = FW_DEVICE_TYPE;

  String json;
  serializeJson(doc, json);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, httpUrl("/admin/device/announce"));
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(json);
  if (httpCode > 0) {
    String payload = http.getString();
    DEBUG_SERIAL.println("服务器回应: " + payload);

    if (payload.indexOf("\"added\":true") >= 0) {
      enableAnnounce = false;
      enableBroadcast = false;
      DEBUG_SERIAL.println("成功上报且已添加，停止上报和广播");
    }
  } else {
    DEBUG_SERIAL.printf("上报失败: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

void sendLightLevelToServer() {
  if (!bh1750Ready || WiFi.status() != WL_CONNECTED) return;

  float lux = lightMeter.readLightLevel();
  DEBUG_SERIAL.printf("当前光照值：%.2f lux\n", lux);

  StaticJsonDocument<192> doc;
  doc["chipId"] = deviceId;
  doc["luxValue"] = lux;

  String json;
  serializeJson(doc, json);
  postJsonToServer("/admin/lux/create", json);
}

// ===================== 初始化 =====================
void setupHardwareAndSensors() {
  pinMode(LED_COLD_PIN, OUTPUT);
  pinMode(LED_WARM_PIN, OUTPUT);
  pinMode(BLUR, OUTPUT);
  digitalWrite(BLUR, HIGH);
  analogWriteRange(1024);

  Wire.begin(TOF_SDA_PIN, TOF_SCL_PIN);
  Wire.setClock(400000);

  if (!lox.begin()) {
    DEBUG_SERIAL.println("VL53L0X 初始化失败");
  } else {
    DEBUG_SERIAL.println("VL53L0X 初始化成功");
    tofReady = true;
  }

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    DEBUG_SERIAL.println("BH1750 初始化成功");
    bh1750Ready = true;
  } else {
    DEBUG_SERIAL.println("BH1750 初始化失败");
  }

  udp.begin(udpPort);
}

bool ensureWiFiReady() {
  if (WiFi.status() == WL_CONNECTED) return true;

  broadcastIPCached = false;

  DEBUG_SERIAL.println("[WiFi] 已断开，尝试重连已保存网络...");
  if (connectSavedWiFi()) {
    broadcastIPCached = false;
    return true;
  }

  DEBUG_SERIAL.println("[WiFi] 已保存网络重连失败，尝试 SmartConfig...");
  if (smartConfigProvision(smartConfigTimeout)) {
    broadcastIPCached = false;
    return true;
  }

  DEBUG_SERIAL.println("[WiFi] 进入 AP 配网模式");
  startConfigPortal();
  return false;
}

// ===================== setup / loop =====================
void setup() {
  Serial.begin(NANO_BAUD);
  DEBUG_SERIAL.begin(115200);
  delay(200);

  deviceId = makeDeviceId();

  DEBUG_SERIAL.println("\n========================");
  DEBUG_SERIAL.println("设备启动");
  DEBUG_SERIAL.println("ID = " + deviceId);
  DEBUG_SERIAL.println("FW = " + String(FW_VERSION));
  DEBUG_SERIAL.println("========================");

  if (!LittleFS.begin()) {
    DEBUG_SERIAL.println("[FS] LittleFS 挂载失败");
  }

  setupHardwareAndSensors();

  bool hasConfig = loadConfig();
  bool wifiOk = false;

  if (hasConfig) {
    DEBUG_SERIAL.println("[BOOT] 检测到本地配置，尝试直连");
    wifiOk = connectSavedWiFi();
  } else {
    DEBUG_SERIAL.println("[BOOT] 没有本地配置");
  }

  if (!wifiOk) {
    wifiOk = smartConfigProvision(smartConfigTimeout);
  }

  if (!wifiOk) {
    startConfigPortal();
    return;
  }

  broadcastIPCached = false;
  setupDeviceHttpServer();
  beginWebSocketClient();
  sendAnnounce();
  sendDeviceStateReport();
}

void loop() {
  server.handleClient();
  pollNano();

  if (portalMode) {
    return;
  }

  if (otaInProgress) {
    return;
  }

  if (!ensureWiFiReady()) {
    return;
  }

  webSocket.loop();
  broadcastDevice();
  if (effectWaveEnabled) {
    updateEffectLoop();
  } else {
    updateLightingByToF();
  }

  unsigned long now = millis();

  if (now - lastPing > wsPingInterval) {
  lastPing = now;

  StaticJsonDocument<96> doc;
  doc["type"] = "ping";
  doc["id"] = deviceId;
  doc["chipId"] = deviceId;

  String pingMsg;
  serializeJson(doc, pingMsg);
  webSocket.sendTXT(pingMsg);

  DEBUG_SERIAL.println("发送 WebSocket 心跳: " + pingMsg);
}

  if (now - lastAnnounce > announceInterval) {
    lastAnnounce = now;
    sendAnnounce();
  }

  if (now - lastLightSend > lightSendInterval) {
    lastLightSend = now;
    sendLightLevelToServer();
  }
}
