#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "app_state.h"
#include "app_services.h"
#include "control_actions.h"
#include "app_utils.h"
#include "web_ui.h"
#include "mqtt_bridge.h"
#include "error_manager.h"
#include "sd_maintenance.h"
#include "hw_pins.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "config_loader.h"

static constexpr char kTag[] = "HTTP";


template <typename T>
struct PsramAllocator {
  using value_type = T;

  PsramAllocator() = default;
  template <typename U>
  PsramAllocator(const PsramAllocator<U>&) {}

  T* allocate(std::size_t n) {
    if (n > static_cast<std::size_t>(-1) / sizeof(T)) {
      std::abort();
    }
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
      p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
    }
    if (!p) {
      std::abort();
    }
    return static_cast<T*>(p);
  }

  void deallocate(T* p, std::size_t) noexcept {
    heap_caps_free(p);
  }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) {
  return true;
}

template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) {
  return false;
}

using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

static PsramString ToPsramString(const char* s) {
  return PsramString(s ? s : "");
}

static PsramString ToPsramString(const std::string& s) {
  return PsramString(s.c_str(), s.size());
}

static void AppendJsonEscaped(PsramString& out, const char* s) {
  out.push_back('"');
  if (s) {
    for (const char* p = s; *p; ++p) {
      const unsigned char c = static_cast<unsigned char>(*p);
      switch (c) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\b':
          out += "\\b";
          break;
        case '\f':
          out += "\\f";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (c < 0x20) {
            char buf[7];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out.push_back(static_cast<char>(c));
          }
          break;
      }
    }
  }
  out.push_back('"');
}

static void AppendJsonEscaped(PsramString& out, const PsramString& s) {
  AppendJsonEscaped(out, s.c_str());
}

static void AppendJsonNumber(PsramString& out, uint64_t value) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
  out += buf;
}

static void AppendJsonNumber(PsramString& out, int value) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", value);
  out += buf;
}

static void AppendJsonBool(PsramString& out, bool value) {
  out += value ? "true" : "false";
}

static esp_err_t SendPsramJson(httpd_req_t* req, const PsramString& json) {
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.size());
}

static bool UrlDecode(const char* src, std::string* out) {
  if (!src || !out) return false;
  out->clear();
  for (size_t i = 0; src[i] != '\0'; ++i) {
    char c = src[i];
    if (c == '%') {
      if (!std::isxdigit(static_cast<unsigned char>(src[i + 1])) || !std::isxdigit(static_cast<unsigned char>(src[i + 2]))) {
        return false;
      }
      auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return 0;
      };
      int v = (hex(src[i + 1]) << 4) | hex(src[i + 2]);
      out->push_back(static_cast<char>(v));
      i += 2;
    } else if (c == '+') {
      out->push_back(' ');
    } else {
      out->push_back(c);
    }
    if (out->size() > 512) return false;
  }
  return true;
}

static std::vector<uint16_t> ParseRtcmTypesText(const std::string& text) {
  std::vector<uint16_t> out;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && (text[start] == ',' || text[start] == ';' || std::isspace(static_cast<unsigned char>(text[start])))) {
      start++;
    }
    if (start >= text.size()) break;
    size_t end = start;
    while (end < text.size() && text[end] != ',' && text[end] != ';' && !std::isspace(static_cast<unsigned char>(text[end]))) {
      end++;
    }
    const int type = std::atoi(text.substr(start, end - start).c_str());
    if (type > 0 && type <= 4095 &&
        std::find(out.begin(), out.end(), static_cast<uint16_t>(type)) == out.end()) {
      out.push_back(static_cast<uint16_t>(type));
    }
    start = end;
  }
  return out;
}

