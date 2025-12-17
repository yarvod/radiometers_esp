#pragma once

#include "driver/gpio.h"

// ADC pins
inline constexpr gpio_num_t ADC_MISO = GPIO_NUM_4;
inline constexpr gpio_num_t ADC_MOSI = GPIO_NUM_5;
inline constexpr gpio_num_t ADC_SCK = GPIO_NUM_6;
inline constexpr gpio_num_t ADC_CS1 = GPIO_NUM_16;
inline constexpr gpio_num_t ADC_CS2 = GPIO_NUM_15;
inline constexpr gpio_num_t ADC_CS3 = GPIO_NUM_7;

// INA219 pins (I2C)
inline constexpr gpio_num_t INA_SDA = GPIO_NUM_42;
inline constexpr gpio_num_t INA_SCL = GPIO_NUM_41;

// Status LEDs (active high)
inline constexpr gpio_num_t STATUS_LED_RED = GPIO_NUM_45;
inline constexpr gpio_num_t STATUS_LED_GREEN = GPIO_NUM_48;

// Heater
inline constexpr gpio_num_t HEATER_PWM = GPIO_NUM_14;

// Fans
inline constexpr gpio_num_t FAN_PWM = GPIO_NUM_2;
inline constexpr gpio_num_t FAN1_TACH = GPIO_NUM_1;
inline constexpr gpio_num_t FAN2_TACH = GPIO_NUM_21;

// Temperature (1-Wire)
inline constexpr gpio_num_t TEMP_1WIRE = GPIO_NUM_18;

// Hall sensor
inline constexpr gpio_num_t MT_HALL_SEN = GPIO_NUM_3;

// SD card pins (1-bit SDMMC)
inline constexpr gpio_num_t SD_CLK = GPIO_NUM_39;
inline constexpr gpio_num_t SD_CMD = GPIO_NUM_38;
inline constexpr gpio_num_t SD_D0 = GPIO_NUM_40;

// Relay and stepper pins
inline constexpr gpio_num_t RELAY_PIN = GPIO_NUM_17;
inline constexpr gpio_num_t STEPPER_EN = GPIO_NUM_35;
inline constexpr gpio_num_t STEPPER_DIR = GPIO_NUM_36;
inline constexpr gpio_num_t STEPPER_STEP = GPIO_NUM_37;

inline constexpr int ADC_SPI_FREQ_HZ = 100'000;

