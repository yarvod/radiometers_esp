#include "ltc2440.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

namespace {
constexpr char TAG[] = "LTC2440";
}

LTC2440::LTC2440(gpio_num_t chip_select_pin, gpio_num_t drdy_pin)
    : chip_select_pin_(chip_select_pin), drdy_pin_(drdy_pin) {}

esp_err_t LTC2440::Init(spi_host_device_t host, int clock_hz) {
  // Manual CS control so we can hold it low while monitoring DRDY on SDO.
  gpio_set_direction(chip_select_pin_, GPIO_MODE_OUTPUT);
  gpio_set_level(chip_select_pin_, 1);

  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = clock_hz;
  devcfg.mode = 0;
  devcfg.spics_io_num = -1;
  devcfg.queue_size = 1;
  devcfg.cs_ena_posttrans = 2;  // hold CS briefly after transfer

  esp_err_t ret = spi_bus_add_device(host, &devcfg, &spi_handle_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // Настроим DRDY пин, если он задан
  if (drdy_pin_ != GPIO_NUM_NC) {
    gpio_config_t io_conf{};
    io_conf.pin_bit_mask = 1ULL << drdy_pin_;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;     // обычно DRDY open-drain / с внешней подтяжкой
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "gpio_config failed for DRDY: %s", esp_err_to_name(ret));
      return ret;
    }
  }

  initialized_ = true;
  return ESP_OK;
}

esp_err_t LTC2440::WaitReady_(TickType_t timeout_ticks) {
  if (drdy_pin_ == GPIO_NUM_NC) {
    return ESP_OK;
  }
  const TickType_t start = xTaskGetTickCount();
  while (gpio_get_level(drdy_pin_) != 0) {
    if ((xTaskGetTickCount() - start) > timeout_ticks) {
      return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return ESP_OK;
}

esp_err_t LTC2440::ReadRaw_(int32_t* value) {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  // Enforce minimum conversion time before polling DRDY; LTC2440 full conversion
  // can take up to ~150 ms depending on OSR. Guard with a fixed delay since we
  // don't always trust DRDY on a shared MISO line.
  const int64_t now_us = esp_timer_get_time();
  const int64_t since_last = now_us - last_conv_start_us_;
  if (last_conv_start_us_ > 0 && since_last < 180'000) {
    const int64_t wait_us = 180'000 - since_last;
    vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));  // ceil to ms ticks
  }

  // Lock the bus while we manually drive CS for DRDY and the transfer.
  esp_err_t lock = spi_device_acquire_bus(spi_handle_, portMAX_DELAY);
  if (lock != ESP_OK) {
    return lock;
  }

  // Pull CS low so the ADC can present DRDY (EOC) on SDO.
  gpio_set_level(chip_select_pin_, 0);

  // Wait until conversion complete (DOUT/SDO goes low) to avoid reading 0xFF headers.
  esp_err_t ready = WaitReady_(pdMS_TO_TICKS(800));
  if (ready != ESP_OK) {
    // Try to proceed anyway after a brief delay; sometimes DRDY not seen on shared MISO.
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  // Give a short guard time after ready before clocking out bits.
  vTaskDelay(pdMS_TO_TICKS(2));

  uint8_t tx[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t rx[4] = {0, 0, 0, 0};

  spi_transaction_t transaction = {};
  transaction.length = 32;
  transaction.tx_buffer = tx;
  transaction.rx_buffer = rx;

  const int kMaxAttempts = 8;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    esp_err_t ret = spi_device_transmit(spi_handle_, &transaction);
    if (ret != ESP_OK) {
      gpio_set_level(chip_select_pin_, 1);
      spi_device_release_bus(spi_handle_);
      return ret;
    }

    uint8_t header = rx[0];
    uint8_t prefix = header >> 4;
    if (prefix == 0b0010 || prefix == 0b0001) {
      bool negative = (header & 0x20) == 0;
      header &= 0x1F;

      int32_t result = (static_cast<int32_t>(header) << 24) |
                       (static_cast<int32_t>(rx[1]) << 16) |
                       (static_cast<int32_t>(rx[2]) << 8)  |
                        static_cast<int32_t>(rx[3]);

      if (negative) {
        result |= 0xF0000000;
      }

      *value = result >> 5;  // drop lowest 5 bits as in reference driver
      gpio_set_level(chip_select_pin_, 1);
      last_conv_start_us_ = esp_timer_get_time();  // conversion starts when CS goes high
      spi_device_release_bus(spi_handle_);
      return ESP_OK;
    }

    // Busy/invalid header: raise CS, wait briefly, and retry
    if (attempt < kMaxAttempts - 1) {
      gpio_set_level(chip_select_pin_, 1);
      // 0xFF or 0x3x often means "not ready yet" on a shared MISO line; wait longer.
      const TickType_t wait_ms = (header == 0xFF || prefix == 0b0011) ? 10 : 5;
      vTaskDelay(pdMS_TO_TICKS(wait_ms));
      gpio_set_level(chip_select_pin_, 0);
      (void)WaitReady_(pdMS_TO_TICKS(200));
      continue;
    } else {
      ESP_LOGW(TAG, "Unexpected header 0x%02X", rx[0]);
    }
  }

  gpio_set_level(chip_select_pin_, 1);
  last_conv_start_us_ = esp_timer_get_time();
  spi_device_release_bus(spi_handle_);
  return ESP_ERR_INVALID_RESPONSE;
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
