#include "onewire_ds18b20.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ds18b20.h"
#include "esp_err.h"
#include "esp_log.h"
#include "onewire_bus.h"
#include "onewire_device.h"

namespace {

constexpr int kMaxDevices = 8;
const char* TAG = "DS18B20";

onewire_bus_handle_t g_bus = nullptr;
std::array<ds18b20_device_handle_t, kMaxDevices> g_devices{};
int g_sensor_count = 0;

void CleanupDevices() {
  for (auto& dev : g_devices) {
    if (dev) {
      ds18b20_del_device(dev);
      dev = nullptr;
    }
  }
  g_sensor_count = 0;
}

bool InitBus(gpio_num_t pin) {
  if (g_bus) {
    return true;
  }

  onewire_bus_config_t bus_cfg = {
      .bus_gpio_num = pin,
      .flags =
          {
              .en_pull_up = true,
          },
  };
  onewire_bus_rmt_config_t rmt_cfg = {
      .max_rx_bytes = 10,  // ROM command + ROM + device command
  };

  esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &g_bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init 1-Wire bus: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

}  // namespace

bool Ds18b20Init(gpio_num_t pin) {
  CleanupDevices();

  if (!InitBus(pin)) {
    return false;
  }

  onewire_device_iter_handle_t iter = nullptr;
  esp_err_t err = onewire_new_device_iter(g_bus, &iter);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create 1-Wire iterator: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(TAG, "Device iterator created, start searching...");
  int device_num = 0;
  onewire_device_t next_dev{};
  while (device_num < kMaxDevices) {
    err = onewire_device_iter_get_next(iter, &next_dev);
    if (err == ESP_ERR_NOT_FOUND) {
      break;
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Iterator error: %s", esp_err_to_name(err));
      break;
    }

    ds18b20_config_t ds_cfg = {};
    ds18b20_device_handle_t handle = nullptr;
    err = ds18b20_new_device_from_enumeration(&next_dev, &ds_cfg, &handle);
    if (err == ESP_OK && handle) {
      g_devices[device_num] = handle;
      g_sensor_count = device_num + 1;
      onewire_device_address_t address = 0;
      if (ds18b20_get_device_address(handle, &address) == ESP_OK) {
        ESP_LOGI(TAG, "Found a DS18B20[%d], address: %016llX", device_num, (unsigned long long)address);
      } else {
        ESP_LOGI(TAG, "Found a DS18B20[%d]", device_num);
      }
      device_num++;
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
      ESP_LOGI(TAG, "Found an unknown device, address: %016llX", (unsigned long long)next_dev.address);
    } else {
      ESP_LOGW(TAG, "Failed to init device %016llX: %s", (unsigned long long)next_dev.address, esp_err_to_name(err));
    }
  }

  onewire_del_device_iter(iter);
  ESP_LOGI(TAG, "Searching done, %d DS18B20 device(s) found", g_sensor_count);

  if (g_sensor_count == 0) {
    ESP_LOGW(TAG, "No DS18B20 sensors found on bus");
    return false;
  }

  return true;
}

int Ds18b20GetSensorCount() {
  return g_sensor_count;
}

bool Ds18b20ReadTemperatures(float* out_values, int max_values, int* out_count) {
  if (!out_values || !out_count || g_sensor_count == 0 || max_values <= 0 || g_bus == nullptr) {
    return false;
  }

  for (int i = 0; i < max_values; ++i) {
    out_values[i] = NAN;
  }

  int to_read = std::min(g_sensor_count, max_values);
  *out_count = 0;

  esp_err_t err = ds18b20_trigger_temperature_conversion_for_all(g_bus);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Trigger conversion failed: %s", esp_err_to_name(err));
    return false;
  }

  bool any_ok = false;
  for (int i = 0; i < to_read; ++i) {
    float temp = NAN;
    err = ds18b20_get_temperature(g_devices[i], &temp);
    if (err == ESP_OK) {
      out_values[i] = temp;
      any_ok = true;
    } else {
      ESP_LOGW(TAG, "Read temp failed for sensor %d: %s", i, esp_err_to_name(err));
    }
  }

  *out_count = to_read;
  return any_ok;
}
