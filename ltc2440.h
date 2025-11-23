/*
 * ltc2440.h
 *
 *  Created on: Oct 2020
 *      Author: Nathan Kau, heavily based on code by Lukas Martisiunas
 *      Description: Basic library to initialise and use LTC2440 with Arduino
 */
#ifndef LTC2440_H
#define LTC2440_H

#include "Arduino.h"

class LTC2440 {
 private:
  uint8_t chip_select_pin_;
  int8_t sck_pin_ = -1;
  int8_t miso_pin_ = -1;
  bool use_softspi_ = false;
  int32_t adc_offset_ = 0;

  int32_t SpiRead_();
  inline uint8_t softSpiRead8_();

 public:
  // HW SPI (uses board's default SPI pins)
  LTC2440(uint8_t chip_select_pin);
  // Soft SPI on custom pins (SCK/MISO). MOSI не требуется для LTC2440
  LTC2440(uint8_t chip_select_pin, int8_t sck_pin, int8_t miso_pin);
  int32_t Read();
  void Tare(int samples, int millis_delay);
  void Init();
};

#endif
