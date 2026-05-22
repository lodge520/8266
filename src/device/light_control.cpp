#include "device/light_control.h"

void applyLightSettings(int br, int tp) {
  tp = constrain(tp, 2700, 6500);

  int tempVal = map(tp, 2700, 6500, 0, 1024);
  int briVal  = map(br, 0, 100, 0, 1024);

  int pwmCold = (long)tempVal * briVal / 1024;
  int pwmWarm = (long)(1024 - tempVal) * briVal / 1024;

  analogWrite(LED_COLD_PIN, 1024 - pwmCold);
  analogWrite(LED_WARM_PIN, 1024 - pwmWarm);
}

void stopEffectWaveForManualControl() {
  if (!effectWaveEnabled) return;
  effectWaveEnabled = false;
  DEBUG_SERIAL.println("[EFFECT] wave stopped by manual control");
}

void locateBreath(int times, int cycleMs) {
  times = constrain(times, 1, 8);
  cycleMs = constrain(cycleMs, 800, 3000);

  int oldBrightness = autoMode ? recommendedBrightness : brightness;
  int oldTemp = autoMode ? recommendedTemp : temp;

  int locateTemp = 4500;
  int stepDelay = cycleMs / LOCATE_STEPS;

  DEBUG_SERIAL.printf(
    "[LOCATE] 呼吸灯开始 times=%d cycleMs=%d restoreB=%d restoreT=%d\n",
    times, cycleMs, oldBrightness, oldTemp
  );

  for (int round = 0; round < times; round++) {
    for (int i = 0; i <= LOCATE_STEPS; i++) {
      float phase = (float)i / (float)LOCATE_STEPS * PI;

      int br = LOCATE_MIN_BRIGHTNESS + int(sin(phase) * (LOCATE_MAX_BRIGHTNESS - LOCATE_MIN_BRIGHTNESS));

      applyLightSettings(br, locateTemp);

      delay(stepDelay);
      yield();

      webSocket.loop();
      server.handleClient();
    }
  }

  applyLightSettings(oldBrightness, oldTemp);

  DEBUG_SERIAL.println("[LOCATE] 呼吸定位结束，已恢复原灯光");
}

void updateEffectLoop() {
  if (!effectWaveEnabled) return;

  unsigned long now = millis();
  if (now - lastEffectUpdateMs < WAVE_UPDATE_INTERVAL_MS) return;
  lastEffectUpdateMs = now;

  float elapsedSec = (now - effectStartMs) / 1000.0f;
  float phase = elapsedSec * WAVE_FREQ_FACTOR * effectSpeed + effectPhaseOffset;
  int targetTemp = (int)round(effectBaseTemp + sin(phase) * effectRange);
  targetTemp = constrain(targetTemp, 2700, 6500);
  int targetBrightness = constrain(effectBrightness, 0, 100);

  brightness = targetBrightness;
  temp = targetTemp;
  recommendedBrightness = targetBrightness;
  recommendedTemp = targetTemp;
  applyLightSettings(targetBrightness, targetTemp);
}

void safeCopyFabric(const char* src) {
  if (!src || strlen(src) == 0) return;
  snprintf(fabric, sizeof(fabric), "%s", src);
}
