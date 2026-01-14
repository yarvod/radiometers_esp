#pragma once

#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks);
void xSemaphoreGive(SemaphoreHandle_t mutex);
