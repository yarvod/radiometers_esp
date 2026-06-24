#pragma once

#include "driver/gpio.h"

bool M1820Init(gpio_num_t pin);
int M1820GetSensorCount();
bool M1820ReadTemperatures(float* out_values, int max_values, int* out_count);
int M1820GetAddresses(uint64_t* out_values, int max_values);
