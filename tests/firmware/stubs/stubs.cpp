#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "hw_pins.h"
#include "app_services.h"
#include "test_stubs.h"

uint64_t stub_time_us = 0;
int stub_led_red = -1;
int stub_led_green = -1;
int stub_timer_period_ms = 0;
int stub_timer_started = 0;
std::string stub_iso = "2026-01-13T00:00:00Z";

uint64_t esp_timer_get_time() {
  return stub_time_us;
}

std::string IsoUtcNow() {
  return stub_iso;
}

int gpio_set_level(gpio_num_t pin, int level) {
  if (pin == STATUS_LED_RED) {
    stub_led_red = level;
  } else if (pin == STATUS_LED_GREEN) {
    stub_led_green = level;
  }
  return 0;
}

SemaphoreHandle_t xSemaphoreCreateMutex() {
  return reinterpret_cast<SemaphoreHandle_t>(0x1);
}

int xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
  return pdTRUE;
}

void xSemaphoreGive(SemaphoreHandle_t) {
}

TimerHandle_t xTimerCreate(const char*, TickType_t period, int, void*, TimerCallbackFunction_t) {
  stub_timer_period_ms = static_cast<int>(period);
  return reinterpret_cast<TimerHandle_t>(0x2);
}

int xTimerStart(TimerHandle_t, TickType_t) {
  stub_timer_started = 1;
  return pdTRUE;
}

int xTimerStop(TimerHandle_t, TickType_t) {
  stub_timer_started = 0;
  return pdTRUE;
}

int xTimerChangePeriod(TimerHandle_t, TickType_t new_period, TickType_t) {
  stub_timer_period_ms = static_cast<int>(new_period);
  return pdTRUE;
}

struct cJSON {
  std::vector<std::string> fields;
};

static std::string EscapeJson(const char* value) {
  std::string out;
  if (!value) return out;
  for (const char* p = value; *p != '\0'; ++p) {
    if (*p == '"' || *p == '\\') {
      out.push_back('\\');
    }
    out.push_back(*p);
  }
  return out;
}

extern "C" cJSON* cJSON_CreateObject(void) {
  return new cJSON();
}

extern "C" void cJSON_AddStringToObject(cJSON* object, const char* key, const char* value) {
  if (!object || !key) return;
  std::string escaped = EscapeJson(value);
  object->fields.push_back("\"" + std::string(key) + "\":\"" + escaped + "\"");
}

extern "C" void cJSON_AddNumberToObject(cJSON* object, const char* key, double value) {
  if (!object || !key) return;
  object->fields.push_back("\"" + std::string(key) + "\":" + std::to_string(value));
}

extern "C" void cJSON_AddBoolToObject(cJSON* object, const char* key, int value) {
  if (!object || !key) return;
  object->fields.push_back("\"" + std::string(key) + "\":" + std::string(value ? "true" : "false"));
}

extern "C" char* cJSON_PrintUnformatted(const cJSON* object) {
  if (!object) return nullptr;
  std::string out = "{";
  for (size_t i = 0; i < object->fields.size(); ++i) {
    if (i > 0) out += ",";
    out += object->fields[i];
  }
  out += "}";
  char* buf = static_cast<char*>(std::malloc(out.size() + 1));
  if (!buf) return nullptr;
  std::memcpy(buf, out.c_str(), out.size() + 1);
  return buf;
}

extern "C" void cJSON_Delete(cJSON* object) {
  delete object;
}

extern "C" void cJSON_free(void* ptr) {
  std::free(ptr);
}
