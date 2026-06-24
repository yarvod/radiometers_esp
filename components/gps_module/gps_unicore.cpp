#include "gps_unicore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "driver/uart.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_pins.h"

namespace {

constexpr char TAG_GPS[] = "GPS_UM982";
constexpr uart_port_t kGpsUart = UART_NUM_2;
constexpr int kGpsBaud = 115200;
constexpr size_t kRxBufferSize = 8192;
constexpr size_t kParserMaxBuffer = 8192;
constexpr size_t kRtcmMaxPayloadLen = 1023;
constexpr TickType_t kReadTimeout = pdMS_TO_TICKS(100);
constexpr TickType_t kCommandDelay = pdMS_TO_TICKS(150);

bool HexNibble(char c, uint8_t* out) {
  if (!out) return false;
  if (c >= '0' && c <= '9') {
    *out = static_cast<uint8_t>(c - '0');
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    *out = static_cast<uint8_t>(c - 'A' + 10);
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    *out = static_cast<uint8_t>(c - 'a' + 10);
    return true;
  }
  return false;
}

bool ParseNmeaChecksum(const std::string& line, size_t star, uint8_t* out) {
  if (!out || star + 2 >= line.size()) return false;
  uint8_t hi = 0;
  uint8_t lo = 0;
  if (!HexNibble(line[star + 1], &hi) || !HexNibble(line[star + 2], &lo)) {
    return false;
  }
  *out = static_cast<uint8_t>((hi << 4) | lo);
  return true;
}

uint8_t CalcNmeaChecksum(const std::string& line, size_t star) {
  uint8_t cs = 0;
  const size_t start = (!line.empty() && line[0] == '$') ? 1 : 0;
  for (size_t i = start; i < star && i < line.size(); ++i) {
    cs ^= static_cast<uint8_t>(line[i]);
  }
  return cs;
}

std::string TrimLine(const std::string& raw) {
  size_t begin = 0;
  while (begin < raw.size() && (raw[begin] == '\r' || raw[begin] == '\n' || raw[begin] == ' ' || raw[begin] == '\t')) {
    begin++;
  }
  size_t end = raw.size();
  while (end > begin && (raw[end - 1] == '\r' || raw[end - 1] == '\n' || raw[end - 1] == ' ' || raw[end - 1] == '\t')) {
    end--;
  }
  return raw.substr(begin, end - begin);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string ExtractModeFromLine(const std::string& line) {
  const size_t semi = line.find(';');
  if (semi == std::string::npos || semi + 1 >= line.size()) {
    return {};
  }
  size_t end = line.find('*', semi + 1);
  if (end == std::string::npos) {
    end = line.size();
  }
  std::string mode = line.substr(semi + 1, end - semi - 1);
  while (!mode.empty() && (mode.back() == ',' || mode.back() == ' ' || mode.back() == '\t')) {
    mode.pop_back();
  }
  return mode;
}

std::vector<std::string> SplitCsv(const std::string& input) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= input.size()) {
    const size_t comma = input.find(',', start);
    if (comma == std::string::npos) {
      out.push_back(input.substr(start));
      break;
    }
    out.push_back(input.substr(start, comma - start));
    start = comma + 1;
  }
  return out;
}

bool ParseIntField(const std::string& s, int* out) {
  if (!out || s.empty()) return false;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  *out = static_cast<int>(v);
  return true;
}

bool IsDigits(const std::string& s, size_t start, size_t count) {
  if (start + count > s.size()) return false;
  for (size_t i = start; i < start + count; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

uint16_t ParseMilliseconds(const std::string& time_field, size_t dot) {
  if (dot == std::string::npos || dot + 1 >= time_field.size()) {
    return 0;
  }
  uint16_t ms = 0;
  int scale = 100;
  for (size_t i = dot + 1; i < time_field.size() && scale > 0; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(time_field[i]))) break;
    ms += static_cast<uint16_t>((time_field[i] - '0') * scale);
    scale /= 10;
  }
  return ms;
}

bool ParseDoubleField(const std::string& s, double* out) {
  if (!out || s.empty()) return false;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (!end || *end != '\0') return false;
  *out = v;
  return true;
}

