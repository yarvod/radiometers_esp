#pragma once

#include <cstdint>
#include <string>

enum class ErrorCode : uint8_t {
  kWifiDisconnected = 0,
  kWifiFallback = 1,
  kUsbMscInit = 2,
  kTempSensor = 3,
  kAdcRead = 4,
  kInaInit = 5,
  kMqttDisconnected = 6,
  kTimeSync = 7,
  kSdMount = 8,
  kLogTaskStack = 9,
  kInaRead = 10,
  kSdMutex = 11,
  kMax
};

enum class ErrorSeverity : uint8_t {
  kInfo = 0,
  kWarning = 1,
  kError = 2,
  kCritical = 3,
};

using ErrorPublishFn = void (*)(const std::string& payload);

void ErrorManagerInit();
void ErrorManagerSetPublisher(ErrorPublishFn publisher);
void ErrorManagerSet(ErrorCode code, ErrorSeverity severity, const std::string& message);
void ErrorManagerClear(ErrorCode code);
const char* ErrorCodeToString(ErrorCode code);
const char* ErrorSeverityToString(ErrorSeverity severity);
