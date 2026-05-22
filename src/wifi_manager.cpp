#include "wifi_manager.h"
#include "config_manager.h"

// ---- HTML 配网页面 ----

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

// ---- WiFi 连接 ----

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

// ---- SmartConfig ----

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

    // 停止 SmartConfig，否则后面切 AP 可能失败
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

// ---- AP Portal 配网 ----

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

// ---- WiFi 保活 ----

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