// HTTP handlers
esp_err_t RootHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t DataHandler(httpd_req_t* req) {
  RefreshHallDebugState();
  SharedState snapshot = CopyState();
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "voltage1", snapshot.voltage1);
  cJSON_AddNumberToObject(root, "voltage2", snapshot.voltage2);
  cJSON_AddNumberToObject(root, "voltage3", snapshot.voltage3);
  cJSON_AddNumberToObject(root, "inaBusVoltage", snapshot.ina_bus_voltage);
  cJSON_AddNumberToObject(root, "inaCurrent", snapshot.ina_current);
  cJSON_AddNumberToObject(root, "inaPower", snapshot.ina_power);
  cJSON_AddNumberToObject(root, "wifiRssi", snapshot.wifi_rssi_dbm);
  cJSON_AddNumberToObject(root, "wifiQuality", snapshot.wifi_quality);
  cJSON_AddStringToObject(root, "wifiIp", snapshot.wifi_ip.c_str());
  cJSON_AddStringToObject(root, "wifiStaIp", snapshot.wifi_ip_sta.c_str());
  cJSON_AddStringToObject(root, "wifiApIp", snapshot.wifi_ip_ap.c_str());
  cJSON_AddStringToObject(root, "ethIp", snapshot.eth_ip.c_str());
  cJSON_AddBoolToObject(root, "ethLink", snapshot.eth_link_up);
  cJSON_AddBoolToObject(root, "ethIpUp", snapshot.eth_ip_up);
  cJSON_AddNumberToObject(root, "sdTotalBytes", static_cast<double>(snapshot.sd_total_bytes));
  cJSON_AddNumberToObject(root, "sdUsedBytes", static_cast<double>(snapshot.sd_used_bytes));
  cJSON_AddNumberToObject(root, "sdRootDataFiles", snapshot.sd_data_root_files);
  cJSON_AddNumberToObject(root, "sdToUploadFiles", snapshot.sd_to_upload_files);
  cJSON_AddNumberToObject(root, "sdUploadedFiles", snapshot.sd_uploaded_files);
  cJSON_AddStringToObject(root, "storageBackend", StorageBackendToString(app_config.storage_backend).c_str());
  cJSON_AddBoolToObject(root, "sdMounted", IsLogSdMounted());
  cJSON_AddBoolToObject(root, "internalFlashMounted", IsInternalFlashMounted());
  cJSON_AddBoolToObject(root, "activeStorageMounted",
                        app_config.storage_backend == StorageBackend::kInternalFlash ? IsInternalFlashMounted() : IsLogSdMounted());
  cJSON_AddNumberToObject(root, "heapFreeBytes", static_cast<double>(snapshot.heap_free_bytes));
  cJSON_AddNumberToObject(root, "heapMinFreeBytes", static_cast<double>(snapshot.heap_min_free_bytes));
  cJSON_AddNumberToObject(root, "heapLargestFreeBlockBytes", static_cast<double>(snapshot.heap_largest_free_block_bytes));
  cJSON_AddNumberToObject(root, "heapInternalFreeBytes", static_cast<double>(snapshot.heap_internal_free_bytes));
  cJSON_AddNumberToObject(root, "heapInternalLargestFreeBlockBytes",
                          static_cast<double>(snapshot.heap_internal_largest_free_block_bytes));
  cJSON_AddNumberToObject(root, "heapPsramFreeBytes", static_cast<double>(snapshot.heap_psram_free_bytes));
  cJSON_AddNumberToObject(root, "heapPsramLargestFreeBlockBytes",
                          static_cast<double>(snapshot.heap_psram_largest_free_block_bytes));
  cJSON_AddNumberToObject(root, "minioUploadAttempts", snapshot.minio_upload_attempts);
  cJSON_AddNumberToObject(root, "minioUploadSuccesses", snapshot.minio_upload_successes);
  cJSON_AddNumberToObject(root, "minioUploadFailures", snapshot.minio_upload_failures);
  cJSON_AddNumberToObject(root, "minioArchiveFailures", snapshot.minio_archive_failures);
  cJSON_AddNumberToObject(root, "minioLastAttemptMs", static_cast<double>(snapshot.minio_last_attempt_ms));
  cJSON_AddNumberToObject(root, "minioLastSuccessMs", static_cast<double>(snapshot.minio_last_success_ms));
  cJSON_AddNumberToObject(root, "minioLastFailureMs", static_cast<double>(snapshot.minio_last_failure_ms));
  cJSON_AddNumberToObject(root, "uptimeMs", static_cast<double>(esp_timer_get_time() / 1000ULL));
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddBoolToObject(root, "externalPowerOn", snapshot.external_power_on);
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "tempSensorCount", snapshot.temp_sensor_count);
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
  cJSON_AddBoolToObject(root, "logging", snapshot.logging);
  cJSON_AddStringToObject(root, "logFilename", snapshot.log_filename.c_str());
  cJSON_AddBoolToObject(root, "logUseMotor", snapshot.log_use_motor);
  cJSON_AddNumberToObject(root, "logDuration", snapshot.log_duration_s);
  cJSON_AddNumberToObject(root, "loggingMotorSteps", app_config.logging_motor_steps);
  cJSON_AddBoolToObject(root, "loggingHomeEachCycle", app_config.logging_home_each_cycle);
  cJSON_AddBoolToObject(root, "wifiApMode", app_config.wifi_ap_mode);
  cJSON_AddStringToObject(root, "wifiSsid", app_config.wifi_ssid.c_str());
  cJSON_AddStringToObject(root, "wifiPassword", app_config.wifi_password.c_str());
  cJSON_AddStringToObject(root, "netMode", NetModeToString(app_config.net_mode).c_str());
  cJSON_AddStringToObject(root, "netPriority", NetPriorityToString(app_config.net_priority).c_str());
  cJSON_AddBoolToObject(root, "ethDhcp", app_config.eth_dhcp);
  cJSON_AddStringToObject(root, "ethStaticIp", app_config.eth_static_ip.c_str());
  cJSON_AddStringToObject(root, "ethStaticNetmask", app_config.eth_static_netmask.c_str());
  cJSON_AddStringToObject(root, "ethStaticGateway", app_config.eth_static_gateway.c_str());
  cJSON_AddStringToObject(root, "ethStaticDns", app_config.eth_static_dns.c_str());
  cJSON* gps_types = cJSON_CreateArray();
  for (uint16_t type : app_config.gps_rtcm_types) {
    cJSON_AddItemToArray(gps_types, cJSON_CreateNumber(type));
  }
  cJSON_AddItemToObject(root, "gpsRtcmTypes", gps_types);
  cJSON_AddStringToObject(root, "gpsMode", app_config.gps_mode.c_str());
  cJSON_AddNumberToObject(root, "meteoPollIntervalS", app_config.meteo_poll_interval_s);
  cJSON_AddNumberToObject(root, "meteoFileIntervalS", app_config.meteo_file_interval_s);
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
  cJSON_AddStringToObject(root, "deviceId", app_config.device_id.c_str());
  cJSON_AddStringToObject(root, "minioEndpoint", app_config.minio_endpoint.c_str());
  cJSON_AddStringToObject(root, "minioAccessKey", app_config.minio_access_key.c_str());
  cJSON_AddStringToObject(root, "minioSecretKey", app_config.minio_secret_key.c_str());
  cJSON_AddStringToObject(root, "minioBucket", app_config.minio_bucket.c_str());
  cJSON_AddBoolToObject(root, "minioEnabled", app_config.minio_enabled);
  cJSON_AddStringToObject(root, "mqttUri", app_config.mqtt_uri.c_str());
  cJSON_AddStringToObject(root, "mqttUser", app_config.mqtt_user.c_str());
  cJSON_AddStringToObject(root, "mqttPassword", app_config.mqtt_password.c_str());
  cJSON_AddBoolToObject(root, "mqttEnabled", app_config.mqtt_enabled);
  cJSON_AddBoolToObject(root, "pidEnabled", snapshot.pid_enabled);
  cJSON_AddNumberToObject(root, "pidSetpoint", snapshot.pid_setpoint);
  cJSON_AddNumberToObject(root, "pidSensorIndex", snapshot.pid_sensor_index);
  cJSON_AddNumberToObject(root, "pidSensorMask", snapshot.pid_sensor_mask);
  cJSON_AddNumberToObject(root, "pidKp", snapshot.pid_kp);
  cJSON_AddNumberToObject(root, "pidKi", snapshot.pid_ki);
  cJSON_AddNumberToObject(root, "pidKd", snapshot.pid_kd);
  cJSON_AddNumberToObject(root, "pidOutput", snapshot.pid_output);
  cJSON_AddNumberToObject(root, "pidTemperature", snapshot.pid_temperature);
  cJSON_AddNumberToObject(root, "pidError", snapshot.pid_error);
  cJSON_AddNumberToObject(root, "pidIntegral", snapshot.pid_integral);
  cJSON_AddNumberToObject(root, "pidIntegralCandidate", snapshot.pid_integral_candidate);
  cJSON_AddNumberToObject(root, "pidDerivative", snapshot.pid_derivative);
  cJSON_AddNumberToObject(root, "pidPTerm", snapshot.pid_p_term);
  cJSON_AddNumberToObject(root, "pidITerm", snapshot.pid_i_term);
  cJSON_AddNumberToObject(root, "pidDTerm", snapshot.pid_d_term);
  cJSON_AddNumberToObject(root, "pidRawOutput", snapshot.pid_raw_output);
  cJSON_AddNumberToObject(root, "pidDt", snapshot.pid_dt);
  cJSON_AddBoolToObject(root, "pidSaturatedHigh", snapshot.pid_saturated_high);
  cJSON_AddBoolToObject(root, "pidSaturatedLow", snapshot.pid_saturated_low);
  cJSON_AddBoolToObject(root, "pidIntegralHeld", snapshot.pid_integral_held);
  cJSON_AddStringToObject(root, "wifiMode", app_config.wifi_ap_mode ? "ap" : "sta");
  cJSON_AddNumberToObject(root, "timestamp", snapshot.last_update_ms);
  const UtcTimeSnapshot now = GetBestUtcTimeForData();
  const std::string iso = FormatUtcIso(now);
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  cJSON_AddStringToObject(root, "timeSource", UtcTimeSourceName(now.source));
  cJSON_AddBoolToObject(root, "stepperEnabled", snapshot.stepper_enabled);
  cJSON_AddBoolToObject(root, "stepperHoming", snapshot.homing);
  cJSON_AddBoolToObject(root, "stepperDirForward", snapshot.stepper_direction_forward);
  cJSON_AddNumberToObject(root, "stepperPosition", snapshot.stepper_position);
  cJSON_AddNumberToObject(root, "stepperTarget", snapshot.stepper_target);
  cJSON_AddNumberToObject(root, "stepperSpeedUs", snapshot.stepper_speed_us);
  cJSON_AddNumberToObject(root, "stepperHomeOffsetSteps", snapshot.stepper_home_offset_steps);
  cJSON_AddNumberToObject(root, "motorHallActiveLevel", snapshot.motor_hall_active_level);
  cJSON_AddNumberToObject(root, "motorHallRawLevel", snapshot.motor_hall_raw_level);
  cJSON_AddBoolToObject(root, "motorHallTriggered", snapshot.motor_hall_triggered);
  cJSON_AddNumberToObject(root, "motorHallEdgeCount", snapshot.motor_hall_edge_count);
  cJSON_AddNumberToObject(root, "motorHallActiveEdgeCount", snapshot.motor_hall_active_edge_count);
  cJSON_AddNumberToObject(root, "motorHallLevel0EdgeCount", snapshot.motor_hall_level0_edge_count);
  cJSON_AddNumberToObject(root, "motorHallLevel1EdgeCount", snapshot.motor_hall_level1_edge_count);
  cJSON_AddNumberToObject(root, "motorHallLastEdgeLevel", snapshot.motor_hall_last_edge_level);
  cJSON_AddNumberToObject(root, "motorHallLastEdgeSeenUs", static_cast<double>(snapshot.motor_hall_last_edge_seen_us));
  cJSON_AddBoolToObject(root, "stepperMoving", snapshot.stepper_moving);
  cJSON_AddBoolToObject(root, "stepperHomed", snapshot.stepper_homed);
  cJSON_AddStringToObject(root, "stepperHomeStatus", snapshot.stepper_home_status.c_str());
  if (!snapshot.usb_error.empty()) {
    cJSON_AddStringToObject(root, "usbError", snapshot.usb_error.c_str());
  }

  const char* resp = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, resp);
  cJSON_free((void*)resp);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t CalibrateHandler(httpd_req_t* req) {
  ActionResult res = ActionCalibrate();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"calibration_started\"}");
}

esp_err_t RestartHandler(httpd_req_t* req) {
  ActionResult res = ActionRestart();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
}

esp_err_t ExternalPowerSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* enabled_item = cJSON_GetObjectItem(root, "enabled");
  if (!enabled_item) {
    enabled_item = cJSON_GetObjectItem(root, "on");
  }
  if (!enabled_item || (!cJSON_IsBool(enabled_item) && !cJSON_IsNumber(enabled_item))) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled");
    return ESP_FAIL;
  }
  const bool enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item) : enabled_item->valueint != 0;
  cJSON_Delete(root);

  ActionResult res = ActionExternalPowerSet(enabled);
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, enabled ? "{\"status\":\"external_power_on\"}" : "{\"status\":\"external_power_off\"}");
}

esp_err_t ExternalPowerCycleHandler(httpd_req_t* req) {
  uint32_t off_ms = 1000;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
    std::string body(buf_len, '\0');
    int received = httpd_req_recv(req, body.data(), buf_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
      return ESP_FAIL;
    }
    body.resize(received);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
      return ESP_FAIL;
    }
    cJSON* off_item = cJSON_GetObjectItem(root, "offMs");
    if (off_item && cJSON_IsNumber(off_item) && off_item->valueint > 0) {
      off_ms = static_cast<uint32_t>(off_item->valueint);
    }
    cJSON_Delete(root);
  }

  ActionResult res = ActionExternalPowerCycle(off_ms);
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"external_power_cycle_started\"}");
}

esp_err_t StepperEnableHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperEnable();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"stepper_enabled\"}");
}

esp_err_t StepperDisableHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperDisable();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"stepper_disabled\"}");
}

esp_err_t StepperMoveHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 256);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  int steps = cJSON_GetObjectItem(root, "steps") ? cJSON_GetObjectItem(root, "steps")->valueint : 400;
  bool forward = true;
  cJSON* dir_item = cJSON_GetObjectItem(root, "direction");
  if (dir_item) {
    if (cJSON_IsString(dir_item) && dir_item->valuestring) {
      std::string dir = dir_item->valuestring;
      std::transform(dir.begin(), dir.end(), dir.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (dir.find("back") == 0) {
        forward = false;
      } else if (dir.find("rev") == 0) {
        forward = false;
      } else if (dir.find("fwd") == 0 || dir.find("forw") == 0) {
        forward = true;
      }
    } else if (cJSON_IsNumber(dir_item)) {
      forward = dir_item->valueint != 0;
    } else if (cJSON_IsBool(dir_item)) {
      forward = cJSON_IsTrue(dir_item);
    }
  }
  int speed = cJSON_GetObjectItem(root, "speed") ? cJSON_GetObjectItem(root, "speed")->valueint : app_config.stepper_speed_us;
  cJSON_Delete(root);

  StepperMoveRequest move_req;
  move_req.steps = steps;
  move_req.forward = forward;
  move_req.speed_us = speed;
  ActionResult res = ActionStepperMove(move_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"movement_started\"}");
}

