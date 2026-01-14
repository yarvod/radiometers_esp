#include "control_actions.h"

#include <algorithm>

#include "app_services.h"
#include "cJSON.h"

namespace {

std::string BuildStateJsonInternal() {
  SharedState snapshot = CopyState();
  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "logging", snapshot.logging);
  cJSON_AddStringToObject(root, "logFilename", snapshot.log_filename.c_str());
  cJSON_AddBoolToObject(root, "logUseMotor", snapshot.log_use_motor);
  cJSON_AddNumberToObject(root, "logDuration", snapshot.log_duration_s);
  cJSON_AddNumberToObject(root, "voltage1", snapshot.voltage1);
  cJSON_AddNumberToObject(root, "voltage2", snapshot.voltage2);
  cJSON_AddNumberToObject(root, "voltage3", snapshot.voltage3);
  cJSON_AddNumberToObject(root, "inaBusVoltage", snapshot.ina_bus_voltage);
  cJSON_AddNumberToObject(root, "inaCurrent", snapshot.ina_current);
  cJSON_AddNumberToObject(root, "inaPower", snapshot.ina_power);
  cJSON_AddBoolToObject(root, "pidEnabled", snapshot.pid_enabled);
  cJSON_AddNumberToObject(root, "pidOutput", snapshot.pid_output);
  cJSON_AddNumberToObject(root, "pidSetpoint", snapshot.pid_setpoint);
  cJSON_AddNumberToObject(root, "pidSensorIndex", snapshot.pid_sensor_index);
  cJSON_AddNumberToObject(root, "pidSensorMask", snapshot.pid_sensor_mask);
  cJSON_AddNumberToObject(root, "pidKp", snapshot.pid_kp);
  cJSON_AddNumberToObject(root, "pidKi", snapshot.pid_ki);
  cJSON_AddNumberToObject(root, "pidKd", snapshot.pid_kd);
  cJSON_AddBoolToObject(root, "stepperEnabled", snapshot.stepper_enabled);
  cJSON_AddBoolToObject(root, "stepperHoming", snapshot.homing);
  cJSON_AddBoolToObject(root, "stepperDirForward", snapshot.stepper_direction_forward);
  cJSON_AddBoolToObject(root, "stepperMoving", snapshot.stepper_moving);
  cJSON_AddNumberToObject(root, "stepperPosition", snapshot.stepper_position);
  cJSON_AddNumberToObject(root, "stepperTarget", snapshot.stepper_target);
  cJSON_AddBoolToObject(root, "stepperHomed", snapshot.stepper_homed);
  cJSON_AddStringToObject(root, "stepperHomeStatus", snapshot.stepper_home_status.c_str());
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddNumberToObject(root, "wifiRssi", snapshot.wifi_rssi_dbm);
  cJSON_AddNumberToObject(root, "wifiQuality", snapshot.wifi_quality);
  cJSON_AddStringToObject(root, "wifiIp", snapshot.wifi_ip.c_str());
  cJSON_AddStringToObject(root, "wifiStaIp", snapshot.wifi_ip_sta.c_str());
  cJSON_AddStringToObject(root, "wifiApIp", snapshot.wifi_ip_ap.c_str());
  cJSON_AddNumberToObject(root, "sdTotalBytes", static_cast<double>(snapshot.sd_total_bytes));
  cJSON_AddNumberToObject(root, "sdUsedBytes", static_cast<double>(snapshot.sd_used_bytes));
  cJSON_AddNumberToObject(root, "sdRootDataFiles", snapshot.sd_data_root_files);
  cJSON_AddNumberToObject(root, "sdToUploadFiles", snapshot.sd_to_upload_files);
  cJSON_AddNumberToObject(root, "sdUploadedFiles", snapshot.sd_uploaded_files);
  cJSON_AddBoolToObject(root, "wifiApMode", app_config.wifi_ap_mode);
  cJSON_AddStringToObject(root, "wifiMode", app_config.wifi_ap_mode ? "ap" : "sta");
  cJSON_AddStringToObject(root, "wifiSsid", app_config.wifi_ssid.c_str());
  cJSON* temp_obj = cJSON_CreateObject();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    const std::string key = "t" + std::to_string(i + 1);
    const std::string& addr = snapshot.temp_addresses[i];
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "value", snapshot.temps_c[i]);
    cJSON_AddStringToObject(entry, "address", addr.c_str());
    cJSON_AddStringToObject(entry, "label", key.c_str());
    cJSON_AddItemToObject(temp_obj, key.c_str(), entry);
  }
  cJSON_AddItemToObject(root, "tempSensors", temp_obj);
  const std::string iso = IsoUtcNow();
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  const char* json = cJSON_PrintUnformatted(root);
  std::string result = json ? json : "{}";
  cJSON_free((void*)json);
  cJSON_Delete(root);
  return result;
}

}  // namespace