bool ParseNmeaDegrees(const std::string& value, const std::string& hemisphere,
                      bool longitude, double* out) {
  if (!out || value.empty() || hemisphere.empty()) return false;
  const size_t dot = value.find('.');
  const size_t min_start = longitude ? 3 : 2;
  if (dot == std::string::npos || dot < min_start) return false;
  const std::string deg_text = value.substr(0, min_start);
  const std::string min_text = value.substr(min_start);
  int deg = 0;
  double minutes = 0.0;
  if (!ParseIntField(deg_text, &deg) || !ParseDoubleField(min_text, &minutes)) {
    return false;
  }
  double sign = 1.0;
  const char hemi = static_cast<char>(std::toupper(static_cast<unsigned char>(hemisphere[0])));
  if (hemi == 'S' || hemi == 'W') {
    sign = -1.0;
  } else if (hemi != 'N' && hemi != 'E') {
    return false;
  }
  *out = sign * (static_cast<double>(deg) + minutes / 60.0);
  return true;
}

void SetNeedMore(bool* need_more, bool value) {
  if (need_more) {
    *need_more = value;
  }
}

std::string HexPreview(const uint8_t* data, size_t len, size_t max_len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  const size_t n = std::min(len, max_len);
  out.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) out.push_back(' ');
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

}  // namespace

bool IsExpectedRtcmType(uint16_t type) {
  return type == static_cast<uint16_t>(RtcmMessageType::kGpsL1L2Observables) ||
         type == static_cast<uint16_t>(RtcmMessageType::kStationAntennaArp) ||
         type == static_cast<uint16_t>(RtcmMessageType::kReceiverAntennaDescriptors);
}

bool parseZdaLine(const std::string& raw_line, GpsDateTime& out) {
  const std::string line = TrimLine(raw_line);
  if (line.empty() || line[0] != '$') {
    return false;
  }

  const size_t star = line.find('*');
  if (star == std::string::npos) {
    return false;
  }

  uint8_t expected = 0;
  if (!ParseNmeaChecksum(line, star, &expected)) {
    return false;
  }
  const uint8_t actual = CalcNmeaChecksum(line, star);
  if (actual != expected) {
    return false;
  }

  const std::string body = line.substr(1, star - 1);
  const std::vector<std::string> fields = SplitCsv(body);
  if (fields.size() < 5) {
    return false;
  }
  if (fields[0].size() < 3 || fields[0].compare(fields[0].size() - 3, 3, "ZDA") != 0) {
    return false;
  }

  const std::string& time_field = fields[1];
  if (time_field.size() < 6 || !IsDigits(time_field, 0, 6)) {
    return false;
  }

  int day = 0;
  int month = 0;
  int year = 0;
  if (!ParseIntField(fields[2], &day) ||
      !ParseIntField(fields[3], &month) ||
      !ParseIntField(fields[4], &year)) {
    return false;
  }

  const int hour = (time_field[0] - '0') * 10 + (time_field[1] - '0');
  const int minute = (time_field[2] - '0') * 10 + (time_field[3] - '0');
  const int second = (time_field[4] - '0') * 10 + (time_field[5] - '0');
  if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
    return false;
  }

  GpsDateTime parsed{};
  parsed.year = static_cast<uint16_t>(year);
  parsed.month = static_cast<uint8_t>(month);
  parsed.day = static_cast<uint8_t>(day);
  parsed.hour = static_cast<uint8_t>(hour);
  parsed.minute = static_cast<uint8_t>(minute);
  parsed.second = static_cast<uint8_t>(second);
  parsed.millisecond = ParseMilliseconds(time_field, time_field.find('.'));
  parsed.valid = true;
  out = parsed;
  return true;
}