esp_err_t StepperHomeOffsetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 256);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* offset_item = cJSON_GetObjectItem(root, "offsetSteps");
  if (!offset_item) {
    offset_item = cJSON_GetObjectItem(root, "offset");
  }
  const int offset_steps = (offset_item && cJSON_IsNumber(offset_item))
                               ? offset_item->valueint
                               : app_config.stepper_home_offset_steps;
  cJSON* speed_item = cJSON_GetObjectItem(root, "speedUs");
  if (!speed_item) {
    speed_item = cJSON_GetObjectItem(root, "speed");
  }
  const int speed_us = (speed_item && cJSON_IsNumber(speed_item) && speed_item->valueint > 0)
                           ? speed_item->valueint
                           : app_config.stepper_speed_us;
  cJSON* logging_steps_item = cJSON_GetObjectItem(root, "loggingMotorSteps");
  bool logging_steps_set = false;
  int logging_motor_steps = app_config.logging_motor_steps;
  if (logging_steps_item && cJSON_IsNumber(logging_steps_item)) {
    logging_motor_steps = logging_steps_item->valueint;
    logging_steps_set = true;
  }
  cJSON* home_each_item = cJSON_GetObjectItem(root, "loggingHomeEachCycle");
  bool home_each_set = false;
  bool logging_home_each_cycle = app_config.logging_home_each_cycle;
  if (home_each_item && cJSON_IsBool(home_each_item)) {
    logging_home_each_cycle = cJSON_IsTrue(home_each_item);
    home_each_set = true;
  }
  cJSON* hall_item = cJSON_GetObjectItem(root, "hallActiveLevel");
  if (!hall_item) {
    hall_item = cJSON_GetObjectItem(root, "motorHallActiveLevel");
  }
  bool hall_active_set = false;
  int hall_active_level = app_config.motor_hall_active_level;
  if (hall_item && cJSON_IsNumber(hall_item)) {
    hall_active_level = hall_item->valueint ? 1 : 0;
    hall_active_set = true;
  }
  cJSON_Delete(root);

  StepperHomeOffsetRequest action_req;
  action_req.offset_steps = offset_steps;
  action_req.speed_us = speed_us;
  action_req.logging_motor_steps = logging_motor_steps;
  action_req.logging_motor_steps_set = logging_steps_set;
  action_req.logging_home_each_cycle = logging_home_each_cycle;
  action_req.logging_home_each_cycle_set = home_each_set;
  action_req.hall_active_level = hall_active_level;
  action_req.hall_active_level_set = hall_active_set;
  ActionResult res = ActionStepperHomeOffset(action_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  char resp[240];
  std::snprintf(resp, sizeof(resp),
                "{\"status\":\"stepper_settings_saved\",\"speedUs\":%d,\"offsetSteps\":%d,"
                "\"loggingMotorSteps\":%d,\"loggingHomeEachCycle\":%s,\"hallActiveLevel\":%d}",
                app_config.stepper_speed_us,
                app_config.stepper_home_offset_steps,
                app_config.logging_motor_steps,
                app_config.logging_home_each_cycle ? "true" : "false",
                app_config.motor_hall_active_level);
  return httpd_resp_sendstr(req, resp);
}

esp_err_t StepperStopHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperStop();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"movement_stopped\"}");
}

esp_err_t StepperZeroHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperZero();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"position_zeroed\"}");
}


esp_err_t StepperFindZeroHandler(httpd_req_t* req) {
  ActionResult res = ActionStepperFindZero();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"homing_started\"}");
}

esp_err_t HeaterSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  float power = 0.0f;
  cJSON* power_item = cJSON_GetObjectItem(root, "power");
  if (power_item && cJSON_IsNumber(power_item)) {
    power = static_cast<float>(power_item->valuedouble);
  } else {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing power");
    return ESP_FAIL;
  }
  cJSON_Delete(root);

  ActionResult res = ActionHeaterSet(power);

  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t FanSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* val = cJSON_GetObjectItem(root, "power");
  if (!val || !cJSON_IsNumber(val)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing power");
    return ESP_FAIL;
  }
  float p = static_cast<float>(val->valuedouble);
  cJSON_Delete(root);

  ActionResult res = ActionFanSet(p);

  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t LogStartHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 96);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  const cJSON* filename_item = cJSON_GetObjectItem(root, "filename");
  std::string filename = filename_item && cJSON_IsString(filename_item) && filename_item->valuestring
                             ? filename_item->valuestring
                             : "";
  cJSON* use_motor_item = cJSON_GetObjectItem(root, "useMotor");
  cJSON* duration_item = cJSON_GetObjectItem(root, "durationSec");
  bool use_motor = (use_motor_item && cJSON_IsBool(use_motor_item)) ? cJSON_IsTrue(use_motor_item) : false;
  float duration_s =
      (duration_item && cJSON_IsNumber(duration_item) && duration_item->valuedouble > 0.1) ? duration_item->valuedouble : 1.0f;
  cJSON_Delete(root);

  log_config.use_motor = use_motor;
  log_config.duration_s = duration_s;

  LogRequest log_req{filename, use_motor, duration_s};
  ActionResult res = ActionStartLog(log_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"logging_started\"}");
}

esp_err_t LogStopHandler(httpd_req_t* req) {
  ActionResult res = ActionStopLog();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"logging_stopped\"}");
}

esp_err_t FsListHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();

  std::string rel_path;
  int page = 0;
  int page_size = 10;
  int qs_len = httpd_req_get_url_query_len(req) + 1;
  if (qs_len > 1) {
    std::string qs(qs_len, '\0');
    if (httpd_req_get_url_query_str(req, qs.data(), qs_len) == ESP_OK) {
      char buf[256] = {};
      if (httpd_query_key_value(qs.c_str(), "path", buf, sizeof(buf)) == ESP_OK) {
        std::string decoded;
        if (!UrlDecode(buf, &decoded)) {
          httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
          return ESP_FAIL;
        }
        rel_path = decoded;
      }
      if (httpd_query_key_value(qs.c_str(), "page", buf, sizeof(buf)) == ESP_OK) {
        page = std::max(0, atoi(buf));
      }
      if (httpd_query_key_value(qs.c_str(), "pageSize", buf, sizeof(buf)) == ESP_OK) {
        int p = atoi(buf);
        if (p > 0 && p <= 100) {
          page_size = p;
        }
      }
    }
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }

  std::string full_dir = CONFIG_MOUNT_POINT;
  if (!rel_path.empty()) {
    if (!SanitizePath(rel_path, &full_dir)) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
  }
  DIR* dir = opendir(full_dir.c_str());
  if (!dir) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open dir failed");
    return ESP_FAIL;
  }

  struct Entry {
    PsramString name;
    uint64_t size;
    bool is_dir;
  };
  PsramVector<Entry> entries;
  entries.reserve(32);
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    const bool maybe_dir = ent->d_type == DT_DIR;
    if (ent->d_type != DT_REG && ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
    std::string full_path = full_dir + "/" + ent->d_name;
    struct stat st {};
    uint64_t size = 0;
    if (stat(full_path.c_str(), &st) == 0) {
      size = static_cast<uint64_t>(st.st_size);
    }
    bool is_dir = maybe_dir || S_ISDIR(st.st_mode);
    entries.push_back({ToPsramString(ent->d_name), size, is_dir});
  }
  closedir(dir);

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  int total = static_cast<int>(entries.size());
  int total_pages = (total + page_size - 1) / page_size;
  int start = page * page_size;
  if (start >= total) {
    page = 0;
    start = 0;
  }

  PsramString json;
  json.reserve(256 + static_cast<size_t>(std::min(page_size, total)) * 128);
  json += "{\"path\":";
  AppendJsonEscaped(json, rel_path.c_str());
  json += ",\"page\":";
  AppendJsonNumber(json, page);
  json += ",\"pageSize\":";
  AppendJsonNumber(json, page_size);
  json += ",\"total\":";
  AppendJsonNumber(json, total);
  json += ",\"totalPages\":";
  AppendJsonNumber(json, total_pages);
  json += ",\"entries\":[";
  for (int i = start; i < total && i < start + page_size; ++i) {
    const Entry& e = entries[i];
    if (i > start) json.push_back(',');
    PsramString child_path = rel_path.empty() ? e.name : ToPsramString(rel_path);
    if (!rel_path.empty()) {
      child_path.push_back('/');
      child_path += e.name;
    }
    json += "{\"name\":";
    AppendJsonEscaped(json, e.name);
    json += ",\"type\":";
    AppendJsonEscaped(json, e.is_dir ? "dir" : "file");
    json += ",\"size\":";
    AppendJsonNumber(json, e.size);
    json += ",\"path\":";
    AppendJsonEscaped(json, child_path);
    json.push_back('}');
  }
  json += "]}";
  return SendPsramJson(req, json);
}

