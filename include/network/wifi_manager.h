#pragma once
#include "app_config.h"

bool connectWiFi(const String& ssid, const String& password, unsigned long timeoutMs);
bool connectSavedWiFi();
bool smartConfigProvision(unsigned long timeoutMs);
void startConfigPortal();
bool ensureWiFiReady();
String getPortalHtml();
