#include "onewire_m1820.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "onewire_crc.h"

namespace {

constexpr int kMaxDevices = 8;
constexpr uint8_t kCmdSearchRom = 0xF0;
constexpr uint8_t kCmdMatchRom = 0x55;
constexpr uint8_t kCmdSkipRom = 0xCC;
constexpr uint8_t kCmdConvertT = 0x44;
constexpr uint8_t kCmdReadScratchpad = 0xBE;
constexpr uint32_t kConversionDelayUs = 11000;  // ~10.6 ms from datasheet

const char* TAG = "M1820";

onewire_bus_handle_t g_bus = nullptr;
std::array<uint64_t, kMaxDevices> g_addresses{};
int g_sensor_count = 0;

uint8_t SearchCrc8(const uint8_t* data, size_t len) {
  return onewire_crc8(0, const_cast<uint8_t*>(data), len);
}

bool BusReset() {
  esp_err_t err = onewire_bus_reset(g_bus);
  if (err == ESP_ERR_NOT_FOUND) {
    ESP_LOGW(TAG, "No devices on 1-Wire bus");
    return false;
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "1-Wire reset failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// Minimal port of the Arduino OneWire search algorithm.
bool SearchNext(uint64_t* out_rom, int* last_discrepancy, bool* last_device_flag, int* last_family_discrepancy) {
  if (!out_rom || !g_bus) return false;
  uint8_t rom[8] = {};
  int id_bit_number = 1;
  int last_zero = 0;
  uint8_t rom_byte_number = 0;
  uint8_t rom_byte_mask = 1;

  if (*last_device_flag) {
    return false;
  }
  if (!BusReset()) {
    *last_discrepancy = 0;
    *last_device_flag = false;
    *last_family_discrepancy = 0;
    return false;
  }

  uint8_t search_cmd = kCmdSearchRom;
  if (onewire_bus_write_bytes(g_bus, &search_cmd, 1) != ESP_OK) {
    return false;
  }

  do {
    uint8_t id_bit = 0;
    uint8_t cmp_id_bit = 0;
    if (onewire_bus_read_bit(g_bus, &id_bit) != ESP_OK) {
      return false;
    }
    if (onewire_bus_read_bit(g_bus, &cmp_id_bit) != ESP_OK) {
      return false;
    }

    if (id_bit == 1 && cmp_id_bit == 1) {
      break;
    }

    uint8_t search_direction;
    if (id_bit != cmp_id_bit) {
      search_direction = id_bit;
    } else {
      if (id_bit_number < *last_discrepancy) {
        search_direction = ((rom[rom_byte_number] & rom_byte_mask) > 0);
      } else {
        search_direction = (id_bit_number == *last_discrepancy);
      }
      if (search_direction == 0) {
        last_zero = id_bit_number;
        if (last_zero < 9) {
          *last_family_discrepancy = last_zero;
        }
      }
    }

    if (search_direction) {
      rom[rom_byte_number] |= rom_byte_mask;
    } else {
      rom[rom_byte_number] &= static_cast<uint8_t>(~rom_byte_mask);
    }

    if (onewire_bus_write_bit(g_bus, search_direction) != ESP_OK) {
      return false;
    }

    id_bit_number++;
    rom_byte_mask <<= 1;

    if (rom_byte_mask == 0) {
      rom_byte_number++;
      rom_byte_mask = 1;
    }
  } while (rom_byte_number < 8);

  if (id_bit_number < 65) {
    return false;
  }

  *last_discrepancy = last_zero;
  if (*last_discrepancy == 0) {
    *last_device_flag = true;
  }

  std::memcpy(out_rom, rom, sizeof(rom));
  return true;
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

bool MatchRom(const uint8_t* rom) {
  uint8_t payload[9] = {kCmdMatchRom};
  std::memcpy(&payload[1], rom, 8);
  return onewire_bus_write_bytes(g_bus, payload, sizeof(payload)) == ESP_OK;
}

bool ReadScratchpad(const uint8_t* rom, uint8_t* out_data, size_t len) {
  if (!out_data || len < 9) return false;
  if (!BusReset()) return false;
  if (!MatchRom(rom)) return false;
  uint8_t cmd = kCmdReadScratchpad;
  if (onewire_bus_write_bytes(g_bus, &cmd, 1) != ESP_OK) return false;
  return onewire_bus_read_bytes(g_bus, out_data, 9) == ESP_OK;
}

}  // namespace

bool M1820Init(gpio_num_t pin) {
  g_addresses.fill(0);
  g_sensor_count = 0;

  if (!InitBus(pin)) {
    return false;
  }

  ESP_LOGI(TAG, "Start searching M1820 sensors...");
  int last_discrepancy = 0;
  bool last_device_flag = false;
  int last_family_discrepancy = 0;

  int found = 0;
  int attempts = 0;
  while (found < kMaxDevices && attempts < 64) {
    uint64_t rom = 0;
    if (!SearchNext(&rom, &last_discrepancy, &last_device_flag, &last_family_discrepancy)) {
      break;
    }
    attempts++;
    uint8_t rom_bytes[8];
    std::memcpy(rom_bytes, &rom, sizeof(rom_bytes));
    uint8_t family = static_cast<uint8_t>(rom & 0xFF);
    if (family != 0x28) {
      ESP_LOGI(TAG, "Skip non-M1820 device family 0x%02X (rom %016llX)", family, (unsigned long long)rom);
      if (last_device_flag) break;
      continue;
    }
    bool duplicate = false;
    for (int i = 0; i < found; ++i) {
      if (g_addresses[i] == rom) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      ESP_LOGI(TAG, "Duplicate device %016llX skipped, stopping search", (unsigned long long)rom);
      break;
    }
    g_addresses[found] = rom;
    ESP_LOGI(TAG, "Found M1820[%d] addr=%016llX", found, (unsigned long long)rom);
    found++;
    if (last_device_flag) {
      break;
    }
  }

  g_sensor_count = found;
  ESP_LOGI(TAG, "Searching done, %d M1820 device(s) found", g_sensor_count);
  return g_sensor_count > 0;
}

int M1820GetSensorCount() {
  return g_sensor_count;
}

bool M1820ReadTemperatures(float* out_values, int max_values, int* out_count) {
  if (!out_values || !out_count || g_sensor_count == 0 || max_values <= 0 || g_bus == nullptr) {
    return false;
  }

  for (int i = 0; i < max_values; ++i) {
    out_values[i] = NAN;
  }

  const int to_read = std::min(g_sensor_count, max_values);
  *out_count = 0;

  // Start conversion for all devices.
  if (!BusReset()) {
    return false;
  }
  uint8_t convert_cmd[2] = {kCmdSkipRom, kCmdConvertT};
  if (onewire_bus_write_bytes(g_bus, convert_cmd, sizeof(convert_cmd)) != ESP_OK) {
    ESP_LOGW(TAG, "Convert command failed");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS((kConversionDelayUs + 999) / 1000));

  bool any_ok = false;
  for (int i = 0; i < to_read; ++i) {
    const uint64_t addr = g_addresses[i];
    if (addr == 0) continue;
    uint8_t rom_bytes[8];
    std::memcpy(rom_bytes, &addr, sizeof(rom_bytes));

    uint8_t data[9] = {};
    if (!ReadScratchpad(rom_bytes, data, sizeof(data))) {
      ESP_LOGW(TAG, "Scratchpad read failed for sensor %d", i);
      continue;
    }
    if (SearchCrc8(data, 8) != data[8]) {
      ESP_LOGW(TAG, "Scratchpad CRC error for sensor %d", i);
      continue;
    }
    int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(data[1]) << 8) | data[0]);
    float celsius = 40.0f + (static_cast<float>(raw) / 256.0f);
    out_values[i] = celsius;
    any_ok = true;
  }

  *out_count = to_read;
  return any_ok;
}

int M1820GetAddresses(uint64_t* out_values, int max_values) {
  if (!out_values || max_values <= 0) {
    return 0;
  }
  int count = std::min(g_sensor_count, max_values);
  for (int i = 0; i < count; ++i) {
    out_values[i] = g_addresses[i];
  }
  for (int i = count; i < max_values; ++i) {
    out_values[i] = 0;
  }
  return count;
}