esp_err_t FsDownloadHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();

  int qs_len = httpd_req_get_url_query_len(req) + 1;
  if (qs_len <= 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
  }
  std::string qs(qs_len, '\0');
  if (httpd_req_get_url_query_str(req, qs.data(), qs_len) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
    return ESP_FAIL;
  }
  char file_param[256] = {};
  bool has_path = httpd_query_key_value(qs.c_str(), "path", file_param, sizeof(file_param)) == ESP_OK;
  if (!has_path) {
    if (httpd_query_key_value(qs.c_str(), "file", file_param, sizeof(file_param)) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
      return ESP_FAIL;
    }
  }
  std::string req_path = file_param;
  std::string decoded_path;
  if (!UrlDecode(req_path.c_str(), &decoded_path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    return ESP_FAIL;
  }
  std::string full_path;
  if (has_path) {
    if (!SanitizePath(decoded_path, &full_path)) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
  } else if (!SanitizeFilename(decoded_path, &full_path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    return ESP_FAIL;
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }

  FILE* f = fopen(full_path.c_str(), "rb");
  if (!f) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  const char* ctype = "application/octet-stream";
  size_t len = full_path.size();
  if (len >= 4 && full_path.compare(len - 4, 4, ".csv") == 0) {
    ctype = "text/csv";
  }
  httpd_resp_set_type(req, ctype);
  std::string download_name = decoded_path;
  size_t slash = download_name.find_last_of('/');
  if (slash != std::string::npos) {
    download_name = download_name.substr(slash + 1);
  }
  std::string disp = "attachment; filename=\"";
  disp.append(download_name);
  disp.push_back('"');
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());

  char buf[1024];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t FsDeleteHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();

  std::vector<std::string> requested_files;
  bool has_body_files = false;
  bool saw_body = false;
  const size_t kMaxDeleteBody = 8192;
  if (req->content_len > kMaxDeleteBody) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  if (req->content_len > 0) {
    saw_body = true;
    std::string body(req->content_len, '\0');
    size_t offset = 0;
    while (offset < body.size()) {
      int received = httpd_req_recv(req, body.data() + offset, body.size() - offset);
      if (received <= 0) {
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
          continue;
        }
        break;
      }
      offset += static_cast<size_t>(received);
    }
    body.resize(offset);

    if (!body.empty()) {
      cJSON* root = cJSON_Parse(body.c_str());
      if (root) {
        auto collect = [&](cJSON* arr) {
          if (!arr || !cJSON_IsArray(arr)) return;
          has_body_files = true;
          cJSON* item = nullptr;
          cJSON_ArrayForEach(item, arr) {
            if (cJSON_IsString(item) && item->valuestring) {
              requested_files.emplace_back(item->valuestring);
            }
          }
        };
        if (cJSON_IsArray(root)) {
          collect(root);
        } else {
          collect(cJSON_GetObjectItem(root, "files"));
        }
        cJSON_Delete(root);
      }
    }
  }

  if (saw_body && !has_body_files) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    return ESP_FAIL;
  }

  if (!has_body_files) {
    int qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len <= 1) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
      return ESP_FAIL;
    }
    std::string qs(qs_len, '\0');
    if (httpd_req_get_url_query_str(req, qs.data(), qs_len) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
      return ESP_FAIL;
    }
    char file_param[256] = {};
    if (httpd_query_key_value(qs.c_str(), "path", file_param, sizeof(file_param)) != ESP_OK &&
        httpd_query_key_value(qs.c_str(), "file", file_param, sizeof(file_param)) != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
      return ESP_FAIL;
    }
    std::string decoded;
    if (!UrlDecode(file_param, &decoded)) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
    requested_files.emplace_back(decoded);
  }

  auto is_protected_config = [](const std::string& name) {
    if (name.size() != 10) return false;
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower == "config.txt";
  };

  struct DeleteCandidate {
    std::string name;
    std::string full_path;
  };

  std::vector<DeleteCandidate> candidates;
  std::vector<std::string> skipped;
  std::vector<std::string> failed;
  std::set<std::string> seen;

  auto basename = [](const std::string& path) {
    size_t pos = path.find_last_of('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
  };

  for (const auto& raw_name : requested_files) {
    if (!seen.insert(raw_name).second) continue;
    const std::string name_only = basename(raw_name);
    if (is_protected_config(name_only)) {
      skipped.push_back(raw_name + " (protected)");
      continue;
    }

    std::string full_path;
    if (!SanitizePath(raw_name, &full_path) && !SanitizeFilename(raw_name, &full_path)) {
      failed.push_back(raw_name + " (invalid)");
      continue;
    }

    if (snapshot.logging && snapshot.log_filename == name_only) {
      skipped.push_back(raw_name + " (active log)");
      continue;
    }
    candidates.push_back({raw_name, std::move(full_path)});
  }

  auto send_result = [&](const std::vector<std::string>& deleted) -> esp_err_t {
    cJSON* resp = cJSON_CreateObject();
    auto add_array = [&](const char* key, const std::vector<std::string>& values) {
      cJSON* arr = cJSON_CreateArray();
      for (const auto& v : values) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(v.c_str()));
      }
      cJSON_AddItemToObject(resp, key, arr);
    };
    add_array("deleted", deleted);
    add_array("skipped", skipped);
    add_array("failed", failed);
    const char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free((void*)json);
    cJSON_Delete(resp);
    return ESP_OK;
  };

  if (candidates.empty()) {
    return send_result({});
  }

  SdLockGuard guard;
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }

  std::vector<std::string> deleted;
  for (const auto& entry : candidates) {
    if (remove(entry.full_path.c_str()) != 0) {
      failed.push_back(entry.name + " (delete failed)");
    } else {
      deleted.push_back(entry.name);
    }
  }

  return send_result(deleted);
}

esp_err_t UploadedClearHandler(httpd_req_t* req) {
  int max_files = 1000;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
    std::string body(buf_len, '\0');
    int received = httpd_req_recv(req, body.data(), buf_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
      return ESP_FAIL;
    }
    body.resize(received);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
      return ESP_FAIL;
    }
    cJSON* max_item = cJSON_GetObjectItem(root, "maxFiles");
    if (max_item && cJSON_IsNumber(max_item)) {
      max_files = max_item->valueint;
    }
    cJSON_Delete(root);
  }

  ActionResult res = ActionUploadedClear({max_files});
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, res.json.empty() ? "{}" : res.json.c_str());
}

static const char* PartitionTypeName(esp_partition_type_t type) {
  switch (type) {
    case ESP_PARTITION_TYPE_APP:
      return "app";
    case ESP_PARTITION_TYPE_DATA:
      return "data";
    case ESP_PARTITION_TYPE_BOOTLOADER:
      return "bootloader";
    case ESP_PARTITION_TYPE_PARTITION_TABLE:
      return "partition_table";
    default:
      return "unknown";
  }
}

static const char* PartitionSubtypeName(esp_partition_type_t type, esp_partition_subtype_t subtype) {
  if (type == ESP_PARTITION_TYPE_APP) {
    if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
    if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return "ota";
    if (subtype == ESP_PARTITION_SUBTYPE_APP_TEST) return "test";
  }
  if (type == ESP_PARTITION_TYPE_DATA) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_OTA:
        return "ota";
      case ESP_PARTITION_SUBTYPE_DATA_PHY:
        return "phy";
      case ESP_PARTITION_SUBTYPE_DATA_NVS:
        return "nvs";
      case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
        return "coredump";
      case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
        return "nvs_keys";
      case ESP_PARTITION_SUBTYPE_DATA_FAT:
        return "fat";
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
        return "spiffs";
      case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
        return "littlefs";
      default:
        break;
    }
  }
  return "unknown";
}

static bool ValidateInternalFlashRelPath(const std::string& raw, std::string* rel_out, std::string* full_out) {
  std::string rel = raw;
  if (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
  if (rel.size() > 256) return false;
  if (rel.find("..") != std::string::npos || rel.find("//") != std::string::npos) return false;
  for (char c : rel) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ' ||
          c == '(' || c == ')')) {
      return false;
    }
  }
  if (rel_out) *rel_out = rel;
  if (full_out) {
    *full_out = std::string(INTERNAL_FLASH_MOUNT_POINT);
    if (!rel.empty()) {
      *full_out += "/";
      *full_out += rel;
    }
  }
  return true;
}

