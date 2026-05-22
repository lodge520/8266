#include "config_manager.h"

String configPath() {
  return "/config.json";
}

String makeDeviceId() {
  String id = "lamp-";
  id += String(ESP.getChipId(), HEX);
  id.toUpperCase();
  return id;
}

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

void ensureConfigDefaults(DeviceConfig& c) {
  if (c.serverHost.length() == 0) c.serverHost = DEFAULT_SERVER_HOST;
  if (c.httpPort == 0) c.httpPort = DEFAULT_HTTP_PORT;
  if (c.wsPort == 0) c.wsPort = DEFAULT_WS_PORT;
}
