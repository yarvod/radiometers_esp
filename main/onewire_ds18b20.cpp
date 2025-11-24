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

constexpr int kMaxDevices = 2;
constexpr TickType_t kConvertDelayTicks = pdMS_TO_TICKS(750);  // Max conversion time for 12-bit

const char* TAG = "DS18B20";

gpio_num_t ow_pin = GPIO_NUM_NC;
int device_count = 0;
uint8_t device_roms[kMaxDevices][8] = {};

inline void ow_drive_low() {
  gpio_set_direction(ow_pin, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(ow_pin, 0);
}

inline void ow_release() {
  gpio_set_direction(ow_pin, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(ow_pin, 1);
}

bool ow_reset() {
  ow_drive_low();
  esp_rom_delay_us(480);
  ow_release();
  esp_rom_delay_us(70);
  int presence = gpio_get_level(ow_pin) == 0;
  esp_rom_delay_us(410);
  return presence;
}

void ow_write_bit(uint8_t bit) {
  if (bit) {
    ow_drive_low();
    esp_rom_delay_us(6);
    ow_release();
    esp_rom_delay_us(64);
  } else {
    ow_drive_low();
    esp_rom_delay_us(60);
    ow_release();
    esp_rom_delay_us(10);
  }
}

bool ow_read_bit() {
  ow_drive_low();
  esp_rom_delay_us(6);
  ow_release();
  esp_rom_delay_us(9);
  bool bit = gpio_get_level(ow_pin);
  esp_rom_delay_us(55);
  return bit;
}

void ow_write_byte(uint8_t data) {
  for (int i = 0; i < 8; ++i) {
    ow_write_bit(data & 0x01);
    data >>= 1;
  }
}

uint8_t ow_read_byte() {
  uint8_t value = 0;
  for (int i = 0; i < 8; ++i) {
    if (ow_read_bit()) {
      value |= (1U << i);
    }
  }
  return value;
}

bool ow_search_rom(uint8_t* new_rom, int* last_discrepancy, bool* last_device_flag) {
  if (!new_rom || !last_discrepancy || !last_device_flag) {
    return false;
  }
  if (*last_device_flag) {
    return false;
  }

  int id_bit_number = 1;
  int last_zero = 0;
  int rom_byte_number = 0;
  uint8_t rom_byte_mask = 1;
  bool search_result = false;

  if (!ow_reset()) {
    *last_discrepancy = 0;
    *last_device_flag = false;
    return false;
  }

  ow_write_byte(kCmdSearchRom);

  while (rom_byte_number < 8) {
    bool id_bit = ow_read_bit();
    bool cmp_id_bit = ow_read_bit();

    if (id_bit && cmp_id_bit) {
      break;  // No devices
    }

    uint8_t search_direction;
    if (id_bit != cmp_id_bit) {
      search_direction = id_bit;  // Only one possibility
    } else {
      if (id_bit_number < *last_discrepancy) {
        search_direction = (new_rom[rom_byte_number] & rom_byte_mask) ? 1 : 0;
      } else {
        search_direction = (id_bit_number == *last_discrepancy);
      }
      if (search_direction == 0) {
        last_zero = id_bit_number;
      }
    }

    if (search_direction) {
      new_rom[rom_byte_number] |= rom_byte_mask;
    } else {
      new_rom[rom_byte_number] &= static_cast<uint8_t>(~rom_byte_mask);
    }

    ow_write_bit(search_direction);
    id_bit_number++;
    rom_byte_mask <<= 1;

    if (rom_byte_mask == 0) {
      rom_byte_number++;
      rom_byte_mask = 1;
    }
  }

  if (id_bit_number > 64) {
    *last_discrepancy = last_zero;
    if (*last_discrepancy == 0) {
      *last_device_flag = true;
    }
    search_result = true;
  }

  if (!search_result) {
    *last_discrepancy = 0;
    *last_device_flag = false;
  }
  return search_result;
}

}  // namespace

bool Ds18b20Init(gpio_num_t pin) {
  ow_pin = pin;
  gpio_config_t cfg = {};
  cfg.mode = GPIO_MODE_OUTPUT_OD;
  cfg.pin_bit_mask = 1ULL << ow_pin;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  gpio_set_level(ow_pin, 1);

  device_count = 0;
  int last_discrepancy = 0;
  bool last_device_flag = false;
  uint8_t rom[8] = {};

  while (!last_device_flag && device_count < kMaxDevices) {
    std::memset(rom, 0, sizeof(rom));
    if (!ow_search_rom(rom, &last_discrepancy, &last_device_flag)) {
      break;
    }
    std::memcpy(device_roms[device_count], rom, sizeof(rom));
    device_count++;
  }

  if (device_count == 0) {
    ESP_LOGW(TAG, "No DS18B20 sensors found on bus");
    return false;
  }
  ESP_LOGI(TAG, "DS18B20 sensors found: %d", device_count);
  return true;
}

int Ds18b20GetSensorCount() {
  return device_count;
}

bool Ds18b20ReadTemperatures(float* out_t1, float* out_t2) {
  if (device_count == 0) {
    return false;
  }
  if (out_t1) *out_t1 = NAN;
  if (out_t2) *out_t2 = NAN;

  if (!ow_reset()) {
    return false;
  }
  ow_write_byte(kCmdSkipRom);
  ow_write_byte(kCmdConvertT);
  vTaskDelay(kConvertDelayTicks);

  bool any_ok = false;
  for (int i = 0; i < device_count && i < kMaxDevices; ++i) {
    if (!ow_reset()) {
      continue;
    }
    ow_write_byte(kCmdMatchRom);
    for (int b = 0; b < 8; ++b) {
      ow_write_byte(device_roms[i][b]);
    }
    ow_write_byte(kCmdReadScratchpad);

    uint8_t scratch[9] = {};
    for (int b = 0; b < 9; ++b) {
      scratch[b] = ow_read_byte();
    }

    int16_t raw = static_cast<int16_t>((static_cast<int16_t>(scratch[1]) << 8) | scratch[0]);
    float temp_c = static_cast<float>(raw) / 16.0f;
    if (i == 0 && out_t1) {
      *out_t1 = temp_c;
    } else if (i == 1 && out_t2) {
      *out_t2 = temp_c;
    }
    any_ok = true;
  }
  return any_ok;
}
