#pragma once
#include "app_config.h"

void sendWsRegister();
void handleWsMessage(const String& text);
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
void beginWebSocketClient();
