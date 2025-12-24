#include "mqtt_bridge.h"

#include <string>

#include "control_actions.h"
#include "app_services.h"
#include "app_state.h"
#include "cJSON.h"
#include "esp_log.h"
#include "mqtt_client.h"

namespace {

constexpr char TAG_MQTT[] = "MQTT_BRIDGE";
esp_mqtt_client_handle_t mqtt_client = nullptr;
static bool mqtt_connected = false;
extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_crt_end[] asm("_binary_ca_crt_end");
extern const uint8_t isrgrootx1_pem_start[] asm("_binary_isrgrootx1_pem_start");
extern const uint8_t isrgrootx1_pem_end[] asm("_binary_isrgrootx1_pem_end");

void MqttPublish(const std::string& topic, const std::string& payload, int qos = 0, bool retain = false) {
  if (!mqtt_client || topic.empty()) return;
  esp_mqtt_client_publish(mqtt_client, topic.c_str(), payload.c_str(), payload.size(), qos, retain ? 1 : 0);
}

void MqttSendResponse(const std::string& device, const std::string& req_id, const ActionResult& res) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", res.ok);
  if (!req_id.empty()) cJSON_AddStringToObject(root, "reqId", req_id.c_str());
  if (!res.message.empty()) cJSON_AddStringToObject(root, "message", res.message.c_str());
  if (!res.json.empty()) {
    cJSON* data = cJSON_Parse(res.json.c_str());
    if (data) {
      cJSON_AddItemToObject(root, "data", data);
    }
  }
  const char* json = cJSON_PrintUnformatted(root);
  std::string topic = device + "/resp";
  MqttPublish(topic, json ? json : "{}");
  cJSON_free((void*)json);
  cJSON_Delete(root);
}

void HandleMqttCommand(const std::string& topic, const uint8_t* data, int len) {
  const std::string device = SanitizeId(app_config.device_id);
  if (topic != device + "/cmd" || !data || len <= 0) {
    return;
  }
  std::string payload(reinterpret_cast<const char*>(data), static_cast<size_t>(len));
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
    res = ActionGetState();
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
  } else if (type == "stepper_enable") {
    res = ActionStepperEnable();
  } else if (type == "stepper_disable") {
    res = ActionStepperDisable();
  } else if (type == "heater_set") {
    res = ActionHeaterSet(get_num("power", 0.0f));
  } else if (type == "fan_set") {
    res = ActionFanSet(get_num("power", 0.0f));
  }

  MqttSendResponse(device, req_id, res);
  cJSON_Delete(root);
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED: {
      mqtt_connected = true;
      const std::string device = SanitizeId(app_config.device_id);
      std::string topic = device + "/cmd";
      esp_mqtt_client_subscribe(event->client, topic.c_str(), 0);
      ESP_LOGI(TAG_MQTT, "MQTT connected, subscribed to %s", topic.c_str());
      break;
    }
    case MQTT_EVENT_DATA: {
      std::string topic(event->topic, event->topic_len);
      HandleMqttCommand(topic, reinterpret_cast<const uint8_t*>(event->data), event->data_len);
      break;
    }
    case MQTT_EVENT_DISCONNECTED: {
      mqtt_connected = false;
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

static void MqttStateTask(void*) {
  const TickType_t interval = pdMS_TO_TICKS(1000);
  const std::string device = SanitizeId(app_config.device_id);
  const std::string topic = device + "/state";
  while (true) {
    if (mqtt_connected && mqtt_client) {
      std::string payload = BuildStateJsonString();
      esp_mqtt_client_publish(mqtt_client, topic.c_str(), payload.c_str(), payload.size(), 0, 0);
    }
    vTaskDelay(interval);
  }
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
  if (mqtt_client) {
    esp_mqtt_client_stop(mqtt_client);
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = nullptr;
  }
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = app_config.mqtt_uri.c_str();
  cfg.credentials.username = app_config.mqtt_user.empty() ? nullptr : app_config.mqtt_user.c_str();
  cfg.credentials.authentication.password = app_config.mqtt_password.empty() ? nullptr : app_config.mqtt_password.c_str();
  cfg.session.keepalive = 30;
  cfg.network.disable_auto_reconnect = false;
  cfg.session.disable_clean_session = false;
  cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  cfg.broker.verification.certificate = reinterpret_cast<const char*>(ca_crt_start);
  cfg.broker.verification.certificate_len = ca_crt_end - ca_crt_start;
  mqtt_client = esp_mqtt_client_init(&cfg);
  if (!mqtt_client) {
    ESP_LOGE(TAG_MQTT, "MQTT init failed");
    return;
  }
  esp_mqtt_client_register_event(mqtt_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), mqtt_event_dispatch, nullptr);
  esp_err_t start_err = esp_mqtt_client_start(mqtt_client);
  if (start_err != ESP_OK) {
    ESP_LOGE(TAG_MQTT, "MQTT start failed: %s", esp_err_to_name(start_err));
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = nullptr;
    mqtt_connected = false;
    return;
  }
  if (mqtt_state_task == nullptr) {
    xTaskCreatePinnedToCore(&MqttStateTask, "mqtt_state", 4096, nullptr, 2, &mqtt_state_task, 0);
  }
}
