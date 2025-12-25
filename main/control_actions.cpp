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
  cJSON_AddNumberToObject(root, "voltage1Cal", snapshot.voltage1_cal);
  cJSON_AddNumberToObject(root, "voltage2Cal", snapshot.voltage2_cal);
  cJSON_AddNumberToObject(root, "voltage3Cal", snapshot.voltage3_cal);
  cJSON_AddNumberToObject(root, "inaBusVoltage", snapshot.ina_bus_voltage);
  cJSON_AddNumberToObject(root, "inaCurrent", snapshot.ina_current);
  cJSON_AddNumberToObject(root, "inaPower", snapshot.ina_power);
  cJSON_AddBoolToObject(root, "pidEnabled", snapshot.pid_enabled);
  cJSON_AddNumberToObject(root, "pidOutput", snapshot.pid_output);
  cJSON_AddBoolToObject(root, "stepperEnabled", snapshot.stepper_enabled);
  cJSON_AddBoolToObject(root, "stepperMoving", snapshot.stepper_moving);
  cJSON_AddNumberToObject(root, "stepperPosition", snapshot.stepper_position);
  cJSON_AddNumberToObject(root, "stepperTarget", snapshot.stepper_target);
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddNumberToObject(root, "wifiRssi", snapshot.wifi_rssi_dbm);
  cJSON_AddNumberToObject(root, "wifiQuality", snapshot.wifi_quality);
  cJSON* temps = cJSON_CreateArray();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    cJSON_AddItemToArray(temps, cJSON_CreateNumber(snapshot.temps_c[i]));
  }
  cJSON_AddItemToObject(root, "temps", temps);
  cJSON* labels = cJSON_CreateArray();
  for (int i = 0; i < snapshot.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    const std::string& name = snapshot.temp_labels[i];
    if (!name.empty()) {
      cJSON_AddItemToArray(labels, cJSON_CreateString(name.c_str()));
    } else {
      std::string def = "Sensor " + std::to_string(i + 1);
      cJSON_AddItemToArray(labels, cJSON_CreateString(def.c_str()));
    }
  }
  cJSON_AddItemToObject(root, "tempLabels", labels);
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
