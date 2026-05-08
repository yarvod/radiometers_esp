#include "control_actions.h"

#include <algorithm>
#include <cctype>

#include "app_services.h"
#include "app_utils.h"
#include "cJSON.h"
#include "freertos/task.h"
#include "tusb_msc_storage.h"

namespace {

TaskHandle_t network_apply_task = nullptr;

void NetworkApplyTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(700));
  ApplyNetworkConfig();
  network_apply_task = nullptr;
  vTaskDelete(nullptr);
}

void ScheduleNetworkApply() {
  if (network_apply_task) {
    return;
  }
  xTaskCreatePinnedToCore(&NetworkApplyTask, "net_apply", 4096, nullptr, 2, &network_apply_task, 0);
}

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
  cJSON_AddNumberToObject(root, "stepperSpeedUs", snapshot.stepper_speed_us);
  cJSON_AddNumberToObject(root, "stepperHomeOffsetSteps", snapshot.stepper_home_offset_steps);
  cJSON_AddNumberToObject(root, "motorHallActiveLevel", snapshot.motor_hall_active_level);
  cJSON_AddBoolToObject(root, "stepperHomed", snapshot.stepper_homed);
  cJSON_AddStringToObject(root, "stepperHomeStatus", snapshot.stepper_home_status.c_str());
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddBoolToObject(root, "externalPowerOn", snapshot.external_power_on);
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
  cJSON* gps_types = cJSON_CreateArray();
  for (uint16_t type : app_config.gps_rtcm_types) {
    cJSON_AddItemToArray(gps_types, cJSON_CreateNumber(type));
  }
  cJSON_AddItemToObject(root, "gpsRtcmTypes", gps_types);
  cJSON_AddStringToObject(root, "gpsMode", app_config.gps_mode.c_str());
  char gps_actual_mode[256] = {};
  GetGpsCurrentModeText(gps_actual_mode, sizeof(gps_actual_mode));
  cJSON_AddStringToObject(root, "gpsActualMode", gps_actual_mode);
  cJSON_AddNumberToObject(root, "gpsAntennaShortRaw", GetGpsAntennaShortRaw());
  cJSON_AddBoolToObject(root, "gpsAntennaShort", IsGpsAntennaShort());
  const GpsReceiverStatus gps_status = GetGpsReceiverStatus();
  cJSON_AddBoolToObject(root, "gpsPositionValid", gps_status.position_valid);
  if (gps_status.position_valid) {
    cJSON_AddNumberToObject(root, "gpsLat", gps_status.latitude_deg);
    cJSON_AddNumberToObject(root, "gpsLon", gps_status.longitude_deg);
    cJSON_AddNumberToObject(root, "gpsAlt", gps_status.altitude_m);
    cJSON_AddNumberToObject(root, "gpsFixQuality", gps_status.fix_quality);
    cJSON_AddNumberToObject(root, "gpsSatellites", gps_status.satellites);
    cJSON_AddNumberToObject(root, "gpsPositionAgeMs", static_cast<double>(gps_status.position_age_ms));
  }
  cJSON_AddBoolToObject(root, "gpsTimeValid", gps_status.time_valid);
  if (gps_status.time_valid) {
    cJSON_AddStringToObject(root, "gpsTimeIso", gps_status.time_iso);
    cJSON_AddNumberToObject(root, "gpsTimeAgeMs", static_cast<double>(gps_status.time_age_ms));
  }
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
  const UtcTimeSnapshot now = GetBestUtcTimeForData();
  const std::string iso = FormatUtcIso(now);
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  cJSON_AddNumberToObject(root, "timestampMs", static_cast<double>(UtcTimeToUnixMs(now)));
  cJSON_AddStringToObject(root, "timeSource", UtcTimeSourceName(now.source));
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

ActionResult ActionStepperStop() {
  StopStepper();
  return {true, "movement_stopped", {}};
}

