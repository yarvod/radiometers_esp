#include "mqtt_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "control_actions.h"
#include "app_services.h"
#include "app_state.h"
#include "app_utils.h"
#include "cJSON.h"
#include "error_manager.h"
#include "esp_log.h"
#include "mqtt_client.h"

namespace {

constexpr char TAG_MQTT[] = "MQTT_BRIDGE";
esp_mqtt_client_handle_t mqtt_client = nullptr;
static bool mqtt_connected = false;
static std::string mqtt_rx_topic;
static std::string mqtt_rx_payload;
extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_crt_end[] asm("_binary_ca_crt_end");
extern const uint8_t isrgrootx1_pem_start[] asm("_binary_isrgrootx1_pem_start");
extern const uint8_t isrgrootx1_pem_end[] asm("_binary_isrgrootx1_pem_end");

struct ParsedMqttUri {
  esp_mqtt_transport_t transport = MQTT_TRANSPORT_UNKNOWN;
  std::string host;
  std::string path;
  uint32_t port = 0;
};

void HandleMqttCommand(const std::string& topic, const std::string& payload);

bool IsAllDigits(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool ParseMqttUri(const std::string& raw_uri, ParsedMqttUri* out) {
  if (!out) {
    return false;
  }
  const std::string uri = NormalizeMqttUri(raw_uri);
  const std::string lower = NormalizeMqttUri(raw_uri);
  const size_t scheme_pos = lower.find("://");
  if (scheme_pos == std::string::npos) {
    return false;
  }
  const std::string scheme = lower.substr(0, scheme_pos);
  std::string rest = uri.substr(scheme_pos + 3);
  while (!rest.empty() && rest.front() == ':') {
    rest.erase(rest.begin());
  }

  if (scheme == "mqtt") {
    out->transport = MQTT_TRANSPORT_OVER_TCP;
    out->port = 1883;
  } else if (scheme == "mqtts") {
    out->transport = MQTT_TRANSPORT_OVER_SSL;
    out->port = 8883;
  } else if (scheme == "ws") {
    out->transport = MQTT_TRANSPORT_OVER_WS;
    out->port = 80;
  } else if (scheme == "wss") {
    out->transport = MQTT_TRANSPORT_OVER_WSS;
    out->port = 443;
  } else {
    return false;
  }

  const size_t path_pos = rest.find('/');
  std::string host_port = path_pos == std::string::npos ? rest : rest.substr(0, path_pos);
  out->path = path_pos == std::string::npos ? std::string() : rest.substr(path_pos);
  while (!host_port.empty() && host_port.front() == ':') {
    host_port.erase(host_port.begin());
  }

  const size_t port_pos = host_port.rfind(':');
  if (port_pos != std::string::npos) {
    const std::string port_str = host_port.substr(port_pos + 1);
    if (IsAllDigits(port_str)) {
      out->port = static_cast<uint32_t>(std::strtoul(port_str.c_str(), nullptr, 10));
      out->host = host_port.substr(0, port_pos);
    } else {
      out->host = host_port;
    }
  } else {
    out->host = host_port;
  }
  while (!out->host.empty() && out->host.front() == ':') {
    out->host.erase(out->host.begin());
  }
  return !out->host.empty() && out->port > 0 && out->transport != MQTT_TRANSPORT_UNKNOWN;
}

bool MqttPublish(const std::string& topic, const char* payload, int len, int qos = 0, bool retain = false) {
  if (!mqtt_client || !mqtt_connected || topic.empty()) return false;
  const int msg_id =
      esp_mqtt_client_enqueue(mqtt_client, topic.c_str(), payload, len, qos, retain ? 1 : 0, true);
  if (msg_id < 0) {
    ESP_LOGD(TAG_MQTT, "MQTT enqueue dropped: %s", topic.c_str());
    return false;
  }
  return true;
}

bool MqttPublish(const std::string& topic, const std::string& payload, int qos = 0, bool retain = false) {
  return MqttPublish(topic, payload.c_str(), payload.size(), qos, retain);
}

void PublishMeasurementPayloadInternal(const std::string& payload) {
  if (!mqtt_connected || !mqtt_client) return;
  const std::string device = SanitizeId(app_config.device_id);
  const std::string topic = device + "/measure";
  MqttPublish(topic, payload);
}

void PublishErrorPayloadInternal(const std::string& payload) {
  if (!mqtt_connected || !mqtt_client) return;
  const std::string device = SanitizeId(app_config.device_id);
  const std::string topic = device + "/error";
  MqttPublish(topic, payload);
}

void MqttSendResponse(const std::string& device, const std::string& req_id, const ActionResult& res, const char* data_json = nullptr) {
  char json[1024];
  int len = 0;
  if (data_json && data_json[0]) {
    len = std::snprintf(json, sizeof(json), "{\"ok\":%s,\"reqId\":\"%s\",\"message\":\"%s\",\"data\":%s}",
                        res.ok ? "true" : "false", req_id.c_str(), res.message.c_str(), data_json);
  } else {
    len = std::snprintf(json, sizeof(json), "{\"ok\":%s,\"reqId\":\"%s\",\"message\":\"%s\"}",
                        res.ok ? "true" : "false", req_id.c_str(), res.message.c_str());
  }
  if (len < 0) {
    return;
  }
  if (len >= static_cast<int>(sizeof(json))) {
    len = sizeof(json) - 1;
  }
  std::string topic = device + "/resp";
  MqttPublish(topic, json, len);
}

void BuildMqttLightState(char* out, size_t out_len) {
  if (!out || out_len == 0) return;
  SharedState s{};
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    s.logging = state.logging;
    s.stepper_enabled = state.stepper_enabled;
    s.stepper_moving = state.stepper_moving;
    s.homing = state.homing;
    s.stepper_position = state.stepper_position;
    s.stepper_target = state.stepper_target;
    s.heater_power = state.heater_power;
    s.fan_power = state.fan_power;
    s.wifi_rssi_dbm = state.wifi_rssi_dbm;
    s.wifi_quality = state.wifi_quality;
    xSemaphoreGive(state_mutex);
  }
  std::snprintf(out, out_len,
                "{\"logging\":%s,\"stepperEnabled\":%s,\"stepperMoving\":%s,\"stepperHoming\":%s,"
                "\"stepperPosition\":%d,\"stepperTarget\":%d,\"heaterPower\":%.1f,\"fanPower\":%.1f,"
                "\"wifiRssi\":%d,\"wifiQuality\":%d}",
                s.logging ? "true" : "false",
                s.stepper_enabled ? "true" : "false",
                s.stepper_moving ? "true" : "false",
                s.homing ? "true" : "false",
                s.stepper_position,
                s.stepper_target,
                s.heater_power,
                s.fan_power,
                s.wifi_rssi_dbm,
                s.wifi_quality);
}

void HandleMqttCommand(const std::string& topic, const std::string& payload) {
  const std::string device = SanitizeId(app_config.device_id);
  if (topic != device + "/cmd" || payload.empty()) {
    ESP_LOGD(TAG_MQTT, "Ignoring MQTT topic %s (device cmd is %s/cmd)", topic.c_str(), device.c_str());
    return;
  }
  ESP_LOGI(TAG_MQTT, "MQTT cmd on %s", topic.c_str());
  cJSON* root = cJSON_Parse(payload.c_str());
  if (!root) {
    MqttSendResponse(device, "", {false, "bad json", {}});
    return;
  }
  auto get_str = [&](const char* key) -> std::string {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsString(item) && item->valuestring) ? std::string(item->valuestring) : std::string();
  };
  auto get_num = [&](const char* key, float def) -> float {
    cJSON* v = cJSON_GetObjectItem(root, key);
    return (v && cJSON_IsNumber(v)) ? static_cast<float>(v->valuedouble) : def;
  };
  auto get_int = [&](const char* key, int def) -> int {
    cJSON* v = cJSON_GetObjectItem(root, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : def;
  };
  auto get_bool = [&](const char* key, bool def) -> bool {
    cJSON* v = cJSON_GetObjectItem(root, key);
    if (v && cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (v && cJSON_IsNumber(v)) return v->valueint != 0;
    return def;
  };

  const std::string type = get_str("type");
  const std::string req_id = get_str("reqId");
  ActionResult res{false, "unknown type", {}};

  if (type == "get_state") {
    char state_json[512];
    BuildMqttLightState(state_json, sizeof(state_json));
    MqttSendResponse(device, req_id, {true, "state", {}}, state_json);
    cJSON_Delete(root);
    return;
  } else if (type == "log_start") {
    LogRequest req;
    req.filename = get_str("filename");
    req.use_motor = get_bool("useMotor", false);
    req.duration_s = get_num("durationSec", log_config.duration_s);
    res = ActionStartLog(req);
  } else if (type == "log_stop") {
    res = ActionStopLog();
  } else if (type == "stepper_move") {
    StepperMoveRequest req;
    req.steps = get_int("steps", 400);
    req.forward = !get_bool("reverse", false);
    req.speed_us = get_int("speedUs", app_config.stepper_speed_us);
    res = ActionStepperMove(req);
  } else if (type == "stepper_stop") {
    res = ActionStepperStop();
  } else if (type == "stepper_find_zero") {
    res = ActionStepperFindZero();
  } else if (type == "stepper_zero") {
    res = ActionStepperZero();
  } else if (type == "stepper_home_offset") {
    StepperHomeOffsetRequest req;
    req.offset_steps = get_int("offsetSteps", get_int("offset", app_config.stepper_home_offset_steps));
    req.speed_us = get_int("speedUs", get_int("speed", app_config.stepper_speed_us));
    cJSON* hall_item = cJSON_GetObjectItem(root, "hallActiveLevel");
    if (!hall_item) hall_item = cJSON_GetObjectItem(root, "motorHallActiveLevel");
    if (hall_item && cJSON_IsNumber(hall_item)) {
      req.hall_active_level = hall_item->valueint ? 1 : 0;
      req.hall_active_level_set = true;
    }
    res = ActionStepperHomeOffset(req);
  } else if (type == "stepper_enable") {
    res = ActionStepperEnable();
  } else if (type == "stepper_disable") {
    res = ActionStepperDisable();
  } else if (type == "heater_set") {
    res = ActionHeaterSet(get_num("power", 0.0f));
  } else if (type == "fan_set") {
    res = ActionFanSet(get_num("power", 0.0f));
  } else if (type == "pid_apply") {
    PidApplyRequest req;
    req.kp = get_num("kp", pid_config.kp);
    req.ki = get_num("ki", pid_config.ki);
    req.kd = get_num("kd", pid_config.kd);
    req.setpoint = get_num("setpoint", pid_config.setpoint);
    req.sensor = get_int("sensor", pid_config.sensor_index);
    req.sensor_mask = pid_config.sensor_mask;

    cJSON* mask_item = cJSON_GetObjectItem(root, "sensorMask");
    if (mask_item && cJSON_IsNumber(mask_item)) {
      req.sensor_mask = static_cast<uint16_t>(mask_item->valuedouble);
      req.sensor_mask_set = true;
    }
    cJSON* sensors_item = cJSON_GetObjectItem(root, "sensors");
    if (sensors_item && cJSON_IsArray(sensors_item)) {
      req.sensor_mask = 0;
      const int len = cJSON_GetArraySize(sensors_item);
      for (int i = 0; i < len; ++i) {
        cJSON* entry = cJSON_GetArrayItem(sensors_item, i);
        if (!entry || !cJSON_IsNumber(entry)) continue;
        int idx = entry->valueint;
        if (idx < 0 || idx >= MAX_TEMP_SENSORS) continue;
        req.sensor_mask = static_cast<uint16_t>(req.sensor_mask | (1u << idx));
      }
      req.sensor_mask_set = true;
    }
    res = ActionPidApply(req);
  } else if (type == "pid_enable") {
    res = ActionPidEnable();
  } else if (type == "pid_disable") {
    res = ActionPidDisable();
  } else if (type == "wifi_apply") {
    res = ActionWifiApply({get_str("mode"), get_str("ssid"), get_str("password")});
  } else if (type == "net_apply") {
    res = ActionNetApply({get_str("mode"), get_str("priority")});
  } else if (type == "cloud_apply") {
    CloudApplyRequest req;
    req.device_id = get_str("deviceId");
    req.minio_endpoint = get_str("minioEndpoint");
    req.minio_access_key = get_str("minioAccessKey");
    req.minio_secret_key = get_str("minioSecretKey");
    req.minio_bucket = get_str("minioBucket");
    req.minio_enabled = get_bool("minioEnabled", app_config.minio_enabled);
    req.mqtt_uri = get_str("mqttUri");
    req.mqtt_user = get_str("mqttUser");
    req.mqtt_password = get_str("mqttPassword");
    req.mqtt_enabled = get_bool("mqttEnabled", app_config.mqtt_enabled);
    res = ActionCloudApply(req);
  } else if (type == "usb_mode_get") {
    res = ActionUsbModeGet();
  } else if (type == "usb_mode_set") {
    const std::string mode = get_str("mode");
    res = ActionUsbModeSet(mode == "msc" ? UsbMode::kMsc : UsbMode::kCdc);
  } else if (type == "calibrate") {
    res = ActionCalibrate();
  } else if (type == "restart") {
    res = ActionRestart();
  }

  MqttSendResponse(device, req_id, res);
  cJSON_Delete(root);
}

void HandleMqttData(esp_mqtt_event_handle_t event) {
  if (!event || !event->data || event->data_len <= 0) {
    return;
  }

  const int total_len = event->total_data_len > 0 ? event->total_data_len : event->data_len;
  const int offset = event->current_data_offset;
  const bool fragmented = total_len > event->data_len || offset > 0;

  if (!fragmented) {
    std::string topic(event->topic, event->topic_len);
    std::string payload(event->data, event->data_len);
    HandleMqttCommand(topic, payload);
    return;
  }

  if (offset == 0) {
    mqtt_rx_topic.assign(event->topic, event->topic_len);
    mqtt_rx_payload.clear();
    mqtt_rx_payload.reserve(total_len);
  }
  if (mqtt_rx_topic.empty() || offset != static_cast<int>(mqtt_rx_payload.size())) {
    ESP_LOGW(TAG_MQTT, "MQTT fragmented command out of order");
    mqtt_rx_topic.clear();
    mqtt_rx_payload.clear();
    return;
  }
  mqtt_rx_payload.append(event->data, event->data_len);
  if (static_cast<int>(mqtt_rx_payload.size()) >= total_len) {
    HandleMqttCommand(mqtt_rx_topic, mqtt_rx_payload);
    mqtt_rx_topic.clear();
    mqtt_rx_payload.clear();
  }
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED: {
      mqtt_connected = true;
      ErrorManagerClear(ErrorCode::kMqttDisconnected);
      ErrorManagerClear(ErrorCode::kMqttTransport);
      const std::string device = SanitizeId(app_config.device_id);
      std::string topic = device + "/cmd";
      const int msg_id = esp_mqtt_client_subscribe(event->client, topic.c_str(), 0);
      ESP_LOGI(TAG_MQTT, "MQTT connected to %s, subscribe %s msg_id=%d", app_config.mqtt_uri.c_str(), topic.c_str(), msg_id);
      if (msg_id < 0) {
        ErrorManagerSet(ErrorCode::kMqttTransport, ErrorSeverity::kWarning, "MQTT cmd subscribe failed");
      }
      break;
    }
    case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGI(TAG_MQTT, "MQTT subscribed msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_ERROR: {
      std::string msg = "MQTT transport error";
      if (event->error_handle) {
        const auto* err = event->error_handle;
        msg += " type=" + std::to_string(err->error_type);
        if (err->esp_tls_last_esp_err != 0) {
          msg += " tls=" + std::string(esp_err_to_name(err->esp_tls_last_esp_err));
        }
        if (err->esp_tls_stack_err != 0) {
          msg += " tls_stack=" + std::to_string(err->esp_tls_stack_err);
        }
        if (err->esp_transport_sock_errno != 0) {
          msg += " sock=" + std::to_string(err->esp_transport_sock_errno);
        }
      }
      ErrorManagerSet(ErrorCode::kMqttTransport, ErrorSeverity::kWarning, msg);
      break;
    }
    case MQTT_EVENT_DATA: {
      HandleMqttData(event);
      break;
    }
    case MQTT_EVENT_DISCONNECTED: {
      mqtt_connected = false;
      ErrorManagerSet(ErrorCode::kMqttDisconnected, ErrorSeverity::kWarning, "MQTT disconnected");
      ESP_LOGW(TAG_MQTT, "MQTT disconnected");
      break;
    }
    default:
      break;
  }
  return ESP_OK;
}

static void mqtt_event_dispatch(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
  (void)handler_args;
  (void)base;
  (void)event_id;
  mqtt_event_handler_cb(static_cast<esp_mqtt_event_handle_t>(event_data));
}

}  // namespace