bool parseGgaLine(const std::string& raw_line, GpsPosition& out) {
  const std::string line = TrimLine(raw_line);
  if (line.empty() || line[0] != '$') {
    return false;
  }

  const size_t star = line.find('*');
  if (star == std::string::npos) {
    return false;
  }

  uint8_t expected = 0;
  if (!ParseNmeaChecksum(line, star, &expected)) {
    return false;
  }
  const uint8_t actual = CalcNmeaChecksum(line, star);
  if (actual != expected) {
    return false;
  }

  const std::string body = line.substr(1, star - 1);
  const std::vector<std::string> fields = SplitCsv(body);
  if (fields.size() < 10) {
    return false;
  }
  if (fields[0].size() < 3 || fields[0].compare(fields[0].size() - 3, 3, "GGA") != 0) {
    return false;
  }

  int fix_quality = 0;
  int satellites = 0;
  double lat = 0.0;
  double lon = 0.0;
  double alt = 0.0;
  if (!ParseIntField(fields[6], &fix_quality) || fix_quality <= 0 ||
      !ParseIntField(fields[7], &satellites) ||
      !ParseNmeaDegrees(fields[2], fields[3], false, &lat) ||
      !ParseNmeaDegrees(fields[4], fields[5], true, &lon) ||
      !ParseDoubleField(fields[9], &alt)) {
    return false;
  }

  GpsPosition parsed{};
  parsed.latitude_deg = lat;
  parsed.longitude_deg = lon;
  parsed.altitude_m = alt;
  parsed.fix_quality = fix_quality;
  parsed.satellites = satellites;
  parsed.valid = true;
  out = parsed;
  return true;
}

uint32_t crc24q(const uint8_t* data, size_t len) {
  uint32_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint32_t>(data[i]) << 16;
    for (int bit = 0; bit < 8; ++bit) {
      crc <<= 1;
      if (crc & 0x1000000U) {
        crc ^= 0x1864CFBU;
      }
    }
  }
  return crc & 0xFFFFFFU;
}

bool checkRtcmCrc(const std::vector<uint8_t>& frame) {
  if (frame.size() < 6 || frame[0] != 0xD3) {
    return false;
  }
  const uint32_t calc = crc24q(frame.data(), frame.size() - 3);
  const size_t n = frame.size();
  const uint32_t got = (static_cast<uint32_t>(frame[n - 3]) << 16) |
                       (static_cast<uint32_t>(frame[n - 2]) << 8) |
                       static_cast<uint32_t>(frame[n - 1]);
  return calc == got;
}

uint16_t getRtcmMessageType(const std::vector<uint8_t>& frame) {
  if (frame.size() < 5 || frame[0] != 0xD3) {
    return 0;
  }
  return (static_cast<uint16_t>(frame[3]) << 4) | (frame[4] >> 4);
}

bool parseRtcmFrameFromBuffer(std::vector<uint8_t>& buffer, RtcmFrame& out, bool* need_more) {
  SetNeedMore(need_more, false);
  if (buffer.empty()) {
    SetNeedMore(need_more, true);
    return false;
  }

  const auto d3 = std::find(buffer.begin(), buffer.end(), 0xD3);
  if (d3 == buffer.end()) {
    buffer.clear();
    return false;
  }
  if (d3 != buffer.begin()) {
    buffer.erase(buffer.begin(), d3);
  }

  if (buffer.size() < 3) {
    SetNeedMore(need_more, true);
    return false;
  }

  const uint16_t payload_len = (static_cast<uint16_t>(buffer[1] & 0x03) << 8) | buffer[2];
  ESP_LOGD(TAG_GPS, "RTCM candidate header: %s payload_len=%u buffered=%u",
           HexPreview(buffer.data(), std::min<size_t>(buffer.size(), 12), 12).c_str(),
           payload_len, static_cast<unsigned>(buffer.size()));
  if ((buffer[1] & 0xFC) != 0 || payload_len == 0 || payload_len > kRtcmMaxPayloadLen) {
    ESP_LOGW(TAG_GPS, "Bad RTCM length/header: b1=0x%02x b2=0x%02x payload_len=%u buffered=%u",
             buffer[1], buffer[2], payload_len, static_cast<unsigned>(buffer.size()));
    buffer.erase(buffer.begin());
    return false;
  }

  const size_t frame_len = 3 + payload_len + 3;
  if (buffer.size() < frame_len) {
    ESP_LOGD(TAG_GPS, "RTCM waiting full frame: payload_len=%u need=%u have=%u",
             payload_len, static_cast<unsigned>(frame_len), static_cast<unsigned>(buffer.size()));
    SetNeedMore(need_more, true);
    return false;
  }

  std::vector<uint8_t> frame(buffer.begin(), buffer.begin() + frame_len);
  if (!checkRtcmCrc(frame)) {
    ESP_LOGW(TAG_GPS, "bad RTCM CRC, type=%u", getRtcmMessageType(frame));
    buffer.erase(buffer.begin());
    return false;
  }
  if (payload_len < 2) {
    buffer.erase(buffer.begin(), buffer.begin() + frame_len);
    return false;
  }

  RtcmFrame parsed{};
  parsed.raw = std::move(frame);
  parsed.crc_ok = true;
  parsed.message_type = getRtcmMessageType(parsed.raw);
  ESP_LOGD(TAG_GPS, "RTCM frame parsed type=%u payload_len=%u total=%u",
           parsed.message_type, payload_len, static_cast<unsigned>(parsed.raw.size()));
  out = std::move(parsed);
  buffer.erase(buffer.begin(), buffer.begin() + frame_len);
  return true;
}

