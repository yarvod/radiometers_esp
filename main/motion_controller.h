#pragma once

#include <cstdint>
#include <string>

#include "app_state.h"

// Initialization — call from app_main after GPIO config is done.
void MotionControllerInit();

// Create StepperTask and PidTask.
void MotionControllerStartTasks();

// Hall sensor
bool IsHallTriggered();

// External module power switch
void SetExternalPower(bool enabled);
bool CycleExternalPower(uint32_t off_ms);

// GPS antenna short detect (GPIO read only — will move to gps_module in Phase 8)
int  GetGpsAntennaShortRaw();
bool IsGpsAntennaShort();

// Heater / fan PWM control
void HeaterSetPowerPercent(float percent);
void FanSetPowerPercent(float percent);

// Stepper primitives
void EnableStepper();
void DisableStepper();
void StopStepper();
void StartStepperMove(int steps, bool forward, int speed_us);

// Calibration
void CalibrateZero();
void CalibrationTask(void*);

// Find-zero (homing)
void FindZeroTask(void*);
bool StartFindZeroTask(std::string* out_message);

// Logging session control
void StopLogging();
bool StartLoggingToFile(const std::string& postfix_raw, UsbMode current_usb_mode);

// Register a callback for publishing measurement payloads (injected to avoid dep on mqtt_bridge).
using MeasurementPublishFn = void (*)(const std::string& payload);
void MotionControllerSetPublisher(MeasurementPublishFn fn);
