#pragma once
#include "app_config.h"

static const int PAN_MIN = -90;
static const int PAN_MAX = 90;
static const int TILT_MIN = -45;
static const int TILT_MAX = 45;
static const int SLIDER_MIN = 0;
static const int SLIDER_MAX = 1200;

void sendNano(char cmd, const String& value = "");
void pollNano();
void sendPanTilt();
void sendSlider();
void applyArmSpeed(const String& speed);
void handleArmAction(const String& action, const String& speed);