esp_err_t FlashListHandler(httpd_req_t* req) {
  std::string rel_path;
  int page = 0;
  int page_size = 10;
  int qs_len = httpd_req_get_url_query_len(req) + 1;
  if (qs_len > 1) {
    std::string qs(qs_len, '\0');
    if (httpd_req_get_url_query_str(req, qs.data(), qs_len) == ESP_OK) {
      char buf[256] = {};
      if (httpd_query_key_value(qs.c_str(), "path", buf, sizeof(buf)) == ESP_OK) {
        std::string decoded;
        if (!UrlDecode(buf, &decoded) || !ValidateInternalFlashRelPath(decoded, &rel_path, nullptr)) {
          httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
          return ESP_FAIL;
        }
      }
      if (httpd_query_key_value(qs.c_str(), "page", buf, sizeof(buf)) == ESP_OK) {
        page = std::max(0, atoi(buf));
      }
      if (httpd_query_key_value(qs.c_str(), "pageSize", buf, sizeof(buf)) == ESP_OK) {
        int p = atoi(buf);
        if (p > 0 && p <= 100) page_size = p;
      }
    }
  }

  uint32_t flash_size = 0;
  const bool has_flash_size = esp_flash_get_size(nullptr, &flash_size) == ESP_OK;

  nvs_stats_t nvs_stats {};
  const bool has_nvs_stats = nvs_get_stats(nullptr, &nvs_stats) == ESP_OK;

  struct FlashEntry {
    PsramString name;
    PsramString type;
    PsramString path;
    PsramString area;
    PsramString part_type;
    PsramString subtype;
    uint64_t size = 0;
    uint32_t offset = 0;
    bool downloadable = false;
  };
  PsramVector<FlashEntry> all_entries;
  all_entries.reserve(32);
  std::string internal_config;
  if (rel_path.empty() && LoadConfigTextFromInternalFlash(&internal_config)) {
    all_entries.push_back({ToPsramString("config.txt"), ToPsramString("file"), ToPsramString("config.txt"),
                           ToPsramString("NVS cfg/config_txt"), PsramString(), PsramString(),
                           static_cast<uint64_t>(internal_config.size()), 0, true});
  }

  bool internal_fs_mounted = false;
  bool has_internal_fs_stats = false;
  uint64_t internal_fs_total = 0;
  uint64_t internal_fs_used = 0;
  {
    SdLockGuard guard(pdMS_TO_TICKS(500));
    if (guard.locked() && MountInternalFlashFs()) {
      internal_fs_mounted = true;
      struct statvfs fs {};
      FsOps ops = DefaultFsOps();
      if (ops.statvfs_fn(INTERNAL_FLASH_MOUNT_POINT, &fs) == 0 && fs.f_blocks > 0) {
        const uint64_t total = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
        const uint64_t avail = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
        internal_fs_total = total;
        internal_fs_used = total > avail ? total - avail : 0;
        has_internal_fs_stats = true;
      }

      std::string full_dir;
      if (!ValidateInternalFlashRelPath(rel_path, nullptr, &full_dir)) {
        full_dir = INTERNAL_FLASH_MOUNT_POINT;
      }
      DIR* dir = opendir(full_dir.c_str());
      if (dir) {
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
          if (ent->d_name[0] == '.') continue;
          if (ent->d_type != DT_REG && ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
          std::string full = full_dir + "/" + ent->d_name;
          struct stat st {};
          uint64_t size = 0;
          bool is_dir = ent->d_type == DT_DIR;
          if (stat(full.c_str(), &st) == 0) {
            size = static_cast<uint64_t>(st.st_size);
            is_dir = S_ISDIR(st.st_mode);
          }
          std::string child_path = rel_path.empty() ? ent->d_name : (rel_path + "/" + ent->d_name);
          all_entries.push_back({ToPsramString(ent->d_name), ToPsramString(is_dir ? "dir" : "file"),
                                 ToPsramString(child_path), ToPsramString("internal FAT /flashfs"), PsramString(), PsramString(),
                                 size, 0, !is_dir});
        }
        closedir(dir);
      }
    }
  }

  uint64_t partition_bytes = 0;
  {
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
      const esp_partition_t* part = esp_partition_get(it);
      if (part) {
        partition_bytes += part->size;
        if (rel_path.empty()) {
          all_entries.push_back({ToPsramString(part->label), ToPsramString("partition"), ToPsramString(part->label),
                                 PsramString(), ToPsramString(PartitionTypeName(part->type)),
                                 ToPsramString(PartitionSubtypeName(part->type, part->subtype)), part->size,
                                 part->address, false});
        }
      }
      it = esp_partition_next(it);
    }
  }
  std::sort(all_entries.begin(), all_entries.end(), [](const FlashEntry& a, const FlashEntry& b) {
    if (a.type != b.type) return a.type < b.type;
    return a.name < b.name;
  });
  const int total = static_cast<int>(all_entries.size());
  const int total_pages = (total + page_size - 1) / page_size;
  int start = page * page_size;
  if (start >= total) {
    page = 0;
    start = 0;
  }
  PsramString json;
  json.reserve(512 + static_cast<size_t>(std::min(page_size, total)) * 192);
  json.push_back('{');
  bool first_field = true;
  auto comma = [&]() {
    if (first_field) {
      first_field = false;
    } else {
      json.push_back(',');
    }
  };
  if (has_flash_size) {
    comma();
    json += "\"flashTotalBytes\":";
    AppendJsonNumber(json, static_cast<uint64_t>(flash_size));
  }
  if (has_nvs_stats) {
    comma();
    json += "\"nvs\":{\"usedEntries\":";
    AppendJsonNumber(json, static_cast<int>(nvs_stats.used_entries));
    json += ",\"freeEntries\":";
    AppendJsonNumber(json, static_cast<int>(nvs_stats.free_entries));
    json += ",\"availableEntries\":";
    AppendJsonNumber(json, static_cast<int>(nvs_stats.available_entries));
    json += ",\"totalEntries\":";
    AppendJsonNumber(json, static_cast<int>(nvs_stats.total_entries));
    json += ",\"namespaceCount\":";
    AppendJsonNumber(json, static_cast<int>(nvs_stats.namespace_count));
    json.push_back('}');
  }
  comma();
  json += "\"storageBackend\":";
  AppendJsonEscaped(json, StorageBackendToString(app_config.storage_backend).c_str());
  if (has_internal_fs_stats) {
    comma();
    json += "\"internalFsTotalBytes\":";
    AppendJsonNumber(json, internal_fs_total);
    comma();
    json += "\"internalFsUsedBytes\":";
    AppendJsonNumber(json, internal_fs_used);
  }
  comma();
  json += "\"internalFsMounted\":";
  AppendJsonBool(json, internal_fs_mounted);
  comma();
  json += "\"path\":";
  AppendJsonEscaped(json, rel_path.c_str());
  comma();
  json += "\"page\":";
  AppendJsonNumber(json, page);
  comma();
  json += "\"pageSize\":";
  AppendJsonNumber(json, page_size);
  comma();
  json += "\"total\":";
  AppendJsonNumber(json, total);
  comma();
  json += "\"totalPages\":";
  AppendJsonNumber(json, total_pages);
  comma();
  json += "\"partitionBytes\":";
  AppendJsonNumber(json, partition_bytes);
  comma();
  json += "\"entries\":[";
  for (int i = start; i < total && i < start + page_size; ++i) {
    const FlashEntry& e = all_entries[i];
    if (i > start) json.push_back(',');
    json += "{\"name\":";
    AppendJsonEscaped(json, e.name);
    json += ",\"type\":";
    AppendJsonEscaped(json, e.type);
    json += ",\"path\":";
    AppendJsonEscaped(json, e.path);
    if (!e.area.empty()) {
      json += ",\"area\":";
      AppendJsonEscaped(json, e.area);
    }
    if (!e.part_type.empty()) {
      json += ",\"partType\":";
      AppendJsonEscaped(json, e.part_type);
    }
    if (!e.subtype.empty()) {
      json += ",\"subtype\":";
      AppendJsonEscaped(json, e.subtype);
    }
    if (e.offset > 0) {
      json += ",\"offset\":";
      AppendJsonNumber(json, static_cast<uint64_t>(e.offset));
    }
    json += ",\"size\":";
    AppendJsonNumber(json, e.size);
    json += ",\"downloadable\":";
    AppendJsonBool(json, e.downloadable);
    json.push_back('}');
  }
  json += "]}";
  return SendPsramJson(req, json);
}

esp_err_t FlashDownloadHandler(httpd_req_t* req) {
  int qs_len = httpd_req_get_url_query_len(req) + 1;
  if (qs_len <= 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
  }
  std::string qs(qs_len, '\0');
  if (httpd_req_get_url_query_str(req, qs.data(), qs_len) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad query");
    return ESP_FAIL;
  }
  char path_param[256] = {};
  if (httpd_query_key_value(qs.c_str(), "path", path_param, sizeof(path_param)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
    return ESP_FAIL;
  }
  std::string decoded;
  if (!UrlDecode(path_param, &decoded)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid flash object");
    return ESP_FAIL;
  }
  if (decoded == "config.txt") {
    std::string text;
    if (!LoadConfigTextFromInternalFlash(&text)) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Config not found in internal flash");
      return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"internal_config.txt\"");
    return httpd_resp_send(req, text.c_str(), text.size());
  }

  std::string rel = decoded;
  if (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
  if (rel.empty() || rel.find("..") != std::string::npos || rel.find("//") != std::string::npos) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    return ESP_FAIL;
  }
  for (char c : rel) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ' ||
          c == '(' || c == ')')) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
      return ESP_FAIL;
    }
  }

  SdLockGuard guard(pdMS_TO_TICKS(1000));
  if (!guard.locked() || !MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash busy");
    return ESP_FAIL;
  }
  std::string full_path = std::string(INTERNAL_FLASH_MOUNT_POINT) + "/" + rel;
  FILE* f = fopen(full_path.c_str(), "rb");
  if (!f) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/octet-stream");
  std::string name = rel;
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  std::string disp = "attachment; filename=\"" + name + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
  char chunk[1024];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

static esp_err_t SendFlashDeleteResult(httpd_req_t* req,
                                       const std::vector<std::string>& deleted,
                                       const std::vector<std::string>& skipped,
                                       const std::vector<std::string>& failed) {
  cJSON* resp = cJSON_CreateObject();
  auto add_array = [&](const char* key, const std::vector<std::string>& values) {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& v : values) {
      cJSON_AddItemToArray(arr, cJSON_CreateString(v.c_str()));
    }
    cJSON_AddItemToObject(resp, key, arr);
  };
  add_array("deleted", deleted);
  add_array("skipped", skipped);
  add_array("failed", failed);
  const char* json = cJSON_PrintUnformatted(resp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(resp);
  return ESP_OK;
}

esp_err_t FlashDeleteHandler(httpd_req_t* req) {
  std::vector<std::string> requested_files;
  const bool saw_body = req->content_len > 0;
  bool has_body_files = false;
  if (saw_body) {
    const size_t buf_len = std::min<size_t>(req->content_len, 2048);
    std::string body(buf_len, '\0');
    int received = httpd_req_recv(req, body.data(), buf_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
      return ESP_FAIL;
    }
    body.resize(received);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root) {
      auto collect = [&](cJSON* arr) {
        if (!arr || !cJSON_IsArray(arr)) return;
        has_body_files = true;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, arr) {
          if (cJSON_IsString(item) && item->valuestring) {
            requested_files.emplace_back(item->valuestring);
          }
        }
      };
      if (cJSON_IsArray(root)) {
        collect(root);
      } else {
        collect(cJSON_GetObjectItem(root, "files"));
      }
      cJSON_Delete(root);
    }
  }
  if (saw_body && !has_body_files) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    return ESP_FAIL;
  }
  if (requested_files.empty()) {
    return SendFlashDeleteResult(req, {}, {}, {});
  }

  std::vector<std::string> deleted;
  std::vector<std::string> skipped;
  std::vector<std::string> failed;
  std::set<std::string> seen;
  SdLockGuard guard(pdMS_TO_TICKS(2000));
  if (!guard.locked() || !MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash busy");
    return ESP_FAIL;
  }

  for (const auto& raw : requested_files) {
    if (!seen.insert(raw).second) continue;
    std::string rel;
    std::string full;
    if (!ValidateInternalFlashRelPath(raw, &rel, &full) || rel.empty()) {
      failed.push_back(raw + " (invalid)");
      continue;
    }
    const std::string base = rel.substr(rel.find_last_of('/') == std::string::npos ? 0 : rel.find_last_of('/') + 1);
    if (base == "config.txt") {
      skipped.push_back(raw + " (protected)");
      continue;
    }
    if (full == current_log_path) {
      skipped.push_back(raw + " (active log)");
      continue;
    }
    struct stat st {};
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      skipped.push_back(raw + " (not a file)");
      continue;
    }
    if (remove(full.c_str()) == 0) {
      deleted.push_back(raw);
    } else {
      failed.push_back(raw + " (delete failed)");
    }
  }
  return SendFlashDeleteResult(req, deleted, skipped, failed);
}

