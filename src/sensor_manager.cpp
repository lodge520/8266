#include "sensor_manager.h"
#include "light_control.h"
#include "http_reporter.h"

void setupHardwareAndSensors() {
  pinMode(LED_COLD_PIN, OUTPUT);
  pinMode(LED_WARM_PIN, OUTPUT);
  pinMode(BLUR, OUTPUT);
  digitalWrite(BLUR, HIGH);
  analogWriteRange(1024);

  Wire.begin(TOF_SDA_PIN, TOF_SCL_PIN);
  Wire.setClock(400000);

  if (!lox.begin()) {
    DEBUG_SERIAL.println("VL53L0X 初始化失败");
  } else {
    DEBUG_SERIAL.println("VL53L0X 初始化成功");
    tofReady = true;
  }

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    DEBUG_SERIAL.println("BH1750 初始化成功");
    bh1750Ready = true;
  } else {
    DEBUG_SERIAL.println("BH1750 初始化失败");
  }

  udp.begin(udpPort);
}

void updateLightingByToF() {
  if (!tofReady) {
    int br = autoMode ? recommendedBrightness : brightness;
    int tp = autoMode ? recommendedTemp : temp;
    unsigned long now = millis();

    if (autoMode && strcmp(fabric, "polyester") == 0) {
      digitalWrite(BLUR, LOW);
    } else {
      digitalWrite(BLUR, HIGH);
    }

    if (now - lastLightUpdate > lightUpdateInterval) {
      applyLightSettings(br, tp);
      lastLightUpdate = now;
    }
    return;
  }

  unsigned long now = millis();
  VL53L0X_RangingMeasurementData_t measure;

  if (now - lastToFRead >= TOF_READ_INTERVAL_MS) {
    lox.rangingTest(&measure, false);
    lastToFRead = now;
  } else {
    return;
  }

  if (measure.RangeMilliMeter > TOF_MAX_RANGE_MM) return;

  static bool wasNearby = false;
  static unsigned long transitionStart = 0;
  static unsigned long detectedStart = 0;
  static unsigned long leftStart = 0;

  bool currentNearby = (measure.RangeMilliMeter < 2000);

  DEBUG_SERIAL.printf("测距: %d mm\n", measure.RangeMilliMeter);

  if (autoMode) {
    if (currentNearby && !wasNearby) {
      if (detectedStart == 0) {
        detectedStart = now;
      } else if (now - detectedStart >= TOF_DEBOUNCE_MS) {
        transitionStart = now;
        wasNearby = true;
        leftStart = 0;
      }
    } else if (!currentNearby && wasNearby) {
      if (leftStart == 0) {
        leftStart = now;
      } else if (now - leftStart >= TOF_DEBOUNCE_MS) {
        transitionStart = now;
        wasNearby = false;
        unsigned long stayDurationSeconds = (now - detectedStart) / 1000;
        sendStayRecordToServer(stayDurationSeconds);
        detectedStart = 0;
      }
    } else {
      if (currentNearby) leftStart = 0;
      else detectedStart = 0;
    }
  }

  float ratio = float(now - transitionStart) / TOF_TRANSITION_MS;
  if (ratio > 1.0f) ratio = 1.0f;

  int br = brightness;
  int tp = temp;

  if (autoMode) {
    if (wasNearby) {
      br = brightness + int((recommendedBrightness - brightness) * ratio);
      tp = temp + int((recommendedTemp - temp) * ratio);

      if (strcmp(fabric, "polyester") == 0) {
        digitalWrite(BLUR, LOW);
      } else {
        digitalWrite(BLUR, HIGH);
      }
    } else {
      br = recommendedBrightness + int((brightness - recommendedBrightness) * ratio);
      tp = recommendedTemp + int((temp - recommendedTemp) * ratio);
      digitalWrite(BLUR, HIGH);
    }
  }

  if (now - lastLightUpdate > lightUpdateInterval) {
    applyLightSettings(br, tp);
    lastLightUpdate = now;
  }
}