esp_err_t GpsUnicoreClient::initUart() {
  if (!data_mutex_) {
    data_mutex_ = xSemaphoreCreateMutex();
    if (!data_mutex_) {
      return ESP_ERR_NO_MEM;
    }
  }

  uart_config_t uart_config = {};
  uart_config.baud_rate = kGpsBaud;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_DISABLE;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = 0;
#if ESP_IDF_VERSION_MAJOR >= 5
  uart_config.source_clk = UART_SCLK_DEFAULT;
#endif

  esp_err_t err = uart_driver_install(kGpsUart, kRxBufferSize, 0, 0, nullptr, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG_GPS, "uart_driver_install failed: %s", esp_err_to_name(err));
    return err;
  }

  err = uart_param_config(kGpsUart, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_GPS, "uart_param_config failed: %s", esp_err_to_name(err));
    return err;
  }

  err = uart_set_pin(kGpsUart, GPS_UART2_TX, GPS_UART2_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_GPS, "uart_set_pin failed: %s", esp_err_to_name(err));
    return err;
  }
  uart_flush_input(kGpsUart);
  ESP_LOGI(TAG_GPS, "UART2 init: tx=%d rx=%d baud=%d", GPS_UART2_TX, GPS_UART2_RX, kGpsBaud);
  return ESP_OK;
}

esp_err_t GpsUnicoreClient::startTasks() {
  if (!read_task_) {
    if (xTaskCreatePinnedToCore(&GpsUnicoreClient::ReadTaskThunk, "gps_uart_rx", 8192, this, 3, &read_task_, 0) != pdPASS) {
      return ESP_ERR_NO_MEM;
    }
  }
  return ESP_OK;
}

void GpsUnicoreClient::sendCommand(const std::string& cmd) {
  std::string line = cmd;
  line += "\r\n";
  const int written = uart_write_bytes(kGpsUart, line.data(), line.size());
  if (written != static_cast<int>(line.size())) {
    ESP_LOGW(TAG_GPS, "UART write incomplete for '%s': %d/%u", cmd.c_str(), written, static_cast<unsigned>(line.size()));
  } else {
    ESP_LOGD(TAG_GPS, "TX: %s", cmd.c_str());
  }
}