esp_err_t FlashClearUploadedHandler(httpd_req_t* req) {
  int max_files = 1000;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
    std::string body(buf_len, '\0');
    int received = httpd_req_recv(req, body.data(), buf_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
      return ESP_FAIL;
    }
    body.resize(received);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
      return ESP_FAIL;
    }
    cJSON* max_item = cJSON_GetObjectItem(root, "maxFiles");
    if (max_item && cJSON_IsNumber(max_item)) {
      max_files = max_item->valueint;
    }
    cJSON_Delete(root);
  }
  max_files = std::clamp(max_files > 0 ? max_files : 1000, 1, 1000);

  int scanned = 0;
  int deleted_count = 0;
  int failed_count = 0;
  SdLockGuard guard(pdMS_TO_TICKS(2000));
  if (!guard.locked() || !MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash busy");
    return ESP_FAIL;
  }
  const std::string uploaded_dir = std::string(INTERNAL_FLASH_MOUNT_POINT) + "/uploaded";
  (void)EnsureDirExists(uploaded_dir.c_str());
  DIR* dir = opendir(uploaded_dir.c_str());
  if (dir) {
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr && scanned < max_files) {
      if (ent->d_name[0] == '.') continue;
      if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
      const std::string full = uploaded_dir + "/" + ent->d_name;
      struct stat st {};
      if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      scanned++;
      if (remove(full.c_str()) == 0) {
        deleted_count++;
      } else {
        failed_count++;
      }
    }
    closedir(dir);
  }

  cJSON* resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "status", "flash_uploaded_cleared");
  cJSON_AddNumberToObject(resp, "scanned", scanned);
  cJSON_AddNumberToObject(resp, "deleted", deleted_count);
  cJSON_AddNumberToObject(resp, "failed", failed_count);
  cJSON_AddNumberToObject(resp, "maxFiles", max_files);
  const char* json = cJSON_PrintUnformatted(resp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(resp);
  return ESP_OK;
}

struct UploadQueueTransferResult {
  int scanned = 0;
  int moved = 0;
  int skipped = 0;
  int failed = 0;
};

static std::string ToUploadDirForBackend(StorageBackend backend) {
  return backend == StorageBackend::kInternalFlash
             ? std::string(INTERNAL_FLASH_MOUNT_POINT) + "/to_upload"
             : std::string(TO_UPLOAD_DIR);
}

static const char* StorageName(StorageBackend backend) {
  return backend == StorageBackend::kInternalFlash ? "internal_flash" : "sd";
}

static bool CopyFileThenRemove(const std::string& src, const std::string& dst) {
  FILE* in = fopen(src.c_str(), "rb");
  if (!in) {
    ESP_LOGW(kTag, "Transfer open source failed %s errno=%d", src.c_str(), errno);
    return false;
  }
  FILE* out = fopen(dst.c_str(), "wb");
  if (!out) {
    ESP_LOGW(kTag, "Transfer open destination failed %s errno=%d", dst.c_str(), errno);
    fclose(in);
    return false;
  }
  char buf[1024];
  bool ok = true;
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  if (ferror(in)) ok = false;
  if (fflush(out) != 0) ok = false;
  fclose(out);
  fclose(in);
  if (!ok) {
    remove(dst.c_str());
    return false;
  }
  if (remove(src.c_str()) != 0) {
    ESP_LOGW(kTag, "Transfer remove source failed %s errno=%d", src.c_str(), errno);
    return false;
  }
  return true;
}

static esp_err_t SendTransferResult(httpd_req_t* req,
                                    StorageBackend src_backend,
                                    StorageBackend dst_backend,
                                    const UploadQueueTransferResult& result) {
  cJSON* resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "status", "ok");
  cJSON_AddStringToObject(resp, "source", StorageName(src_backend));
  cJSON_AddStringToObject(resp, "destination", StorageName(dst_backend));
  cJSON_AddNumberToObject(resp, "scanned", result.scanned);
  cJSON_AddNumberToObject(resp, "moved", result.moved);
  cJSON_AddNumberToObject(resp, "skipped", result.skipped);
  cJSON_AddNumberToObject(resp, "failed", result.failed);
  const char* json = cJSON_PrintUnformatted(resp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(resp);
  return ESP_OK;
}

static esp_err_t TransferUploadQueueHandler(httpd_req_t* req, StorageBackend src_backend, StorageBackend dst_backend) {
  if (src_backend == dst_backend) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Source and destination are the same");
    return ESP_FAIL;
  }
  if (log_config.active || CopyState().logging || log_file != nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stop logging before transfer");
    return ESP_FAIL;
  }
  int max_files = 10;
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len > 0) {
    std::string body(buf_len, '\0');
    int received = httpd_req_recv(req, body.data(), buf_len);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
      return ESP_FAIL;
    }
    body.resize(received);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
      return ESP_FAIL;
    }
    cJSON* max_item = cJSON_GetObjectItem(root, "maxFiles");
    if (max_item && cJSON_IsNumber(max_item)) {
      max_files = max_item->valueint;
    }
    cJSON_Delete(root);
  }
  max_files = std::clamp(max_files > 0 ? max_files : 10, 1, 25);

  SdLockGuard guard(pdMS_TO_TICKS(10000));
  if (!guard.locked()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage busy");
    return ESP_FAIL;
  }
  if (!MountLogSd()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD mount failed");
    return ESP_FAIL;
  }
  if (!MountInternalFlashFs()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal flash mount failed");
    return ESP_FAIL;
  }

  const std::string src_dir = ToUploadDirForBackend(src_backend);
  const std::string dst_dir = ToUploadDirForBackend(dst_backend);
  (void)EnsureDirExists(src_dir.c_str());
  if (!EnsureDirExists(dst_dir.c_str())) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Destination to_upload unavailable");
    return ESP_FAIL;
  }

  UploadQueueTransferResult result;
  DIR* dir = opendir(src_dir.c_str());
  if (!dir) {
    return SendTransferResult(req, src_backend, dst_backend, result);
  }

  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr && result.scanned < max_files) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    const std::string src = src_dir + "/" + ent->d_name;
    const std::string dst = dst_dir + "/" + ent->d_name;
    struct stat src_st {};
    if (stat(src.c_str(), &src_st) != 0 || !S_ISREG(src_st.st_mode)) {
      continue;
    }
    result.scanned++;
    struct stat dst_st {};
    if (stat(dst.c_str(), &dst_st) == 0) {
      result.skipped++;
      continue;
    }
    if (CopyFileThenRemove(src, dst)) {
      result.moved++;
    } else {
      result.failed++;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  closedir(dir);
  return SendTransferResult(req, src_backend, dst_backend, result);
}

esp_err_t TransferFlashToSdHandler(httpd_req_t* req) {
  return TransferUploadQueueHandler(req, StorageBackend::kInternalFlash, StorageBackend::kSd);
}

esp_err_t TransferSdToFlashHandler(httpd_req_t* req) {
  return TransferUploadQueueHandler(req, StorageBackend::kSd, StorageBackend::kInternalFlash);
}

esp_err_t PidApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 128);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  auto get_number = [&](const char* key, float* out) -> bool {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsNumber(item)) {
      *out = static_cast<float>(item->valuedouble);
      return true;
    }
    return false;
  };
  float kp = pid_config.kp;
  float ki = pid_config.ki;
  float kd = pid_config.kd;
  float sp = pid_config.setpoint;
  int sensor = pid_config.sensor_index;
  uint16_t sensor_mask = pid_config.sensor_mask;
  bool sensor_mask_set = false;

  get_number("kp", &kp);
  get_number("ki", &ki);
  get_number("kd", &kd);
  get_number("setpoint", &sp);
  cJSON* sensor_item = cJSON_GetObjectItem(root, "sensor");
  if (sensor_item && cJSON_IsNumber(sensor_item)) {
    sensor = sensor_item->valueint;
  }
  cJSON* mask_item = cJSON_GetObjectItem(root, "sensorMask");
  if (mask_item && cJSON_IsNumber(mask_item)) {
    sensor_mask = static_cast<uint16_t>(mask_item->valuedouble);
    sensor_mask_set = true;
  }
  cJSON* sensors_item = cJSON_GetObjectItem(root, "sensors");
  if (sensors_item && cJSON_IsArray(sensors_item)) {
    sensor_mask = 0;
    const int len = cJSON_GetArraySize(sensors_item);
    for (int i = 0; i < len; ++i) {
      cJSON* entry = cJSON_GetArrayItem(sensors_item, i);
      if (!entry || !cJSON_IsNumber(entry)) continue;
      int idx = entry->valueint;
      if (idx < 0 || idx >= MAX_TEMP_SENSORS) continue;
      sensor_mask = static_cast<uint16_t>(sensor_mask | (1u << idx));
    }
    sensor_mask_set = true;
  }
  cJSON_Delete(root);

  PidApplyRequest action_req;
  action_req.kp = kp;
  action_req.ki = ki;
  action_req.kd = kd;
  action_req.setpoint = sp;
  action_req.sensor = sensor;
  action_req.sensor_mask = sensor_mask;
  action_req.sensor_mask_set = sensor_mask_set;
  ActionResult res = ActionPidApply(action_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"pid_applied\"}");
}

