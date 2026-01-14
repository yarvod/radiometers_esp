#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TimerCallbackFunction_t)(TimerHandle_t);

TimerHandle_t xTimerCreate(const char* name, TickType_t period, int auto_reload, void* timer_id,
                           TimerCallbackFunction_t callback);
int xTimerStart(TimerHandle_t timer, TickType_t ticks);
int xTimerStop(TimerHandle_t timer, TickType_t ticks);
int xTimerChangePeriod(TimerHandle_t timer, TickType_t new_period, TickType_t ticks);
