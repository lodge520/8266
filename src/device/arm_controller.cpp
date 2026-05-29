#include "device/arm_controller.h"

// 当前速度档位
String currentArmSpeed = "normal";

// 摇杆连续运动状态
bool armJoystickActive = false;
float joystickX = 0.0f;
float joystickY = 0.0f;
float panVelocityDegPerSec = 0.0f;
float tiltVelocityDegPerSec = 0.0f;
unsigned long joystickExpireAt = 0;
unsigned long lastArmMotionUpdateAt = 0;
unsigned long lastNanoPositionSendAt = 0;

void sendNano(char cmd, const String& value) {
  nanoSerial.print(cmd);
  if (value.length() > 0) {
    nanoSerial.print(value);
  }
  nanoSerial.print('\n');

  delay(25);
  pollNano();

  DEBUG_SERIAL.print("[NANO] TX ");
  DEBUG_SERIAL.print(cmd);
  DEBUG_SERIAL.println(value);
}

void pollNano() {
  static String line;

  while (nanoSerial.available() > 0) {
    char ch = (char)nanoSerial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (line.length() > 0) {
        DEBUG_SERIAL.println("[NANO] RX " + line);
        line = "";
      }
      continue;
    }

    if (line.length() < 120) {
      line += ch;
    } else {
      line = "";
      DEBUG_SERIAL.println("[NANO] RX line too long, dropped");
    }
  }
}

void sendPanTilt() {
  panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
  tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
  sendNano('p', String(panDeg, 2));
  sendNano('t', String(tiltDeg, 2));
}

void sendSlider() {
  sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
  sendNano('x', String(sliderMm, 2));
}

void applyArmSpeed(const String& speed) {
  String normalized = speed;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "slow") {
    angleStep = 2;
    sliderStep = 5;
    panSpeedDeg = 4;
    tiltSpeedDeg = 3;
    sliderSpeedMm = 5;
  } else if (normalized == "fast") {
    angleStep = 5;
    sliderStep = 20;
    panSpeedDeg = 15;
    tiltSpeedDeg = 10;
    sliderSpeedMm = 30;
  } else {
    // normal
    angleStep = 5;
    sliderStep = 10;
    panSpeedDeg = 8;
    tiltSpeedDeg = 5;
    sliderSpeedMm = 10;
  }

  currentArmSpeed = normalized;

  sendNano('s', String(panSpeedDeg, 2));
  sendNano('S', String(tiltSpeedDeg, 2));
  sendNano('X', String(sliderSpeedMm, 2));
}

void getArmJoystickMaxSpeed(float& maxPanSpeed, float& maxTiltSpeed) {
  if (currentArmSpeed == "slow") {
    maxPanSpeed = 4.0f;
    maxTiltSpeed = 3.0f;
  } else if (currentArmSpeed == "fast") {
    maxPanSpeed = 15.0f;
    maxTiltSpeed = 10.0f;
  } else {
    // normal
    maxPanSpeed = 8.0f;
    maxTiltSpeed = 5.0f;
  }
}

void setArmJoystickMotion(float x, float y, int durationMs) {
  x = constrain(x, -1.0f, 1.0f);
  y = constrain(y, -1.0f, 1.0f);
  durationMs = constrain(durationMs, 100, 1000);

  float maxPanSpeed = 8.0f;
  float maxTiltSpeed = 5.0f;
  getArmJoystickMaxSpeed(maxPanSpeed, maxTiltSpeed);

  joystickX = x;
  joystickY = y;

  panVelocityDegPerSec = joystickX * maxPanSpeed;
  tiltVelocityDegPerSec = joystickY * maxTiltSpeed;

  armJoystickActive = true;
  joystickExpireAt = millis() + durationMs;
  lastArmMotionUpdateAt = millis();

  DEBUG_SERIAL.printf(
    "[ARM] joystick x=%.2f y=%.2f panVel=%.2f tiltVel=%.2f duration=%d\n",
    x,
    y,
    panVelocityDegPerSec,
    tiltVelocityDegPerSec,
    durationMs
  );
}

void stopArmJoystickMotion() {
  armJoystickActive = false;
  joystickX = 0.0f;
  joystickY = 0.0f;
  panVelocityDegPerSec = 0.0f;
  tiltVelocityDegPerSec = 0.0f;

  DEBUG_SERIAL.println("[ARM] joystick stopped");
}

void updateArmJoystickMotion() {
  if (!armJoystickActive) {
    return;
  }

  unsigned long now = millis();

  // 超时自动停止（前端断连/stop 丢包保护）
  if ((long)(now - joystickExpireAt) >= 0) {
    stopArmJoystickMotion();
    DEBUG_SERIAL.println("[ARM] joystick expired, auto stop");
    return;
  }

  if (lastArmMotionUpdateAt == 0) {
    lastArmMotionUpdateAt = now;
    return;
  }

  float dt = (now - lastArmMotionUpdateAt) / 1000.0f;
  lastArmMotionUpdateAt = now;

  // 防止帧间隔过大导致位置跳变
  if (dt <= 0.0f || dt > 0.2f) {
    return;
  }

  panDeg += (int)(panVelocityDegPerSec * dt);
  tiltDeg += (int)(tiltVelocityDegPerSec * dt);

  panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
  tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);

  // 每 80ms 向 Nano 发送一次当前位置
  if (now - lastNanoPositionSendAt >= 80) {
    sendNano('p', String(panDeg, 2));
    sendNano('t', String(tiltDeg, 2));
    lastNanoPositionSendAt = now;
  }
}

void handleArmAction(const String& action) {
  String normalizedAction = action;
  normalizedAction.trim();
  normalizedAction.toLowerCase();

  if (normalizedAction == "slider_position") {
    DEBUG_SERIAL.println("[ARM] slider_position ignored by lamp firmware");
    return;
  }

  if (normalizedAction == "up") {
    tiltDeg += angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', String(tiltDeg, 2));
  } else if (normalizedAction == "down") {
    tiltDeg -= angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', String(tiltDeg, 2));
  } else if (normalizedAction == "left") {
    panDeg -= angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', String(panDeg, 2));
  } else if (normalizedAction == "right") {
    panDeg += angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', String(panDeg, 2));
  } else if (normalizedAction == "center") {
    panDeg = 0;
    tiltDeg = 0;
    sendPanTilt();
  } else if (normalizedAction == "home") {
    sendNano('A');
  } else if (normalizedAction == "stop") {
    DEBUG_SERIAL.println("[ARM] stop: keep current pan/tilt");
    sendPanTilt();
  } else if (normalizedAction == "aim_person") {
    panDeg = 0;
    tiltDeg = -10;
    sendPanTilt();
  } else if (normalizedAction == "aim_cloth") {
    panDeg = 0;
    tiltDeg = 20;
    sendPanTilt();
  } else {
    DEBUG_SERIAL.println("[ARM] unsupported lamp action: " + normalizedAction);
    return;
  }

  DEBUG_SERIAL.printf(
    "[ARM] action=%s pan=%d tilt=%d slider=%d angleStep=%d sliderStep=%d\n",
    normalizedAction.c_str(),
    panDeg,
    tiltDeg,
    sliderMm,
    angleStep,
    sliderStep
  );
}
