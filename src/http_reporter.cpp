#include "http_reporter.h"

String httpUrl(const String& path) {
  return "http://" + cfg.serverHost + ":" + String(cfg.httpPort) + path;
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

void sendStayRecordToServer(unsigned long durationSeconds) {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<192> doc;
  doc["chipId"] = deviceId;
  doc["durationValue"] = durationSeconds * 1000UL;

  String payload;
  serializeJson(doc, payload);
  postJsonToServer("/admin/duration/create", payload);
}