esp_err_t PidEnableHandler(httpd_req_t* req) {
  ActionResult res = ActionPidEnable();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"pid_enabled\"}");
}

esp_err_t PidDisableHandler(httpd_req_t* req) {
  ActionResult res = ActionPidDisable();
  httpd_resp_set_type(req, "application/json");
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"status\":\"pid_disabled\"}");
}

esp_err_t WifiApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 160);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* mode_item = cJSON_GetObjectItem(root, "mode");
  cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
  cJSON* pass_item = cJSON_GetObjectItem(root, "password");
  std::string mode = (mode_item && cJSON_IsString(mode_item) && mode_item->valuestring) ? mode_item->valuestring : "sta";
  std::string ssid = (ssid_item && cJSON_IsString(ssid_item) && ssid_item->valuestring) ? ssid_item->valuestring : "";
  std::string pass = (pass_item && cJSON_IsString(pass_item) && pass_item->valuestring) ? pass_item->valuestring : "";
  cJSON_Delete(root);

  ActionResult res = ActionWifiApply({mode, ssid, pass});
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"wifi_applied\"}");
}

esp_err_t NetApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 384);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* mode_item = cJSON_GetObjectItem(root, "mode");
  cJSON* prio_item = cJSON_GetObjectItem(root, "priority");
  cJSON* dhcp_item = cJSON_GetObjectItem(root, "ethDhcp");
  cJSON* ip_item = cJSON_GetObjectItem(root, "ethIp");
  cJSON* netmask_item = cJSON_GetObjectItem(root, "ethNetmask");
  cJSON* gateway_item = cJSON_GetObjectItem(root, "ethGateway");
  cJSON* dns_item = cJSON_GetObjectItem(root, "ethDns");
  std::string mode = (mode_item && cJSON_IsString(mode_item) && mode_item->valuestring) ? mode_item->valuestring : "";
  std::string priority =
      (prio_item && cJSON_IsString(prio_item) && prio_item->valuestring) ? prio_item->valuestring : "";
  const bool eth_dhcp = !dhcp_item || !cJSON_IsBool(dhcp_item) || cJSON_IsTrue(dhcp_item);
  std::string eth_ip = (ip_item && cJSON_IsString(ip_item) && ip_item->valuestring) ? ip_item->valuestring : "";
  std::string eth_netmask =
      (netmask_item && cJSON_IsString(netmask_item) && netmask_item->valuestring) ? netmask_item->valuestring : "";
  std::string eth_gateway =
      (gateway_item && cJSON_IsString(gateway_item) && gateway_item->valuestring) ? gateway_item->valuestring : "";
  std::string eth_dns = (dns_item && cJSON_IsString(dns_item) && dns_item->valuestring) ? dns_item->valuestring : "";
  cJSON_Delete(root);

  ActionResult res = ActionNetApply({mode, priority, eth_dhcp, eth_ip, eth_netmask, eth_gateway, eth_dns});
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"net_applied\"}");
}

esp_err_t CloudApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 512);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  auto get_str = [&](const char* key) -> std::string {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsString(item) && item->valuestring) ? std::string(item->valuestring) : std::string();
  };
  auto get_bool = [&](const char* key, bool def) -> bool {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (item && cJSON_IsNumber(item)) return item->valueint != 0;
    return def;
  };
  CloudApplyRequest action_req;
  action_req.device_id = get_str("deviceId");
  action_req.minio_endpoint = get_str("minioEndpoint");
  action_req.minio_access_key = get_str("minioAccessKey");
  action_req.minio_secret_key = get_str("minioSecretKey");
  action_req.minio_bucket = get_str("minioBucket");
  action_req.minio_enabled = get_bool("minioEnabled", app_config.minio_enabled);
  action_req.mqtt_uri = get_str("mqttUri");
  action_req.mqtt_user = get_str("mqttUser");
  action_req.mqtt_password = get_str("mqttPassword");
  action_req.mqtt_enabled = get_bool("mqttEnabled", app_config.mqtt_enabled);
  cJSON_Delete(root);

  ActionResult res = ActionCloudApply(action_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, res.message.c_str());
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"cloud_applied\"}");
}

esp_err_t MeteoConfigApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 256);
  if (buf_len == 0 || req->content_len > 256) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  size_t received_total = 0;
  while (received_total < buf_len) {
    const int received = httpd_req_recv(req, body.data() + received_total, buf_len - received_total);
    if (received <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
      return ESP_FAIL;
    }
    received_total += static_cast<size_t>(received);
  }

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* poll_item = cJSON_GetObjectItem(root, "pollIntervalS");
  cJSON* file_item = cJSON_GetObjectItem(root, "fileIntervalS");
  const bool valid_numbers = poll_item && file_item && cJSON_IsNumber(poll_item) && cJSON_IsNumber(file_item) &&
                             poll_item->valuedouble == poll_item->valueint &&
                             file_item->valuedouble == file_item->valueint;
  MeteoConfigApplyRequest action_req;
  if (valid_numbers) {
    action_req.poll_interval_s = poll_item->valueint;
    action_req.file_interval_s = file_item->valueint;
  }
  cJSON_Delete(root);
  if (!valid_numbers) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Intervals must be integers");
    return ESP_FAIL;
  }
  if (action_req.poll_interval_s < 1 || action_req.poll_interval_s > 3600 ||
      action_req.file_interval_s < 10 || action_req.file_interval_s > 86400) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Intervals are outside the allowed range");
    return ESP_FAIL;
  }

  ActionResult res = ActionMeteoConfigApply(action_req);
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, res.json.c_str());
}

esp_err_t GpsApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 512);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  std::vector<uint16_t> rtcm_types;
  cJSON* types_item = cJSON_GetObjectItem(root, "rtcmTypes");
  if (types_item && cJSON_IsArray(types_item)) {
    const int len = cJSON_GetArraySize(types_item);
    for (int i = 0; i < len; ++i) {
      cJSON* item = cJSON_GetArrayItem(types_item, i);
      if (!item) continue;
      int type = 0;
      if (cJSON_IsNumber(item)) {
        type = item->valueint;
      } else if (cJSON_IsString(item) && item->valuestring) {
        type = std::atoi(item->valuestring);
      }
      if (type > 0 && type <= 4095 &&
          std::find(rtcm_types.begin(), rtcm_types.end(), static_cast<uint16_t>(type)) == rtcm_types.end()) {
        rtcm_types.push_back(static_cast<uint16_t>(type));
      }
    }
  } else if (types_item && cJSON_IsString(types_item) && types_item->valuestring) {
    rtcm_types = ParseRtcmTypesText(types_item->valuestring);
  }
  if (rtcm_types.empty()) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "RTCM type list is empty");
    return ESP_FAIL;
  }

  std::string mode = app_config.gps_mode.empty() ? "base_time_60" : app_config.gps_mode;
  cJSON* mode_item = cJSON_GetObjectItem(root, "mode");
  if (mode_item && cJSON_IsString(mode_item) && mode_item->valuestring) {
    mode = mode_item->valuestring;
  }
  cJSON_Delete(root);

  if (!(mode == "keep" || mode == "base_time_60" || mode == "base" || mode == "rover_uav" || mode == "rover")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GPS mode");
    return ESP_FAIL;
  }
  app_config.gps_rtcm_types = rtcm_types;
  app_config.gps_mode = mode;
  if (!SaveConfigToSdCard(app_config, pid_config)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config.txt");
    return ESP_FAIL;
  }
  RequestGpsReconfigure();
  ProbeGpsMode();

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"gps_saved\",\"reconfigure\":\"queued\"}");
}

esp_err_t GpsProbeHandler(httpd_req_t* req) {
  ProbeGpsMode();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"gps_mode_probe_sent\"}");
}

esp_err_t ConfigSyncInternalFlashHandler(httpd_req_t* req) {
  ActionResult res = ActionConfigSyncInternalFlash();
  if (!res.ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, res.message.c_str());
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"config_synced_to_internal_flash\"}");
}

esp_err_t StorageApplyHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 128);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* item = cJSON_GetObjectItem(root, "backend");
  StorageBackend backend = app_config.storage_backend;
  const bool ok = item && cJSON_IsString(item) && item->valuestring && ParseStorageBackend(item->valuestring, &backend);
  cJSON_Delete(root);
  if (!ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid storage backend");
    return ESP_FAIL;
  }
  if (log_config.active || CopyState().logging || log_file != nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stop logging before switching storage");
    return ESP_FAIL;
  }

  bool restart_required = false;
  if (backend == StorageBackend::kInternalFlash) {
    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked()) {
      restart_required = true;
    } else if (!MountInternalFlashFs()) {
      restart_required = true;
    }
  }

  app_config.storage_backend = backend;
  if (!SaveConfigToSdCard(app_config, pid_config)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  std::string resp = std::string("{\"status\":\"storage_saved\",\"backend\":\"") +
                     StorageBackendToString(app_config.storage_backend) + "\",\"restartRequired\":" +
                     (restart_required ? "true" : "false") + "}";
  return httpd_resp_sendstr(req, resp.c_str());
}

esp_err_t StorageRemountHandler(httpd_req_t* req) {
  if (log_config.active || CopyState().logging || log_file != nullptr) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stop logging before remount");
    return ESP_FAIL;
  }
  bool ok = false;
  {
    SdLockGuard guard(pdMS_TO_TICKS(3000));
    if (!guard.locked()) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage busy");
      return ESP_FAIL;
    }
    if (app_config.storage_backend == StorageBackend::kInternalFlash) {
      UnmountInternalFlashFs();
      ok = MountInternalFlashFs();
    } else {
      UnmountLogSd();
      ok = MountLogSd();
    }
  }

  if (!ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Remount failed");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  std::string resp = std::string("{\"status\":\"storage_remounted\",\"backend\":\"") +
                     StorageBackendToString(app_config.storage_backend) +
                     "\",\"sdMounted\":" + (IsLogSdMounted() ? "true" : "false") +
                     ",\"internalFlashMounted\":" + (IsInternalFlashMounted() ? "true" : "false") + "}";
  return httpd_resp_sendstr(req, resp.c_str());
}

