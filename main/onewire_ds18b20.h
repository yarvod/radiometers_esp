#pragma once

#include "driver/gpio.h"

bool Ds18b20Init(gpio_num_t pin);
int Ds18b20GetSensorCount();
bool Ds18b20ReadTemperatures(float* out_t1, float* out_t2);

