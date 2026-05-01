#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum class RtcmMessageType : uint16_t {
  kGpsL1L2Observables = 1004,
  kStationAntennaArp = 1006,
  kReceiverAntennaDescriptors = 1033,
};

struct GpsDateTime {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint16_t millisecond = 0;
  bool valid = false;
};

struct RtcmFrame {
  uint16_t message_type = 0;
  std::vector<uint8_t> raw;
  bool crc_ok = false;
};

struct CurrentFrame {
  uint32_t frame_index = 0;
  GpsDateTime timestamp{};
  std::string gpzda_line;
  std::map<uint16_t, RtcmFrame> rtcm_by_type;
};

bool parseZdaLine(const std::string& line, GpsDateTime& out);
bool parseRtcmFrameFromBuffer(std::vector<uint8_t>& buffer, RtcmFrame& out, bool* need_more = nullptr);
uint32_t crc24q(const uint8_t* data, size_t len);
bool checkRtcmCrc(const std::vector<uint8_t>& frame);
uint16_t getRtcmMessageType(const std::vector<uint8_t>& frame);
bool IsExpectedRtcmType(uint16_t type);

class GpsUnicoreClient {
 public:
  esp_err_t initUart();
  esp_err_t startTasks();

  void sendCommand(const std::string& cmd);
  void probeReceiver();
  void configurePeriodicOutput(bool auto_base_config = true);
  void startFrame(uint32_t frame_index);
  void pollFrame();
  void stopFrameOutput();
  bool isCurrentFrameComplete();
  bool finishFrame(CurrentFrame& out);
  bool writeRtcmFramesToFile(const CurrentFrame& frame, FILE* file);

  void uartReadTask();

  bool getLastDateTime(GpsDateTime& out);
  bool getLastRtcm(uint16_t type, RtcmFrame& out);

 private:
  static void ReadTaskThunk(void* arg);

  void handleBytes(const uint8_t* data, size_t len);
  void handleLine(const std::string& line);
  void handleRtcmFrame(const RtcmFrame& frame);
  void storeRtcmLocked(const RtcmFrame& frame);
  bool copyRtcmLocked(uint16_t type, RtcmFrame& out) const;

  SemaphoreHandle_t data_mutex_ = nullptr;
  TaskHandle_t read_task_ = nullptr;

  std::vector<uint8_t> rx_buffer_;

  bool current_frame_active_ = false;
  CurrentFrame current_frame_{};

  GpsDateTime last_datetime_{};
  bool has_datetime_ = false;

  RtcmFrame last_1004_{};
  RtcmFrame last_1006_{};
  RtcmFrame last_1033_{};
  bool has_1004_ = false;
  bool has_1006_ = false;
  bool has_1033_ = false;
};
