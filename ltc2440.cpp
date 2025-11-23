/*
 * ltc2440.c

 *
 *  Created on: 7 Jul 2017
 *      Author: Lukas Martisiunas
 *
 */
#include "ltc2440.h"

#include "Arduino.h"
#include "SPI.h"

// --- Внутренние методы класса ---
int32_t LTC2440::SpiRead_() {
  int32_t result = 0;
  int32_t b;

  if (!use_softspi_) {
    SPI.beginTransaction(SPISettings(3000000, MSBFIRST, SPI_MODE0));
    digitalWrite(chip_select_pin_, LOW);  // select
    delayMicroseconds(10);

    byte sign = 0;
    b = SPI.transfer(0xff);  // B3
    if (b >> 4 == 0b0010 || b >> 4 == 0b0001) {
      if ((b & 0x20) == 0) {
        sign = 1;
      }
      b &= 0x1f;
      result = b;
      result = result << 8;
      b = SPI.transfer(0xff);  // B2
      result |= b;
      result = result << 8;
      b = SPI.transfer(0xff);  // B1
      result |= b;
      result = result << 8;
      b = SPI.transfer(0xff);  // B0
      result |= b;
      digitalWrite(chip_select_pin_, HIGH);  // deselect
      SPI.endTransaction();

      if (sign) result |= 0xf0000000;
      return (result);
    } else {
      digitalWrite(chip_select_pin_, HIGH);
      SPI.endTransaction();
      Serial.print("First 8 bits (first 4 should be 0010 for + or 0001 for -): ");
      Serial.println(b, BIN);
      return (0);
    }
  } else {
    // Soft SPI path: use custom SCK/MISO pins
    // Optional: wait until EOC (MISO goes LOW) with timeout
    unsigned long start = millis();
    while (digitalRead(miso_pin_) == HIGH) {
      if (millis() - start > 500) break;  // timeout 500ms
      delay(1);
    }

    digitalWrite(chip_select_pin_, LOW);  // select
    delayMicroseconds(10);

    // Read 4 bytes MSB first
    byte sign = 0;
    b = softSpiRead8_();  // B3
    if (b >> 4 == 0b0010 || b >> 4 == 0b0001) {
      if ((b & 0x20) == 0) {
        sign = 1;
      }
      b &= 0x1f;
      result = b;
      result = (result << 8) | softSpiRead8_(); // B2
      result = (result << 8) | softSpiRead8_(); // B1
      result = (result << 8) | softSpiRead8_(); // B0

      digitalWrite(chip_select_pin_, HIGH);  // deselect

      if (sign) result |= 0xf0000000;
      return (result);
    } else {
      digitalWrite(chip_select_pin_, HIGH);
      Serial.print("First 8 bits (first 4 should be 0010 for + or 0001 for -): ");
      Serial.println(b, BIN);
      return (0);
    }
  }
}

inline uint8_t LTC2440::softSpiRead8_() {
  uint8_t v = 0;
  // SPI mode 0: sample on rising edge, idle low
  for (uint8_t i = 0; i < 8; i++) {
    v <<= 1;
    digitalWrite(sck_pin_, HIGH);
    // small hold time
    // delayMicroseconds(1);
    if (digitalRead(miso_pin_)) v |= 0x01;
    digitalWrite(sck_pin_, LOW);
  }
  return v;
}

LTC2440::LTC2440(uint8_t chip_select_pin) : chip_select_pin_(chip_select_pin) {}
LTC2440::LTC2440(uint8_t chip_select_pin, int8_t sck_pin, int8_t miso_pin)
    : chip_select_pin_(chip_select_pin), sck_pin_(sck_pin), miso_pin_(miso_pin), use_softspi_(true) {}

int32_t LTC2440::Read() {
  /*
  Don't call faster than the sampling LTC2440 sampling rate because I didn't
  implement the thing that checks for ready data
  */
  int32_t read_adc_val = SpiRead_();
  read_adc_val = read_adc_val >> 5;  // truncate lowest 5 bits
  return read_adc_val - adc_offset_;
}

void LTC2440::Init() {
  pinMode(chip_select_pin_, OUTPUT);
  // chip select is active low
  digitalWrite(chip_select_pin_, HIGH);

  if (!use_softspi_) {
    SPI.begin();
  } else {
    // Setup software SPI pins
    pinMode(sck_pin_, OUTPUT);
    digitalWrite(sck_pin_, LOW); // idle low
    pinMode(miso_pin_, INPUT_PULLUP);
  }

  // vestiges of an old library - two dummy reads to sync
  char temp_2 = SpiRead_();
  temp_2 = SpiRead_();
}

void LTC2440::Tare(int samples, int millis_delay) {
  int32_t average_reading = 0;
  for (int i = 0; i < samples; i++) {
    average_reading += Read() / float(samples);
    delay(millis_delay);
  }
  adc_offset_ = average_reading;
}
