#pragma once

#include <string>
#include <vector>

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

struct StepperHomeOffsetRequest {
  int offset_steps = 0;
  int speed_us = 0;
  int hall_active_level = 0;
  bool hall_active_level_set = false;
};

struct PidApplyRequest {
  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;
  float setpoint = 0.0f;
  int sensor = 0;
  uint16_t sensor_mask = 0;
  bool sensor_mask_set = false;
};

struct WifiApplyRequest {
  std::string mode;
  std::string ssid;
  std::string password;
};

struct NetApplyRequest {
  std::string mode;
  std::string priority;
};

struct CloudApplyRequest {
  std::string device_id;
  std::string minio_endpoint;
  std::string minio_access_key;
  std::string minio_secret_key;
  std::string minio_bucket;
  bool minio_enabled = false;
  std::string mqtt_uri;
  std::string mqtt_user;
  std::string mqtt_password;
  bool mqtt_enabled = false;
};

struct GpsApplyRequest {
  std::vector<uint16_t> rtcm_types;
  std::string mode;
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
ActionResult ActionStepperStop();
ActionResult ActionStepperFindZero();
ActionResult ActionStepperZero();
ActionResult ActionStepperHomeOffset(const StepperHomeOffsetRequest& req);
ActionResult ActionHeaterSet(float power_percent);
ActionResult ActionFanSet(float power_percent);
ActionResult ActionPidApply(const PidApplyRequest& req);
ActionResult ActionPidEnable();
ActionResult ActionPidDisable();
ActionResult ActionWifiApply(const WifiApplyRequest& req);
ActionResult ActionNetApply(const NetApplyRequest& req);
ActionResult ActionCloudApply(const CloudApplyRequest& req);
ActionResult ActionGpsApply(const GpsApplyRequest& req);
ActionResult ActionGpsProbe();
ActionResult ActionUsbModeSet(UsbMode requested);
ActionResult ActionUsbModeGet();
ActionResult ActionCalibrate();
ActionResult ActionRestart();
ActionResult ActionGetState();

// Serialize current state to JSON (same payload as ActionGetState)
std::string BuildStateJsonString();
