#include <iostream>
#include <string>

#include "error_manager.h"
#include "test_stubs.h"

namespace {

int failures = 0;
std::string last_payload;

void CapturePayload(const std::string& payload) {
  last_payload = payload;
}

void ResetStubs() {
  stub_time_us = 0;
  stub_led_red = -1;
  stub_led_green = -1;
  stub_timer_period_ms = 0;
  stub_timer_started = 0;
  last_payload.clear();
}

void ClearAllErrors() {
  for (int i = 0; i < static_cast<int>(ErrorCode::kMax); ++i) {
    ErrorManagerClear(static_cast<ErrorCode>(i));
  }
}

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    failures++;
  }
}

void TestInitShowsGreen() {
  ResetStubs();
  ErrorManagerSetPublisher(&CapturePayload);
  ErrorManagerInit();
  ClearAllErrors();
  Check(stub_led_red == 0, "LED red off after init");
  Check(stub_led_green == 1, "LED green on after init");
}

void TestSetPublishesAndBlink() {
  ResetStubs();
  ErrorManagerSetPublisher(&CapturePayload);
  ErrorManagerSet(ErrorCode::kTimeSync, ErrorSeverity::kWarning, "NTP sync timed out");
  Check(last_payload.find("\"code\":\"time_sync\"") != std::string::npos, "payload has code");
  Check(last_payload.find("\"severity\":\"warning\"") != std::string::npos, "payload has severity");
  Check(last_payload.find("\"active\":true") != std::string::npos, "payload has active true");
  Check(stub_timer_period_ms == 1500, "blink period for time sync is 1500ms");
  Check(stub_timer_started == 1, "timer started on error");
}

void TestPrioritySelection() {
  ResetStubs();
  ErrorManagerSetPublisher(&CapturePayload);
  ErrorManagerSet(ErrorCode::kTimeSync, ErrorSeverity::kWarning, "NTP sync timed out");
  Check(stub_timer_period_ms == 1500, "time sync period set");
  ErrorManagerSet(ErrorCode::kAdcRead, ErrorSeverity::kError, "ADC1 read failed");
  Check(stub_timer_period_ms == 200, "adc read overrides with faster blink");
  ErrorManagerClear(ErrorCode::kAdcRead);
  Check(stub_timer_period_ms == 1500, "reverts to time sync after adc clear");
}

void TestClearPublishes() {
  ResetStubs();
  ErrorManagerSetPublisher(&CapturePayload);
  ErrorManagerSet(ErrorCode::kMqttDisconnected, ErrorSeverity::kWarning, "MQTT disconnected");
  ErrorManagerClear(ErrorCode::kMqttDisconnected);
  Check(last_payload.find("\"active\":false") != std::string::npos, "payload has active false on clear");
}

}  // namespace

int main() {
  TestInitShowsGreen();
  TestSetPublishesAndBlink();
  TestPrioritySelection();
  TestClearPublishes();

  if (failures == 0) {
    std::cout << "OK: all firmware unit tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
