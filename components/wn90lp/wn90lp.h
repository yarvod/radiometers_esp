#pragma once

#include "app_state.h"  // MeteoData is defined there; wn90lp.h uses it for getData() and internal storage

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Write one CSV row to {ActiveStorageMountPoint()}/meteo_YYYYMMDD_HHMMSS_<bootId>.txt.
// Rotates hourly: completed file is moved to ActiveToUploadDir() for S3 upload.
// Works with both SD and internal flash backends.
bool AppendMeteoLog(const MeteoData& d);

// Driver for WN90LP weather station via Modbus RTU over RS485.
// UART_NUM_1, 9600 8N1, RTS-controlled half-duplex (hw_pins.h: METEO_RS485_*).
//
// The station is an optional removable module: if it does not respond,
// poll() returns false and MeteoData::online stays false — no crash.
class Wn90lpClient {
 public:
  // Initialise UART peripheral. Always succeeds even if station is absent.
  esp_err_t initUart();

  // Spawn the polling/logging FreeRTOS task.
  esp_err_t startTask();

  // Send one Modbus request and parse the response.
  // Returns true on valid reply, false on timeout / CRC error / absent station.
  bool poll();

  // Thread-safe copy of the last known reading.
  MeteoData getData() const;

 private:
  static void TaskThunk(void* arg);
  void taskLoop();

  static uint16_t crc16Modbus(const uint8_t* data, size_t len);
  bool sendRequest();
  bool readResponse(uint8_t* buf, size_t expected_len);
  bool parseResponse(const uint8_t* buf, MeteoData& out);

  SemaphoreHandle_t mutex_ = nullptr;
  MeteoData last_{};
};