void StartMqttBridge() {
  if (!app_config.mqtt_enabled) {
    ESP_LOGI(TAG_MQTT, "MQTT disabled by config");
    return;
  }
  if (app_config.mqtt_uri.empty()) {
    ESP_LOGI(TAG_MQTT, "MQTT disabled (uri empty)");
    return;
  }
  ParsedMqttUri parsed_uri;
  if (!ParseMqttUri(app_config.mqtt_uri, &parsed_uri)) {
    ESP_LOGE(TAG_MQTT, "Invalid MQTT URI: %s", app_config.mqtt_uri.c_str());
    ErrorManagerSet(ErrorCode::kMqttDisconnected, ErrorSeverity::kError, "Invalid MQTT URI");
    return;
  }
  if (mqtt_client) {
    esp_mqtt_client_stop(mqtt_client);
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = nullptr;
  }
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.hostname = parsed_uri.host.c_str();
  cfg.broker.address.port = parsed_uri.port;
  cfg.broker.address.transport = parsed_uri.transport;
  cfg.broker.address.path = parsed_uri.path.empty() ? nullptr : parsed_uri.path.c_str();
  cfg.credentials.username = app_config.mqtt_user.empty() ? nullptr : app_config.mqtt_user.c_str();
  cfg.credentials.authentication.password = app_config.mqtt_password.empty() ? nullptr : app_config.mqtt_password.c_str();
  cfg.session.keepalive = 30;
  cfg.network.disable_auto_reconnect = false;
  cfg.session.disable_clean_session = false;
  cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  cfg.task.stack_size = 8192;
  if (parsed_uri.transport == MQTT_TRANSPORT_OVER_SSL || parsed_uri.transport == MQTT_TRANSPORT_OVER_WSS) {
    cfg.broker.verification.certificate = reinterpret_cast<const char*>(ca_crt_start);
    cfg.broker.verification.certificate_len = ca_crt_end - ca_crt_start;
  }
  mqtt_client = esp_mqtt_client_init(&cfg);
  if (!mqtt_client) {
    ESP_LOGE(TAG_MQTT, "MQTT init failed");
    ErrorManagerSet(ErrorCode::kMqttDisconnected, ErrorSeverity::kError, "MQTT init failed");
    return;
  }
  esp_mqtt_client_register_event(mqtt_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), mqtt_event_dispatch, nullptr);
  esp_err_t start_err = esp_mqtt_client_start(mqtt_client);
  if (start_err != ESP_OK) {
    ESP_LOGE(TAG_MQTT, "MQTT start failed: %s", esp_err_to_name(start_err));
    ErrorManagerSet(ErrorCode::kMqttDisconnected, ErrorSeverity::kError, "MQTT start failed");
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = nullptr;
    mqtt_connected = false;
    return;
  }
}

void PublishMeasurementPayload(const std::string& payload) {
  PublishMeasurementPayloadInternal(payload);
}

void PublishErrorPayload(const std::string& payload) {
  PublishErrorPayloadInternal(payload);
}
