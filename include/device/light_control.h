#pragma once
#include "app_config.h"

void applyLightSettings(int br, int tp);
void stopEffectWaveForManualControl();
void locateBreath(int times, int cycleMs);
void updateEffectLoop();
void safeCopyFabric(const char* src);