std::string BuildStateJsonString() {
  return BuildStateJsonInternal();
}

ActionResult ActionStartLog(const LogRequest& req) {
  LogRequest r = req;
  if (r.duration_s <= 0.0f) r.duration_s = 1.0f;
  log_config.use_motor = r.use_motor;
  log_config.duration_s = r.duration_s;
  if (!StartLoggingToFile(r.filename, usb_mode)) {
    return {false, "Failed to start logging", {}};
  }
  const std::string postfix = SanitizePostfix(r.filename);
  app_config.logging_active = true;
  app_config.logging_postfix = postfix;
  app_config.logging_use_motor = r.use_motor;
  app_config.logging_duration_s = r.duration_s;
  UpdateState([&](SharedState& s) {
    s.logging = true;
    s.log_use_motor = r.use_motor;
    s.log_duration_s = r.duration_s;
  });
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  return {true, "logging_started", {}};
}

ActionResult ActionStopLog() {
  StopLogging();
  app_config.logging_active = false;
  app_config.logging_use_motor = false;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  UpdateState([](SharedState& s) {
    s.logging = false;
    s.log_use_motor = false;
  });
  return {true, "logging_stopped", {}};
}

ActionResult ActionStepperEnable() {
  EnableStepper();
  return {true, "stepper_enabled", {}};
}

ActionResult ActionStepperDisable() {
  DisableStepper();
  return {true, "stepper_disabled", {}};
}

ActionResult ActionStepperMove(const StepperMoveRequest& req) {
  SharedState snapshot = CopyState();
  if (!snapshot.stepper_enabled) {
    return {false, "Stepper not enabled", {}};
  }
  int steps = std::clamp(req.steps, 1, 10000);
  int speed = req.speed_us > 0 ? req.speed_us : snapshot.stepper_speed_us;
  StartStepperMove(steps, req.forward, speed);
  app_config.stepper_speed_us = speed;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  return {true, "movement_started", {}};
}

ActionResult ActionStepperFindZero() {
  SharedState snapshot = CopyState();
  if (!snapshot.stepper_enabled) {
    return {false, "Stepper not enabled", {}};
  }
  std::string msg;
  if (!StartFindZeroTask(&msg)) {
    return {false, msg, {}};
  }
  return {true, "homing_started", {}};
}

ActionResult ActionStepperZero() {
  UpdateState([](SharedState& s) {
    s.stepper_position = 0;
    s.stepper_target = 0;
    s.stepper_homed = true;
    s.stepper_home_status = "manual_zero";
  });
  return {true, "position_zeroed", {}};
}

ActionResult ActionHeaterSet(float power_percent) {
  HeaterSetPowerPercent(power_percent);
  return {true, "heater_set", {}};
}

ActionResult ActionFanSet(float power_percent) {
  FanSetPowerPercent(power_percent);
  return {true, "fan_set", {}};
}

ActionResult ActionGetState() {
  return {true, {}, BuildStateJsonInternal()};
}