void GpsUnicoreClient::probeReceiver() {
  static constexpr std::array<const char*, 2> kProbeCommands = {
      "VERSION",
      "MODE",
  };
  for (const char* cmd : kProbeCommands) {
    sendCommand(cmd);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void GpsUnicoreClient::configurePeriodicOutput(const std::vector<uint16_t>& rtcm_types, const std::string& mode) {
  std::vector<uint16_t> sanitized;
  sanitized.reserve(rtcm_types.size());
  for (uint16_t type : rtcm_types) {
    if (type == 0 || type > 4095) {
      continue;
    }
    if (std::find(sanitized.begin(), sanitized.end(), type) == sanitized.end()) {
      sanitized.push_back(type);
    }
  }
  if (sanitized.empty()) {
    sanitized = {1004, 1006, 1033};
  }
  if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    enabled_rtcm_types_ = sanitized;
    xSemaphoreGive(data_mutex_);
  }

  const std::string mode_lower = ToLower(mode);
  if (mode_lower == "base_time_60") {
    ESP_LOGI(TAG_GPS, "Configuring UM982 BASE mode before RTCM stream");
    sendCommand("UNLOG");
    vTaskDelay(pdMS_TO_TICKS(1000));
    sendCommand("MODE BASE TIME 60");
    ESP_LOGI(TAG_GPS, "Waiting 60 seconds for UM982 BASE TIME initialization");
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
  } else if (mode_lower == "base") {
    ESP_LOGI(TAG_GPS, "Configuring UM982 BASE mode");
    sendCommand("UNLOG");
    vTaskDelay(pdMS_TO_TICKS(1000));
    sendCommand("MODE BASE");
    vTaskDelay(pdMS_TO_TICKS(1000));
  } else if (mode_lower == "rover_uav" || mode_lower == "rover") {
    ESP_LOGI(TAG_GPS, "Configuring UM982 ROVER UAV mode");
    sendCommand("UNLOG");
    vTaskDelay(pdMS_TO_TICKS(1000));
    sendCommand("MODE ROVER UAV");
    vTaskDelay(pdMS_TO_TICKS(1000));
  } else {
    ESP_LOGI(TAG_GPS, "Keeping current UM982 mode");
  }

  sendCommand("GPZDA COM2 30");
  vTaskDelay(kCommandDelay);

  ESP_LOGI(TAG_GPS, "Configuring UM982 periodic RTCM3 output on COM2 every 30 seconds");
  for (uint16_t type : sanitized) {
    std::string cmd = "RTCM";
    cmd += std::to_string(type);
    cmd += " COM2 30";
    sendCommand(cmd);
    vTaskDelay(kCommandDelay);
  }
  sendCommand("MODE");
}

void GpsUnicoreClient::startFrame(uint32_t frame_index) {
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  current_frame_ = CurrentFrame{};
  current_frame_.frame_index = frame_index;
  current_frame_active_ = true;
  xSemaphoreGive(data_mutex_);
}

void GpsUnicoreClient::pollFrame() {
  // Periodic output is configured once by configurePeriodicOutput(). Frame
  // collection only listens to the incoming UART stream.
}

void GpsUnicoreClient::stopFrameOutput() {
  // UM982 does not accept period 0 for these logs, and we intentionally do not
  // send UNLOG here so the receiver output configuration is not changed after a frame.
}

bool GpsUnicoreClient::isCurrentFrameComplete() {
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
    return false;
  }
  bool ok = current_frame_active_;
  for (uint16_t type : enabled_rtcm_types_) {
    if (current_frame_.rtcm_by_type.count(type) == 0) {
      ok = false;
      break;
    }
  }
  xSemaphoreGive(data_mutex_);
  return ok;
}

bool GpsUnicoreClient::finishFrame(CurrentFrame& out) {
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }
  out = current_frame_;
  current_frame_active_ = false;
  xSemaphoreGive(data_mutex_);
  ESP_LOGD(TAG_GPS, "Frame %u collected: time=%s gpzda=%s 1006=%s 1033=%s 1004=%s",
           static_cast<unsigned>(out.frame_index),
           out.timestamp.valid ? "yes" : "no",
           out.gpzda_line.empty() ? "no" : "yes",
           out.rtcm_by_type.count(1006) ? "yes" : "no",
           out.rtcm_by_type.count(1033) ? "yes" : "no",
           out.rtcm_by_type.count(1004) ? "yes" : "no");
  return true;
}

