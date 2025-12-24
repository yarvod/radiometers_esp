#pragma once

#include <string>

#include "app_state.h"

struct LogRequest {
  std::string filename;
  bool use_motor = false;
  float duration_s = 1.0f;
};

struct StepperMoveRequest {
  int steps = 0;
  bool forward = true;
  int speed_us = 0;
};

struct ActionResult {
  bool ok = false;
  std::string message;
  std::string json;  // optional payload
};

// Shared business actions for HTTP/MQTT bridges
ActionResult ActionStartLog(const LogRequest& req);
ActionResult ActionStopLog();
ActionResult ActionStepperEnable();
ActionResult ActionStepperDisable();
ActionResult ActionStepperMove(const StepperMoveRequest& req);
ActionResult ActionHeaterSet(float power_percent);
ActionResult ActionFanSet(float power_percent);
ActionResult ActionGetState();

