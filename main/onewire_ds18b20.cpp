#include "onewire_ds18b20.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint8_t kCmdSearchRom = 0xF0;
constexpr uint8_t kCmdReadRom = 0x33;
constexpr uint8_t kCmdMatchRom = 0x55;
constexpr uint8_t kCmdSkipRom = 0xCC;
constexpr uint8_t kCmdConvertT = 0x44;
constexpr uint8_t kCmdReadScratchpad = 0xBE;

constexpr int kMaxDevices = 8;
constexpr TickType_t kConvertDelayTicks = pdMS_TO_TICKS(750);  // Max conversion time for 12-bit

const char* TAG = "DS18B20";

gpio_num_t g_pin = GPIO_NUM_NC;
int g_sensor_count = 0;
uint8_t g_rom[kMaxDevices][8] = {};

inline void ow_drive_low(gpio_num_t pin) {
  gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(pin, 0);
}

inline void ow_release(gpio_num_t pin) {
  gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(pin, 1);
}

bool onewire_reset(gpio_num_t pin) {
  ow_drive_low(pin);
  esp_rom_delay_us(480);
  ow_release(pin);
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  esp_rom_delay_us(70);
  bool presence = (gpio_get_level(pin) == 0);
  esp_rom_delay_us(410);
  return presence;
}

void onewire_write_bit(gpio_num_t pin, uint8_t bit) {
  if (bit) {
    ow_drive_low(pin);
    esp_rom_delay_us(6);
    ow_release(pin);
    esp_rom_delay_us(64);
  } else {
    ow_drive_low(pin);
    esp_rom_delay_us(60);
    ow_release(pin);
    esp_rom_delay_us(10);
  }
}

bool onewire_read_bit(gpio_num_t pin) {
  ow_drive_low(pin);
  esp_rom_delay_us(6);
  ow_release(pin);
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  esp_rom_delay_us(9);
  bool bit = gpio_get_level(pin);
  esp_rom_delay_us(55);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(pin, 1);
  return bit;
}

void onewire_write_byte(gpio_num_t pin, uint8_t data) {
  for (int i = 0; i < 8; ++i) {
    onewire_write_bit(pin, data & 0x01);
    data >>= 1;
  }
}

uint8_t onewire_read_byte(gpio_num_t pin) {
  uint8_t value = 0;
  for (int i = 0; i < 8; ++i) {
    if (onewire_read_bit(pin)) {
      value |= (1U << i);
    }
  }
  return value;
}

bool ds18b20_search(uint8_t roms[][8], int max_sensors, int* out_count) {
  if (!roms || !out_count || max_sensors <= 0) {
    return false;
  }

  int last_discrepancy = 0;
  bool last_device_flag = false;
  int count = 0;

  while (!last_device_flag && count < max_sensors) {
    int id_bit_number = 1;
    int last_zero = 0;
    int rom_byte_number = 0;
    uint8_t rom_byte_mask = 1;
    uint8_t rom[8] = {};

    if (!onewire_reset(g_pin)) {
      break;
    }

    onewire_write_byte(g_pin, kCmdSearchRom);

    while (rom_byte_number < 8) {
      bool id_bit = onewire_read_bit(g_pin);
      bool cmp_id_bit = onewire_read_bit(g_pin);

      if (id_bit && cmp_id_bit) {
        break;  // No devices
      }

      uint8_t search_direction;
      if (id_bit != cmp_id_bit) {
        search_direction = id_bit;
      } else {
        if (id_bit_number < last_discrepancy) {
          search_direction = (rom[rom_byte_number] & rom_byte_mask) ? 1 : 0;
        } else {
          search_direction = (id_bit_number == last_discrepancy);
        }
        if (search_direction == 0) {
          last_zero = id_bit_number;
        }
      }

      if (search_direction) {
        rom[rom_byte_number] |= rom_byte_mask;
      } else {
        rom[rom_byte_number] &= static_cast<uint8_t>(~rom_byte_mask);
      }

      onewire_write_bit(g_pin, search_direction);
      id_bit_number++;
      rom_byte_mask <<= 1;

      if (rom_byte_mask == 0) {
        rom_byte_number++;
        rom_byte_mask = 1;
      }
    }

    if (id_bit_number > 64) {
      last_discrepancy = last_zero;
      if (last_discrepancy == 0) {
        last_device_flag = true;
      }
      std::memcpy(roms[count], rom, 8);
      count++;
    } else {
      break;
    }
  }

  *out_count = count;
  return count > 0;
}