bool GpsUnicoreClient::writeRtcmFramesToFile(const CurrentFrame& frame, FILE* file) {
  if (!file) return false;

  bool ok = true;
  auto write_one = [&](uint16_t type) {
    const auto it = frame.rtcm_by_type.find(type);
    if (it == frame.rtcm_by_type.end()) {
      return;
    }
    const RtcmFrame& rtcm = it->second;
    if (!rtcm.crc_ok || rtcm.raw.empty()) {
      ESP_LOGW(TAG_GPS, "skip RTCM type %u for .rtcm3: bad CRC or empty frame", type);
      return;
    }
    if (fwrite(rtcm.raw.data(), 1, rtcm.raw.size(), file) != rtcm.raw.size()) {
      ESP_LOGE(TAG_GPS, "Failed to write RTCM%u raw frame", type);
      ok = false;
      return;
    }
    fflush(file);
    ESP_LOGD(TAG_GPS, "RTCM%u raw frame written, size=%u", type, static_cast<unsigned>(rtcm.raw.size()));
  };

  static constexpr std::array<uint16_t, 3> kPriorityOrder = {1006, 1033, 1004};
  for (uint16_t type : kPriorityOrder) {
    if (!ok) break;
    write_one(type);
  }
  for (const auto& entry : frame.rtcm_by_type) {
    if (!ok) break;
    const uint16_t type = entry.first;
    if (type == 1004 || type == 1006 || type == 1033) {
      continue;
    }
    write_one(type);
  }
  return ok;
}

void GpsUnicoreClient::uartReadTask() {
  std::array<uint8_t, 1024> tmp{};
  while (true) {
    const int n = uart_read_bytes(kGpsUart, tmp.data(), tmp.size(), kReadTimeout);
    if (n > 0) {
      handleBytes(tmp.data(), static_cast<size_t>(n));
    }
  }
}

bool GpsUnicoreClient::getLastDateTime(GpsDateTime& out) {
  return getLastDateTime(out, nullptr);
}

bool GpsUnicoreClient::getLastDateTime(GpsDateTime& out, int64_t* received_us) {
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  const bool ok = has_datetime_;
  if (ok) {
    out = last_datetime_;
    if (received_us) {
      *received_us = last_datetime_received_us_;
    }
  }
  xSemaphoreGive(data_mutex_);
  return ok;
}

bool GpsUnicoreClient::getLastPosition(GpsPosition& out) {
  return getLastPosition(out, nullptr);
}

bool GpsUnicoreClient::getLastPosition(GpsPosition& out, int64_t* received_us) {
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  const bool ok = has_position_;
  if (ok) {
    out = last_position_;
    if (received_us) {
      *received_us = last_position_received_us_;
    }
  }
  xSemaphoreGive(data_mutex_);
  return ok;
}

bool GpsUnicoreClient::getLastRtcm(uint16_t type, RtcmFrame& out) {
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  const bool ok = copyRtcmLocked(type, out);
  xSemaphoreGive(data_mutex_);
  return ok;
}

bool GpsUnicoreClient::getCurrentMode(std::string& out) {
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  const bool ok = has_current_mode_;
  if (ok) {
    out = current_mode_;
  }
  xSemaphoreGive(data_mutex_);
  return ok;
}

bool GpsUnicoreClient::getCurrentMode(char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return false;
  }
  out[0] = '\0';
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  const bool ok = has_current_mode_;
  if (ok) {
    std::snprintf(out, out_len, "%s", current_mode_.c_str());
  }
  xSemaphoreGive(data_mutex_);
  return ok;
}

void GpsUnicoreClient::ReadTaskThunk(void* arg) {
  static_cast<GpsUnicoreClient*>(arg)->uartReadTask();
}