httpd_handle_t StartHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  // Cap the web server's socket pool so it can't monopolize LWIP_MAX_SOCKETS and
  // starve MQTT / SNTP / MinIO upload (default 7 + 3 reserved == the whole pool).
  config.max_open_sockets = 4;
  config.max_uri_handlers = 47;
  config.stack_size = 8192;

  if (httpd_start(&http_server, &config) != ESP_OK) {
    ESP_LOGE(kTag, "Failed to start HTTP server");
    return nullptr;
  }

  httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = RootHandler, .user_ctx = nullptr};
  httpd_uri_t data_uri = {.uri = "/data", .method = HTTP_GET, .handler = DataHandler, .user_ctx = nullptr};
  httpd_uri_t calibrate_uri = {.uri = "/calibrate", .method = HTTP_POST, .handler = CalibrateHandler, .user_ctx = nullptr};
  httpd_uri_t restart_uri = {.uri = "/restart", .method = HTTP_POST, .handler = RestartHandler, .user_ctx = nullptr};
  httpd_uri_t external_power_set_uri = {.uri = "/external_power/set", .method = HTTP_POST, .handler = ExternalPowerSetHandler, .user_ctx = nullptr};
  httpd_uri_t external_power_cycle_uri = {.uri = "/external_power/cycle", .method = HTTP_POST, .handler = ExternalPowerCycleHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_enable_uri = {.uri = "/stepper/enable", .method = HTTP_POST, .handler = StepperEnableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_disable_uri = {.uri = "/stepper/disable", .method = HTTP_POST, .handler = StepperDisableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_move_uri = {.uri = "/stepper/move", .method = HTTP_POST, .handler = StepperMoveHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_stop_uri = {.uri = "/stepper/stop", .method = HTTP_POST, .handler = StepperStopHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_zero_uri = {.uri = "/stepper/zero", .method = HTTP_POST, .handler = StepperZeroHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_find_zero_uri = {.uri = "/stepper/find_zero", .method = HTTP_POST, .handler = StepperFindZeroHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_home_offset_uri = {.uri = "/stepper/home_offset", .method = HTTP_POST, .handler = StepperHomeOffsetHandler, .user_ctx = nullptr};
  httpd_uri_t wifi_apply_uri = {.uri = "/wifi/apply", .method = HTTP_POST, .handler = WifiApplyHandler, .user_ctx = nullptr};
  httpd_uri_t net_apply_uri = {.uri = "/net/apply", .method = HTTP_POST, .handler = NetApplyHandler, .user_ctx = nullptr};
  httpd_uri_t cloud_apply_uri = {.uri = "/cloud/apply", .method = HTTP_POST, .handler = CloudApplyHandler, .user_ctx = nullptr};
  httpd_uri_t meteo_config_apply_uri = {.uri = "/meteo/config", .method = HTTP_POST, .handler = MeteoConfigApplyHandler, .user_ctx = nullptr};
  httpd_uri_t gps_apply_uri = {.uri = "/gps/apply", .method = HTTP_POST, .handler = GpsApplyHandler, .user_ctx = nullptr};
  httpd_uri_t gps_probe_uri = {.uri = "/gps/probe", .method = HTTP_POST, .handler = GpsProbeHandler, .user_ctx = nullptr};
  httpd_uri_t config_sync_internal_uri = {.uri = "/config/sync_internal_flash", .method = HTTP_POST, .handler = ConfigSyncInternalFlashHandler, .user_ctx = nullptr};
  httpd_uri_t storage_apply_uri = {.uri = "/storage/apply", .method = HTTP_POST, .handler = StorageApplyHandler, .user_ctx = nullptr};
  httpd_uri_t storage_remount_uri = {.uri = "/storage/remount", .method = HTTP_POST, .handler = StorageRemountHandler, .user_ctx = nullptr};
  httpd_uri_t heater_set_uri = {.uri = "/heater/set", .method = HTTP_POST, .handler = HeaterSetHandler, .user_ctx = nullptr};
  httpd_uri_t fan_set_uri = {.uri = "/fan/set", .method = HTTP_POST, .handler = FanSetHandler, .user_ctx = nullptr};
  httpd_uri_t log_start_uri = {.uri = "/log/start", .method = HTTP_POST, .handler = LogStartHandler, .user_ctx = nullptr};
  httpd_uri_t log_stop_uri = {.uri = "/log/stop", .method = HTTP_POST, .handler = LogStopHandler, .user_ctx = nullptr};
  httpd_uri_t pid_apply_uri = {.uri = "/pid/apply", .method = HTTP_POST, .handler = PidApplyHandler, .user_ctx = nullptr};
  httpd_uri_t pid_enable_uri = {.uri = "/pid/enable", .method = HTTP_POST, .handler = PidEnableHandler, .user_ctx = nullptr};
  httpd_uri_t pid_disable_uri = {.uri = "/pid/disable", .method = HTTP_POST, .handler = PidDisableHandler, .user_ctx = nullptr};
  httpd_uri_t fs_list_uri = {.uri = "/fs/list", .method = HTTP_GET, .handler = FsListHandler, .user_ctx = nullptr};
  httpd_uri_t fs_download_uri = {.uri = "/fs/download", .method = HTTP_GET, .handler = FsDownloadHandler, .user_ctx = nullptr};
  httpd_uri_t fs_delete_uri = {.uri = "/fs/delete", .method = HTTP_POST, .handler = FsDeleteHandler, .user_ctx = nullptr};
  httpd_uri_t uploaded_clear_uri = {.uri = "/fs/clear_uploaded", .method = HTTP_POST, .handler = UploadedClearHandler, .user_ctx = nullptr};
  httpd_uri_t flash_list_uri = {.uri = "/flash/list", .method = HTTP_GET, .handler = FlashListHandler, .user_ctx = nullptr};
  httpd_uri_t flash_download_uri = {.uri = "/flash/download", .method = HTTP_GET, .handler = FlashDownloadHandler, .user_ctx = nullptr};
  httpd_uri_t flash_delete_uri = {.uri = "/flash/delete", .method = HTTP_POST, .handler = FlashDeleteHandler, .user_ctx = nullptr};
  httpd_uri_t flash_clear_uploaded_uri = {.uri = "/flash/clear_uploaded", .method = HTTP_POST, .handler = FlashClearUploadedHandler, .user_ctx = nullptr};
  httpd_uri_t transfer_flash_to_sd_uri = {.uri = "/storage/transfer_flash_to_sd", .method = HTTP_POST, .handler = TransferFlashToSdHandler, .user_ctx = nullptr};
  httpd_uri_t transfer_sd_to_flash_uri = {.uri = "/storage/transfer_sd_to_flash", .method = HTTP_POST, .handler = TransferSdToFlashHandler, .user_ctx = nullptr};

  httpd_register_uri_handler(http_server, &root_uri);
  httpd_register_uri_handler(http_server, &data_uri);
  httpd_register_uri_handler(http_server, &calibrate_uri);
  httpd_register_uri_handler(http_server, &restart_uri);
  httpd_register_uri_handler(http_server, &external_power_set_uri);
  httpd_register_uri_handler(http_server, &external_power_cycle_uri);
  httpd_register_uri_handler(http_server, &stepper_enable_uri);
  httpd_register_uri_handler(http_server, &stepper_disable_uri);
  httpd_register_uri_handler(http_server, &stepper_move_uri);
  httpd_register_uri_handler(http_server, &stepper_stop_uri);
  httpd_register_uri_handler(http_server, &stepper_zero_uri);
  httpd_register_uri_handler(http_server, &stepper_find_zero_uri);
  httpd_register_uri_handler(http_server, &stepper_home_offset_uri);
  httpd_register_uri_handler(http_server, &wifi_apply_uri);
  httpd_register_uri_handler(http_server, &net_apply_uri);
  httpd_register_uri_handler(http_server, &cloud_apply_uri);
  httpd_register_uri_handler(http_server, &meteo_config_apply_uri);
  httpd_register_uri_handler(http_server, &gps_apply_uri);
  httpd_register_uri_handler(http_server, &gps_probe_uri);
  httpd_register_uri_handler(http_server, &config_sync_internal_uri);
  httpd_register_uri_handler(http_server, &storage_apply_uri);
  httpd_register_uri_handler(http_server, &storage_remount_uri);
  httpd_register_uri_handler(http_server, &heater_set_uri);
  httpd_register_uri_handler(http_server, &fan_set_uri);
  httpd_register_uri_handler(http_server, &log_start_uri);
  httpd_register_uri_handler(http_server, &log_stop_uri);
  httpd_register_uri_handler(http_server, &pid_apply_uri);
  httpd_register_uri_handler(http_server, &pid_enable_uri);
  httpd_register_uri_handler(http_server, &pid_disable_uri);
  httpd_register_uri_handler(http_server, &fs_list_uri);
  httpd_register_uri_handler(http_server, &fs_download_uri);
  httpd_register_uri_handler(http_server, &fs_delete_uri);
  httpd_register_uri_handler(http_server, &uploaded_clear_uri);
  httpd_register_uri_handler(http_server, &flash_list_uri);
  httpd_register_uri_handler(http_server, &flash_download_uri);
  httpd_register_uri_handler(http_server, &flash_delete_uri);
  httpd_register_uri_handler(http_server, &flash_clear_uploaded_uri);
  httpd_register_uri_handler(http_server, &transfer_flash_to_sd_uri);
  httpd_register_uri_handler(http_server, &transfer_sd_to_flash_uri);
  ESP_LOGI(kTag, "HTTP server started");
  return http_server;
}
