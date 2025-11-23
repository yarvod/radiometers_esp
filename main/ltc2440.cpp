#include "ltc2440.h"

#include <inttypes.h>

#include "esp_log.h"

namespace {
constexpr char TAG[] = "LTC2440";
}

LTC2440::LTC2440(gpio_num_t chip_select_pin) : chip_select_pin_(chip_select_pin) {}

esp_err_t LTC2440::Init(spi_host_device_t host, int clock_hz) {
  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = clock_hz;
  devcfg.mode = 0;
  devcfg.spics_io_num = static_cast<int>(chip_select_pin_);
  devcfg.queue_size = 1;
  devcfg.cs_ena_posttrans = 2;  // hold CS briefly after transfer

  esp_err_t ret = spi_bus_add_device(host, &devcfg, &spi_handle_);
  if (ret == ESP_OK) {
    initialized_ = true;
  } else {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t LTC2440::ReadRaw_(int32_t* value) {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  uint8_t tx[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t rx[4] = {0, 0, 0, 0};

  spi_transaction_t transaction = {};
  transaction.length = 32;
  transaction.tx_buffer = tx;
  transaction.rx_buffer = rx;

  esp_err_t ret = spi_device_transmit(spi_handle_, &transaction);
  if (ret != ESP_OK) {
    return ret;
  }

  uint8_t header = rx[0];
  uint8_t prefix = header >> 4;
  if (prefix != 0b0010 && prefix != 0b0001) {
    ESP_LOGW(TAG, "Unexpected header 0x%02X", header);
    return ESP_ERR_INVALID_RESPONSE;
  }

  bool negative = (header & 0x20) == 0;
  header &= 0x1F;

  int32_t result = (static_cast<int32_t>(header) << 24) |
                   (static_cast<int32_t>(rx[1]) << 16) |
                   (static_cast<int32_t>(rx[2]) << 8) |
                   static_cast<int32_t>(rx[3]);

  if (negative) {
    result |= 0xF0000000;
  }

  *value = result >> 5;  // drop lowest 5 bits as in reference driver
  return ESP_OK;
}

esp_err_t LTC2440::Read(int32_t* value) {
  int32_t raw = 0;
  esp_err_t ret = ReadRaw_(&raw);
  if (ret != ESP_OK) {
    return ret;
  }
  *value = raw - adc_offset_;
  return ESP_OK;
}

esp_err_t LTC2440::Tare(int samples, int delay_ms) {
  if (samples <= 0) {
    return ESP_ERR_INVALID_ARG;
  }

  int64_t acc = 0;
  for (int i = 0; i < samples; ++i) {
    int32_t raw = 0;
    esp_err_t ret = ReadRaw_(&raw);
    if (ret != ESP_OK) {
      return ret;
    }
    acc += raw;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }

  adc_offset_ = static_cast<int32_t>(acc / samples);
  ESP_LOGI(TAG, "Offset calibrated: %" PRId32, adc_offset_);
  return ESP_OK;
}
