#pragma once
#include "app_config.h"

String httpUrl(const String& path);
int    postJsonToServer(const String& path, const String& jsonBody);
void   sendDeviceStateReport();
void   sendAnnounce();
void   sendLightLevelToServer();
void   sendStayRecordToServer(unsigned long durationSeconds);
