#include "network/wifi_manager.h"
#include "config/config_manager.h"

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
    <p class="tip">设备优先通过 SmartConfig 配网；若连接失败则自动切换到此 AP 配网页面。</p>

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

// ---- 串行配网 (SmartConfig 优先, AP 后备) ----

static void ensureProvisionRoutes() {
  static bool registered = false;
  if (registered) return;
  registered = true;

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

    DEBUG_SERIAL.println("[PROV] AP config saved, restarting...");

    if (smartConfigActive) {
      WiFi.stopSmartConfig();
      smartConfigActive = false;
    }

    server.send(200, "text/html; charset=utf-8", "<h3>保存成功，设备即将重启...</h3>");
    delay(1200);
    ESP.restart();
  });

  server.on("/resetWifi", HTTP_POST, []() {
    clearConfig();
    DEBUG_SERIAL.println("[PROV] Config cleared via AP, restarting...");
    server.send(200, "text/html; charset=utf-8", "<h3>已清除配置，设备即将重启...</h3>");
    delay(1200);
    ESP.restart();
  });
}

void startAPPortal() {
  // 先停掉正在运行的 SmartConfig
  if (smartConfigActive) {
    WiFi.stopSmartConfig();
    smartConfigActive = false;
  }

  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_AP);
  delay(300);

  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);

  String apName = "LightConfig_" + deviceId;
  bool apOk = WiFi.softAP(apName.c_str(), AP_DEFAULT_PASSWORD, 1, false, 4);

  DEBUG_SERIAL.println("[PROV] ====== AP portal started ======");
  DEBUG_SERIAL.println("[PROV] AP SSID: " + apName);
  DEBUG_SERIAL.println("[PROV] AP password: " + String(AP_DEFAULT_PASSWORD));

  if (apOk) {
    DEBUG_SERIAL.println("[PROV] AP portal ready -> http://" + WiFi.softAPIP().toString());
  } else {
    DEBUG_SERIAL.println("[PROV] AP start failed!");
  }

  ensureProvisionRoutes();
  server.begin();
}

void startParallelProvision() {
  provisioningMode = true;

  // SmartConfig 优先：纯 STA 模式（ESP8266 只能 STA 模式启动 SmartConfig）
  WiFi.mode(WIFI_STA);
  delay(300);

  bool scOk = WiFi.beginSmartConfig();
  smartConfigActive = scOk;
  smartConfigDoneHandled = false;
  smartConfigStartMs = millis();

  if (scOk) {
    DEBUG_SERIAL.println("[PROV] SmartConfig listening (AirKiss, " + String(smartConfigTimeout / 1000) + "s timeout)");
    ensureProvisionRoutes();
  } else {
    DEBUG_SERIAL.println("[PROV] SmartConfig failed, falling back to AP portal");
    startAPPortal();
  }
}

void handleProvisioningLoop() {
  if (smartConfigActive) {
    if (!smartConfigDoneHandled) {
      if (WiFi.smartConfigDone()) {
        smartConfigDoneHandled = true;
        DEBUG_SERIAL.println("[PROV] SmartConfig received credentials, connecting...");

        unsigned long connectStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 20000) {
          delay(100);
          yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
          DeviceConfig newCfg;
          newCfg.ssid = WiFi.SSID();
          newCfg.password = WiFi.psk();
          newCfg.serverHost = cfg.serverHost;
          newCfg.httpPort = cfg.httpPort;
          newCfg.wsPort = cfg.wsPort;
          ensureConfigDefaults(newCfg);
          cfg = newCfg;
          saveConfig(cfg);

          DEBUG_SERIAL.println("[PROV] SmartConfig config saved, restarting...");
          WiFi.stopSmartConfig();
          smartConfigActive = false;
          delay(500);
          ESP.restart();
        } else {
          DEBUG_SERIAL.println("[PROV] SmartConfig connect failed, falling back to AP portal");
          startAPPortal();
        }
        return;
      }

      static unsigned long lastScLog = 0;
      if (millis() - lastScLog > 15000) {
        lastScLog = millis();
        DEBUG_SERIAL.println("[PROV] SmartConfig still waiting...");
      }

      if (millis() - smartConfigStartMs > smartConfigTimeout) {
        DEBUG_SERIAL.println("[PROV] SmartConfig timeout, falling back to AP portal");
        startAPPortal();
      }
    }
  }
}

// ---- WiFi 保活 ----

bool ensureWiFiReady() {
  if (WiFi.status() == WL_CONNECTED) return true;

  broadcastIPCached = false;

  DEBUG_SERIAL.println("[WiFi] Disconnected, trying saved network...");
  if (connectSavedWiFi()) {
    broadcastIPCached = false;
    return true;
  }

  DEBUG_SERIAL.println("[WiFi] Saved network failed, starting parallel provisioning...");
  startParallelProvision();
  return false;
}