void GpsUnicoreClient::handleBytes(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  const bool chunk_has_d3 = std::find(data, data + len, 0xD3) != data + len;
  if (chunk_has_d3) {
    ESP_LOGD(TAG_GPS, "RX chunk contains RTCM preamble 0xD3, bytes=%u parser_buffer_before=%u",
             static_cast<unsigned>(len), static_cast<unsigned>(rx_buffer_.size()));
  }
  if (rx_buffer_.size() + len > kParserMaxBuffer) {
    ESP_LOGW(TAG_GPS, "RX parser buffer overflow, clearing %u bytes", static_cast<unsigned>(rx_buffer_.size()));
    rx_buffer_.clear();
  }
  rx_buffer_.insert(rx_buffer_.end(), data, data + len);

  while (!rx_buffer_.empty()) {
    auto dollar = std::find(rx_buffer_.begin(), rx_buffer_.end(), '$');
    auto hash = std::find(rx_buffer_.begin(), rx_buffer_.end(), '#');
    auto d3 = std::find(rx_buffer_.begin(), rx_buffer_.end(), 0xD3);
    if (dollar == rx_buffer_.end() && hash == rx_buffer_.end() && d3 == rx_buffer_.end()) {
      ESP_LOGD(TAG_GPS, "Dropping UART noise bytes=%u without '$', '#', or RTCM 0xD3", static_cast<unsigned>(rx_buffer_.size()));
      rx_buffer_.clear();
      return;
    }
    auto next = d3;
    if (dollar != rx_buffer_.end() && (d3 == rx_buffer_.end() || dollar < d3)) {
      next = dollar;
    }
    if (hash != rx_buffer_.end() && (next == rx_buffer_.end() || hash < next)) {
      next = hash;
    }
    if (next != rx_buffer_.begin()) {
      rx_buffer_.erase(rx_buffer_.begin(), next);
    }

    if (rx_buffer_.empty()) {
      return;
    }
    if (rx_buffer_[0] == '$' || rx_buffer_[0] == '#') {
      const auto newline = std::find(rx_buffer_.begin(), rx_buffer_.end(), '\n');
      const auto early_d3 = std::find(rx_buffer_.begin() + 1, rx_buffer_.end(), 0xD3);
      if (early_d3 != rx_buffer_.end() && (newline == rx_buffer_.end() || early_d3 < newline)) {
        rx_buffer_.erase(rx_buffer_.begin(), early_d3);
        continue;
      }
      if (newline == rx_buffer_.end()) {
        break;
      }
      std::string line(rx_buffer_.begin(), newline + 1);
      rx_buffer_.erase(rx_buffer_.begin(), newline + 1);
      handleLine(line);
      continue;
    }

    RtcmFrame frame;
    bool need_more = false;
    if (parseRtcmFrameFromBuffer(rx_buffer_, frame, &need_more)) {
      handleRtcmFrame(frame);
      continue;
    }
    if (need_more) {
      break;
    }
  }
}

void GpsUnicoreClient::handleLine(const std::string& raw_line) {
  const std::string line = TrimLine(raw_line);
  if (line.empty()) {
    return;
  }

  const bool is_zda = line.rfind("$GNZDA", 0) == 0 || line.rfind("$GPZDA", 0) == 0;
  if (is_zda) {
    GpsDateTime parsed{};
    if (!parseZdaLine(line, parsed)) {
      ESP_LOGW(TAG_GPS, "Invalid ZDA line ignored: %s", line.c_str());
      return;
    }
    uint32_t frame_index = 0;
    bool active = false;
    if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      last_datetime_ = parsed;
      last_datetime_received_us_ = esp_timer_get_time();
      has_datetime_ = true;
      if (current_frame_active_) {
        current_frame_.timestamp = parsed;
        current_frame_.gpzda_line = line;
        frame_index = current_frame_.frame_index;
        active = true;
      }
      xSemaphoreGive(data_mutex_);
    }
    if (active) {
      ESP_LOGD(TAG_GPS, "Frame %u time %04u-%02u-%02u %02u:%02u:%02u.%03u UTC",
               static_cast<unsigned>(frame_index), parsed.year, parsed.month, parsed.day,
               parsed.hour, parsed.minute, parsed.second, parsed.millisecond);
    } else {
      ESP_LOGD(TAG_GPS, "UTC %04u-%02u-%02u %02u:%02u:%02u.%03u",
               parsed.year, parsed.month, parsed.day, parsed.hour, parsed.minute, parsed.second, parsed.millisecond);
    }
    return;
  }

  const bool is_gga = line.rfind("$GNGGA", 0) == 0 || line.rfind("$GPGGA", 0) == 0;
  if (is_gga) {
    GpsPosition parsed{};
    if (!parseGgaLine(line, parsed)) {
      ESP_LOGW(TAG_GPS, "Invalid GGA line ignored: %s", line.c_str());
      return;
    }
    if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      last_position_ = parsed;
      last_position_received_us_ = esp_timer_get_time();
      has_position_ = true;
      xSemaphoreGive(data_mutex_);
    }
    ESP_LOGD(TAG_GPS, "Position lat=%.8f lon=%.8f alt=%.2f fix=%d sats=%d",
             parsed.latitude_deg, parsed.longitude_deg, parsed.altitude_m,
             parsed.fix_quality, parsed.satellites);
    return;
  }

  if (line.rfind("$command", 0) == 0) {
    ESP_LOGD(TAG_GPS, "Command response: %s", line.c_str());
  } else if (line.rfind("#VERSION", 0) == 0 || line.rfind("#MODE", 0) == 0) {
    ESP_LOGD(TAG_GPS, "Receiver response: %s", line.c_str());
    if (line.rfind("#MODE", 0) == 0) {
      const std::string mode = ExtractModeFromLine(line);
      if (!mode.empty() && data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        current_mode_ = mode;
        has_current_mode_ = true;
        xSemaphoreGive(data_mutex_);
      }
    }
  } else {
    ESP_LOGD(TAG_GPS, "skip text line: %s", line.c_str());
  }
}