void log_roms() {
  for (int i = 0; i < g_sensor_count; ++i) {
    ESP_LOGI(TAG, "ROM%d: %02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", i,
             g_rom[i][0], g_rom[i][1], g_rom[i][2], g_rom[i][3],
             g_rom[i][4], g_rom[i][5], g_rom[i][6], g_rom[i][7]);
  }
}

}  // namespace

bool Ds18b20Init(gpio_num_t pin) {
  g_pin = pin;
  gpio_config_t cfg = {};
  cfg.mode = GPIO_MODE_OUTPUT_OD;
  cfg.pin_bit_mask = 1ULL << g_pin;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;  // внешний резистор на плате
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  gpio_set_level(g_pin, 1);

  std::memset(g_rom, 0, sizeof(g_rom));
  g_sensor_count = 0;

  // Try READ_ROM if only one device is present, else SEARCH_ROM
  if (onewire_reset(g_pin)) {
    onewire_write_byte(g_pin, kCmdReadRom);
    uint8_t rom[8] = {};
    for (int i = 0; i < 8; ++i) {
      rom[i] = onewire_read_byte(g_pin);
    }
    if (onewire_reset(g_pin)) {
      std::memcpy(g_rom[0], rom, 8);
      g_sensor_count = 1;
    }
  }

  if (g_sensor_count == 0) {
    ds18b20_search(g_rom, kMaxDevices, &g_sensor_count);
  }

  if (g_sensor_count == 0) {
    ESP_LOGW(TAG, "No DS18B20 sensors found on bus");
    return false;
  }

  ESP_LOGI(TAG, "Found %d sensors", g_sensor_count);
  log_roms();
  return true;
}

bool Ds18b20ReadTemperatures(float* out_values, int max_values, int* out_count) {
  if (g_sensor_count == 0 || !out_values || max_values <= 0 || !out_count) {
    return false;
  }
  for (int i = 0; i < max_values; ++i) {
    out_values[i] = NAN;
  }
  *out_count = 0;

  if (!onewire_reset(g_pin)) {
    return false;
  }
  onewire_write_byte(g_pin, kCmdSkipRom);
  onewire_write_byte(g_pin, kCmdConvertT);
  vTaskDelay(kConvertDelayTicks);

  bool any_ok = false;
  const int to_read = (g_sensor_count < max_values) ? g_sensor_count : max_values;
  for (int i = 0; i < to_read; ++i) {
    if (!onewire_reset(g_pin)) {
      continue;
    }
    onewire_write_byte(g_pin, kCmdMatchRom);
    for (int b = 0; b < 8; ++b) {
      onewire_write_byte(g_pin, g_rom[i][b]);
    }
    onewire_write_byte(g_pin, kCmdReadScratchpad);

    uint8_t scratch[9] = {};
    for (int b = 0; b < 9; ++b) {
      scratch[b] = onewire_read_byte(g_pin);
    }

    int16_t raw = static_cast<int16_t>((static_cast<int16_t>(scratch[1]) << 8) | scratch[0]);
    float temp_c = static_cast<float>(raw) / 16.0f;
    out_values[i] = temp_c;
    any_ok = true;
  }
  *out_count = to_read;
  return any_ok;
}

int Ds18b20GetSensorCount() {
  return g_sensor_count;
}
