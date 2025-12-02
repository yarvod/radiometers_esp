#pragma once

#include <cstdint>  // int32_t

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class LTC2440 {
 public:
  explicit LTC2440(gpio_num_t chip_select_pin, gpio_num_t drdy_pin = GPIO_NUM_NC);

  // Initialize device on already-configured SPI bus.
  esp_err_t Init(spi_host_device_t host, int clock_hz = 100'000);  // я бы по умолчанию <= 2.5 MHz

  // Read value with offset applied (24-bit, sign-extended, shifted by 5 bits).
  esp_err_t Read(int32_t* value);

  // Compute and store offset using a moving average over given samples.
  esp_err_t Tare(int samples, int delay_ms);

  int32_t offset() const { return adc_offset_; }

 private:
  esp_err_t WaitReady_(TickType_t timeout_ticks);
  esp_err_t ReadRaw_(int32_t* value);

  spi_device_handle_t spi_handle_{nullptr};
  gpio_num_t chip_select_pin_;
  gpio_num_t drdy_pin_;
  int32_t adc_offset_ = 0;
  bool initialized_ = false;
};