void GpsUnicoreClient::handleRtcmFrame(const RtcmFrame& frame) {
  if (!frame.crc_ok) {
    ESP_LOGW(TAG_GPS, "RTCM frame type=%u failed CRC", frame.message_type);
    return;
  }
  bool enabled = false;
  if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    enabled = isRtcmTypeEnabledLocked(frame.message_type);
    xSemaphoreGive(data_mutex_);
  }
  if (!enabled) {
    ESP_LOGD(TAG_GPS, "skip RTCM type %u size=%u", frame.message_type, static_cast<unsigned>(frame.raw.size()));
    return;
  }

  if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    storeRtcmLocked(frame);
    xSemaphoreGive(data_mutex_);
  }
}

void GpsUnicoreClient::storeRtcmLocked(const RtcmFrame& frame) {
  switch (frame.message_type) {
    case static_cast<uint16_t>(RtcmMessageType::kGpsL1L2Observables):
      if (current_frame_active_) {
        current_frame_.rtcm_by_type[frame.message_type] = frame;
      }
      last_1004_ = frame;
      has_1004_ = true;
      break;
    case static_cast<uint16_t>(RtcmMessageType::kStationAntennaArp):
      if (current_frame_active_) {
        current_frame_.rtcm_by_type[frame.message_type] = frame;
      }
      last_1006_ = frame;
      has_1006_ = true;
      break;
    case static_cast<uint16_t>(RtcmMessageType::kReceiverAntennaDescriptors):
      if (current_frame_active_) {
        current_frame_.rtcm_by_type[frame.message_type] = frame;
      }
      last_1033_ = frame;
      has_1033_ = true;
      break;
    default:
      if (current_frame_active_) {
        current_frame_.rtcm_by_type[frame.message_type] = frame;
      }
      break;
  }
}

bool GpsUnicoreClient::isRtcmTypeEnabledLocked(uint16_t type) const {
  return std::find(enabled_rtcm_types_.begin(), enabled_rtcm_types_.end(), type) != enabled_rtcm_types_.end();
}

bool GpsUnicoreClient::copyRtcmLocked(uint16_t type, RtcmFrame& out) const {
  switch (type) {
    case static_cast<uint16_t>(RtcmMessageType::kGpsL1L2Observables):
      if (!has_1004_) return false;
      out = last_1004_;
      return true;
    case static_cast<uint16_t>(RtcmMessageType::kStationAntennaArp):
      if (!has_1006_) return false;
      out = last_1006_;
      return true;
    case static_cast<uint16_t>(RtcmMessageType::kReceiverAntennaDescriptors):
      if (!has_1033_) return false;
      out = last_1033_;
      return true;
    default:
      return false;
  }
}