ActionResult ActionStepperFindZero() {
  SharedState snapshot = CopyState();
  if (!snapshot.stepper_enabled) {
    EnableStepper();
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

ActionResult ActionStepperHomeOffset(const StepperHomeOffsetRequest& req) {
  const int speed = req.speed_us > 0 ? req.speed_us : app_config.stepper_speed_us;
  app_config.stepper_home_offset_steps = req.offset_steps;
  app_config.stepper_speed_us = speed;
  if (req.hall_active_level_set) {
    app_config.motor_hall_active_level = req.hall_active_level ? 1 : 0;
  }
  UpdateState([&](SharedState& s) {
    s.stepper_home_offset_steps = app_config.stepper_home_offset_steps;
    s.stepper_speed_us = app_config.stepper_speed_us;
    s.motor_hall_active_level = app_config.motor_hall_active_level;
  });
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "speedUs", app_config.stepper_speed_us);
  cJSON_AddNumberToObject(root, "offsetSteps", app_config.stepper_home_offset_steps);
  cJSON_AddNumberToObject(root, "hallActiveLevel", app_config.motor_hall_active_level);
  const char* json = cJSON_PrintUnformatted(root);
  std::string payload = json ? json : "{}";
  cJSON_free((void*)json);
  cJSON_Delete(root);
  return {true, "stepper_settings_saved", payload};
}

ActionResult ActionHeaterSet(float power_percent) {
  HeaterSetPowerPercent(power_percent);
  return {true, "heater_set", {}};
}

ActionResult ActionFanSet(float power_percent) {
  FanSetPowerPercent(power_percent);
  return {true, "fan_set", {}};
}

ActionResult ActionExternalPowerSet(bool enabled) {
  SetExternalPower(enabled);
  return {true, enabled ? "external_power_on" : "external_power_off", {}};
}

ActionResult ActionExternalPowerCycle(uint32_t off_ms) {
  if (!CycleExternalPower(off_ms)) {
    return {false, "external power cycle already running", {}};
  }
  return {true, "external_power_cycle_started", {}};
}

ActionResult ActionPidApply(const PidApplyRequest& req) {
  int sensor = req.sensor;
  uint16_t sensor_mask = req.sensor_mask;

  SharedState snapshot = CopyState();
  if (snapshot.temp_sensor_count > 0) {
    sensor = std::clamp(sensor, 0, snapshot.temp_sensor_count - 1);
  } else if (sensor < 0) {
    sensor = 0;
  }
  if (req.sensor_mask_set) {
    sensor_mask = ClampSensorMask(sensor_mask, snapshot.temp_sensor_count);
    if (sensor_mask == 0 && snapshot.temp_sensor_count > 0) {
      sensor_mask = static_cast<uint16_t>(1u << sensor);
    }
    sensor = FirstSetBitIndex(sensor_mask);
  } else {
    sensor_mask = static_cast<uint16_t>(1u << sensor);
  }

  pid_config.kp = req.kp;
  pid_config.ki = req.ki;
  pid_config.kd = req.kd;
  pid_config.setpoint = req.setpoint;
  pid_config.sensor_index = sensor;
  pid_config.sensor_mask = sensor_mask;

  UpdateState([&](SharedState& s) {
    s.pid_kp = pid_config.kp;
    s.pid_ki = pid_config.ki;
    s.pid_kd = pid_config.kd;
    s.pid_setpoint = pid_config.setpoint;
    s.pid_sensor_index = pid_config.sensor_index;
    s.pid_sensor_mask = pid_config.sensor_mask;
  });

  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  return {true, "pid_applied", {}};
}

ActionResult ActionPidEnable() {
  SharedState snapshot = CopyState();
  if (snapshot.temp_sensor_count == 0) {
    return {false, "no temp sensors", {}};
  }
  UpdateState([](SharedState& s) { s.pid_enabled = true; });
  return {true, "pid_enabled", {}};
}

ActionResult ActionPidDisable() {
  UpdateState([](SharedState& s) { s.pid_enabled = false; });
  return {true, "pid_disabled", {}};
}

ActionResult ActionWifiApply(const WifiApplyRequest& req) {
  std::string mode = req.mode;
  std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (mode != "ap") mode = "sta";

  if (req.ssid.empty() || req.ssid.size() >= WIFI_SSID_MAX_LEN) {
    return {false, "invalid ssid", {}};
  }
  if (req.password.size() >= WIFI_PASSWORD_MAX_LEN) {
    return {false, "invalid password", {}};
  }

  app_config.wifi_ssid = req.ssid;
  app_config.wifi_password = req.password;
  app_config.wifi_ap_mode = (mode == "ap");
  app_config.wifi_from_file = true;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  ScheduleNetworkApply();
  return {true, "wifi_saved_reconnect_scheduled", {}};
}

ActionResult ActionNetApply(const NetApplyRequest& req) {
  NetMode new_mode = app_config.net_mode;
  NetPriority new_priority = app_config.net_priority;
  if (!req.mode.empty() && !ParseNetMode(req.mode, &new_mode)) {
    return {false, "invalid net mode", {}};
  }
  if (!req.priority.empty() && !ParseNetPriority(req.priority, &new_priority)) {
    return {false, "invalid net priority", {}};
  }
  app_config.net_mode = new_mode;
  app_config.net_priority = new_priority;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  ScheduleNetworkApply();
  return {true, "net_saved_reconnect_scheduled", {}};
}

ActionResult ActionCloudApply(const CloudApplyRequest& req) {
  app_config.device_id = req.device_id;
  app_config.minio_endpoint = req.minio_endpoint;
  app_config.minio_access_key = req.minio_access_key;
  app_config.minio_secret_key = req.minio_secret_key;
  app_config.minio_bucket = req.minio_bucket;
  app_config.minio_enabled = req.minio_enabled;
  app_config.mqtt_uri = NormalizeMqttUri(req.mqtt_uri);
  app_config.mqtt_user = req.mqtt_user;
  app_config.mqtt_password = req.mqtt_password;
  app_config.mqtt_enabled = req.mqtt_enabled;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  return {true, "cloud_applied", {}};
}

ActionResult ActionGpsApply(const GpsApplyRequest& req) {
  if (req.rtcm_types.empty()) {
    return {false, "empty rtcm types", {}};
  }
  std::vector<uint16_t> types;
  types.reserve(req.rtcm_types.size());
  for (uint16_t type : req.rtcm_types) {
    if (type == 0 || type > 4095) {
      return {false, "invalid rtcm type", {}};
    }
    if (std::find(types.begin(), types.end(), type) == types.end()) {
      types.push_back(type);
    }
  }
  const std::string mode = req.mode.empty() ? "base_time_60" : req.mode;
  if (!(mode == "keep" || mode == "base_time_60" || mode == "base" || mode == "rover_uav" || mode == "rover")) {
    return {false, "invalid gps mode", {}};
  }
  app_config.gps_rtcm_types = types;
  app_config.gps_mode = mode;
  SaveConfigToSdCard(app_config, pid_config, usb_mode);
  RequestGpsReconfigure();
  return {true, "gps_applied", {}};
}

ActionResult ActionGpsProbe() {
  ProbeGpsMode();
  return {true, "gps_probe_sent", {}};
}

ActionResult ActionConfigSyncInternalFlash() {
  if (!SyncConfigToInternalFlash()) {
    return {false, "config_internal_flash_sync_failed", {}};
  }
  return {true, "config_synced_to_internal_flash", {}};
}

ActionResult ActionUsbModeSet(UsbMode requested) {
#if !CONFIG_TINYUSB_MSC_ENABLED
  if (requested == UsbMode::kMsc) {
    return {false, "MSC not enabled in firmware", {}};
  }
#endif
  if (requested == usb_mode) {
    return {true, "unchanged", {}};
  }
  if (usb_mode == UsbMode::kMsc && requested == UsbMode::kCdc) {
    tinyusb_msc_storage_unmount();
  }
  SaveUsbModeToNvs(requested);
  ScheduleRestart();
  return {true, "restarting", {}};
}

ActionResult ActionUsbModeGet() {
  return {true, usb_mode == UsbMode::kMsc ? "msc" : "cdc", {}};
}

ActionResult ActionCalibrate() {
  if (calibration_task == nullptr) {
    xTaskCreatePinnedToCore(&CalibrationTask, "calibrate", 4096, nullptr, 4, &calibration_task, tskNO_AFFINITY);
  }
  return {true, "calibration_started", {}};
}

ActionResult ActionRestart() {
  if (CopyState().logging) {
    StopLogging();
  }
  ScheduleRestart();
  return {true, "restarting", {}};
}

ActionResult ActionGetState() {
  return {true, {}, BuildStateJsonInternal()};
}
