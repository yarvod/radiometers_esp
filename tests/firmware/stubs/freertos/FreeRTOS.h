#pragma once

#include <cstdint>

typedef void* SemaphoreHandle_t;
typedef void* TimerHandle_t;
using TickType_t = uint32_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
