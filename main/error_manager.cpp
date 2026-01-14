#include "error_manager.h"

#include <array>

#include "app_services.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "hw_pins.h"

namespace {

constexpr char TAG_ERROR[] = "ERROR_MANAGER";

struct ErrorEntry {
  bool active = false;
  ErrorSeverity severity = ErrorSeverity::kInfo;
  std::string message;
  uint64_t changed_ms = 0;
};

struct ErrorMeta {
  const char* code;
  bool led_red;
  bool led_green;
  int blink_ms;
  int priority;
};

constexpr std::array<ErrorMeta, static_cast<size_t>(ErrorCode::kMax)> kErrorMeta = {{
    {"wifi_disconnected", true, true, 900, 2},
    {"wifi_fallback", true, true, 250, 4},
    {"usb_msc_init", true, false, 600, 3},
    {"temp_sensor", true, false, 1200, 1},
    {"adc_read", true, false, 200, 5},
    {"ina_init", true, false, 800, 2},
    {"mqtt_disconnected", false, true, 500, 2},
    {"time_sync", false, true, 1500, 1},
    {"sd_mount", true, false, 400, 3},
}};

static_assert(kErrorMeta.size() == static_cast<size_t>(ErrorCode::kMax), "error meta mismatch");

SemaphoreHandle_t error_mutex = nullptr;
TimerHandle_t blink_timer = nullptr;
ErrorPublishFn publish_fn = nullptr;

std::array<ErrorEntry, static_cast<size_t>(ErrorCode::kMax)> entries{};

bool blink_on = false;
bool current_red = false;
bool current_green = false;
int current_blink_ms = 0;
ErrorCode current_code = ErrorCode::kMax;

void ApplyLed(bool red, bool green) {
  gpio_set_level(STATUS_LED_RED, red ? 1 : 0);
  gpio_set_level(STATUS_LED_GREEN, green ? 1 : 0);
}

void ErrorBlinkTimerCb(TimerHandle_t) {
  blink_on = !blink_on;
  if (blink_on) {
    ApplyLed(current_red, current_green);
  } else {
    ApplyLed(false, false);
  }
}

void PublishEvent(ErrorCode code, ErrorSeverity severity, const std::string& message, bool active) {
  if (!publish_fn) {
    return;
  }
  const std::string iso = IsoUtcNow();
  const uint64_t ts_ms = esp_timer_get_time() / 1000ULL;

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "code", ErrorCodeToString(code));
  cJSON_AddStringToObject(root, "severity", ErrorSeverityToString(severity));
  cJSON_AddStringToObject(root, "message", message.c_str());
  cJSON_AddBoolToObject(root, "active", active);
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  cJSON_AddNumberToObject(root, "timestampMs", static_cast<double>(ts_ms));
  const char* json = cJSON_PrintUnformatted(root);
  if (json) {
    publish_fn(json);
  }
  cJSON_free((void*)json);
  cJSON_Delete(root);
}

void UpdateLedPattern() {
  ErrorCode selected = ErrorCode::kMax;
  int best_rank = -1;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i].active) {
      continue;
    }
    const auto& meta = kErrorMeta[i];
    const int rank = static_cast<int>(entries[i].severity) * 10 + meta.priority;
    if (rank > best_rank) {
      best_rank = rank;
      selected = static_cast<ErrorCode>(i);
    }
  }

  if (selected == ErrorCode::kMax) {
    if (blink_timer) {
      xTimerStop(blink_timer, 0);
    }
    current_code = ErrorCode::kMax;
    blink_on = false;
    ApplyLed(false, true);
    return;
  }

  const auto& meta = kErrorMeta[static_cast<size_t>(selected)];
  current_red = meta.led_red;
  current_green = meta.led_green;
  current_blink_ms = meta.blink_ms;
  current_code = selected;

  if (!blink_timer) {
    blink_timer = xTimerCreate("err_led", pdMS_TO_TICKS(current_blink_ms), pdTRUE, nullptr,
                               reinterpret_cast<TimerCallbackFunction_t>(ErrorBlinkTimerCb));
  }
  if (blink_timer) {
    blink_on = true;
    ApplyLed(current_red, current_green);
    xTimerChangePeriod(blink_timer, pdMS_TO_TICKS(current_blink_ms), 0);
    xTimerStart(blink_timer, 0);
  }
}

}  // namespace

void ErrorManagerInit() {
  if (!error_mutex) {
    error_mutex = xSemaphoreCreateMutex();
  }
  if (!error_mutex) {
    ESP_LOGW(TAG_ERROR, "Failed to create error mutex");
  }
  ApplyLed(false, true);
}

void ErrorManagerSetPublisher(ErrorPublishFn publisher) {
  publish_fn = publisher;
}

void ErrorManagerSet(ErrorCode code, ErrorSeverity severity, const std::string& message) {
  const size_t idx = static_cast<size_t>(code);
  if (idx >= entries.size()) {
    return;
  }
  bool changed = false;
  if (error_mutex && xSemaphoreTake(error_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    ErrorEntry& entry = entries[idx];
    changed = !entry.active || entry.severity != severity || entry.message != message;
    entry.active = true;
    entry.severity = severity;
    entry.message = message;
    entry.changed_ms = esp_timer_get_time() / 1000ULL;
    xSemaphoreGive(error_mutex);
  }
  if (changed) {
    PublishEvent(code, severity, message, true);
  }
  UpdateLedPattern();
}

void ErrorManagerClear(ErrorCode code) {
  const size_t idx = static_cast<size_t>(code);
  if (idx >= entries.size()) {
    return;
  }
  bool was_active = false;
  ErrorSeverity severity = ErrorSeverity::kInfo;
  std::string message;
  if (error_mutex && xSemaphoreTake(error_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    ErrorEntry& entry = entries[idx];
    was_active = entry.active;
    severity = entry.severity;
    message = entry.message;
    entry.active = false;
    entry.changed_ms = esp_timer_get_time() / 1000ULL;
    xSemaphoreGive(error_mutex);
  }
  if (was_active) {
    PublishEvent(code, severity, message, false);
  }
  UpdateLedPattern();
}

const char* ErrorCodeToString(ErrorCode code) {
  const size_t idx = static_cast<size_t>(code);
  if (idx >= kErrorMeta.size()) {
    return "unknown";
  }
  return kErrorMeta[idx].code;
}

const char* ErrorSeverityToString(ErrorSeverity severity) {
  switch (severity) {
    case ErrorSeverity::kInfo:
      return "info";
    case ErrorSeverity::kWarning:
      return "warning";
    case ErrorSeverity::kError:
      return "error";
    case ErrorSeverity::kCritical:
      return "critical";
    default:
      return "unknown";
  }
}
