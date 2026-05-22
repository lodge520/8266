#include "udp_discovery.h"

IPAddress calcBroadcastIP() {
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; i++) {
    broadcast[i] = ip[i] | (~mask[i]);
  }
  return broadcast;
}

void refreshBroadcastIP() {
  cachedBroadcastIP = calcBroadcastIP();
  broadcastIPCached = true;
}

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
