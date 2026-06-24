#pragma once

#include "esp_err.h"

// Call once before any ADC or Ethernet SPI use; idempotent.
esp_err_t InitSpiBus();

// Ensures gpio_install_isr_service() has been called (idempotent, thread-safe).
void EnsureGpioIsrServiceInstalled();

// Configure fan-tachometer GPIO pins and attach ISR handlers.
void SensorHubInitGpios();

// Initialize SPI bus + all three LTC2440 ADC devices.
esp_err_t SensorHubInitAdcs();

// Initialize (or reinitialize) the INA219 power monitor over I2C.
esp_err_t SensorHubInitIna();

// Block until at least one temperature sensor is detected, or timeout expires.
bool WaitForTempSensors(int timeout_ms);

// Read all three ADC channels.
esp_err_t ReadAllAdc(float* v1, float* v2, float* v3);

// Create ADC, INA219, fan-tach, and temperature FreeRTOS tasks.
// ina_ok: skip Ina219Task if false; temp_ok: skip TempTask if false.
void SensorHubStartTasks(bool ina_ok, bool temp_ok);
