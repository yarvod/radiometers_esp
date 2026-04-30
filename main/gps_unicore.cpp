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

struct Rtcm1004Debug {
  bool valid = false;
  uint16_t station_id = 0;
  uint32_t gps_epoch_time_ms = 0;
  uint8_t satellite_count = 0;
  std::vector<uint8_t> prns;
};

struct Rtcm1006Debug {
  bool valid = false;
  uint16_t station_id = 0;
  double ecef_x_m = 0.0;
  double ecef_y_m = 0.0;
  double ecef_z_m = 0.0;
  double antenna_height_m = 0.0;
};

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

void SetNeedMore(bool* need_more, bool value) {
  if (need_more) {
    *need_more = value;
  }
}

void AppendBytes(std::vector<uint8_t>* out, const void* data, size_t len) {
  if (!out || !data || len == 0) return;
  const auto* p = static_cast<const uint8_t*>(data);
  out->insert(out->end(), p, p + len);
}

void AppendRecord(std::vector<uint8_t>* out, uint16_t type, uint16_t flags, const uint8_t* payload, uint32_t payload_size) {
  GnssRecordHeader header{};
  std::memcpy(header.magic, "REC1", sizeof(header.magic));
  header.record_type = type;
  header.flags = flags;
  header.payload_size = payload_size;
  AppendBytes(out, &header, sizeof(header));
  AppendBytes(out, payload, payload_size);
}

bool ReadBits(const uint8_t* data, size_t bit_len, size_t* bit_pos, uint8_t count, uint64_t* out) {
  if (!data || !bit_pos || !out || count > 64 || *bit_pos + count > bit_len) {
    return false;
  }
  uint64_t value = 0;
  for (uint8_t i = 0; i < count; ++i) {
    const size_t pos = *bit_pos + i;
    const uint8_t bit = (data[pos / 8] >> (7 - (pos % 8))) & 0x01;
    value = (value << 1) | bit;
  }
  *bit_pos += count;
  *out = value;
  return true;
}

int64_t SignExtend(uint64_t value, uint8_t bits) {
  if (bits == 0 || bits >= 64) {
    return static_cast<int64_t>(value);
  }
  const uint64_t sign = 1ULL << (bits - 1);
  if ((value & sign) == 0) {
    return static_cast<int64_t>(value);
  }
  const uint64_t mask = (~0ULL) << bits;
  return static_cast<int64_t>(value | mask);
}

bool DecodeRtcm1004(const RtcmFrame& frame, Rtcm1004Debug* out) {
  if (!out || frame.raw.size() < 6 || frame.message_type != 1004) return false;
  const uint16_t payload_len = (static_cast<uint16_t>(frame.raw[1] & 0x03) << 8) | frame.raw[2];
  if (frame.raw.size() < 3 + payload_len + 3) return false;
  const uint8_t* payload = frame.raw.data() + 3;
  const size_t bit_len = static_cast<size_t>(payload_len) * 8;
  size_t bit = 0;
  uint64_t v = 0;

  if (!ReadBits(payload, bit_len, &bit, 12, &v) || v != 1004) return false;
  if (!ReadBits(payload, bit_len, &bit, 12, &v)) return false;
  out->station_id = static_cast<uint16_t>(v);
  if (!ReadBits(payload, bit_len, &bit, 30, &v)) return false;
  out->gps_epoch_time_ms = static_cast<uint32_t>(v);
  if (!ReadBits(payload, bit_len, &bit, 1, &v)) return false;
  if (!ReadBits(payload, bit_len, &bit, 5, &v)) return false;
  out->satellite_count = static_cast<uint8_t>(v);
  if (!ReadBits(payload, bit_len, &bit, 1, &v)) return false;
  if (!ReadBits(payload, bit_len, &bit, 3, &v)) return false;

  out->prns.clear();
  out->prns.reserve(out->satellite_count);
  for (uint8_t i = 0; i < out->satellite_count; ++i) {
    if (!ReadBits(payload, bit_len, &bit, 6, &v)) return false;
    out->prns.push_back(static_cast<uint8_t>(v));
    bit += 119;  // Remaining bits in an RTCM1004 GPS satellite observation block.
    if (bit > bit_len && i + 1 < out->satellite_count) {
      return false;
    }
  }
  out->valid = true;
  return true;
}

bool Rtcm1004HasObservations(const RtcmFrame& frame) {
  Rtcm1004Debug dbg{};
  return DecodeRtcm1004(frame, &dbg) && dbg.satellite_count > 0;
}

bool DecodeRtcm1006(const RtcmFrame& frame, Rtcm1006Debug* out) {
  if (!out || frame.raw.size() < 6 || frame.message_type != 1006) return false;
  const uint16_t payload_len = (static_cast<uint16_t>(frame.raw[1] & 0x03) << 8) | frame.raw[2];
  if (frame.raw.size() < 3 + payload_len + 3) return false;
  const uint8_t* payload = frame.raw.data() + 3;
  const size_t bit_len = static_cast<size_t>(payload_len) * 8;
  size_t bit = 0;
  uint64_t v = 0;

  if (!ReadBits(payload, bit_len, &bit, 12, &v) || v != 1006) return false;
  if (!ReadBits(payload, bit_len, &bit, 12, &v)) return false;
  out->station_id = static_cast<uint16_t>(v);
  if (!ReadBits(payload, bit_len, &bit, 6, &v)) return false;  // ITRF realization year
  bit += 4;  // GPS/GLO/GAL/reference-station indicator bits.
  if (bit > bit_len) return false;
  if (!ReadBits(payload, bit_len, &bit, 38, &v)) return false;
  out->ecef_x_m = static_cast<double>(SignExtend(v, 38)) * 0.0001;
  bit += 2;  // oscillator indicator + reserved bit.
  if (bit > bit_len) return false;
  if (!ReadBits(payload, bit_len, &bit, 38, &v)) return false;
  out->ecef_y_m = static_cast<double>(SignExtend(v, 38)) * 0.0001;
  bit += 2;  // quarter-cycle indicator.
  if (bit > bit_len) return false;
  if (!ReadBits(payload, bit_len, &bit, 38, &v)) return false;
  out->ecef_z_m = static_cast<double>(SignExtend(v, 38)) * 0.0001;
  if (!ReadBits(payload, bit_len, &bit, 16, &v)) return false;
  out->antenna_height_m = static_cast<double>(v) * 0.0001;
  out->valid = true;
  return true;
}

void Log1033AsciiFragments(const RtcmFrame& frame) {
  if (frame.raw.size() < 6) return;
  const size_t payload_len = ((frame.raw[1] & 0x03) << 8) | frame.raw[2];
  if (frame.raw.size() < 3 + payload_len + 3) return;

  std::string current;
  std::vector<std::string> fragments;
  for (size_t i = 3; i < 3 + payload_len; ++i) {
    const uint8_t b = frame.raw[i];
    if (b >= 0x20 && b <= 0x7E) {
      current.push_back(static_cast<char>(b));
    } else {
      if (current.size() >= 4) {
        fragments.push_back(current);
      }
      current.clear();
    }
  }
  if (current.size() >= 4) {
    fragments.push_back(current);
  }
  for (size_t i = 0; i < fragments.size() && i < 6; ++i) {
    ESP_LOGI(TAG_GPS, "RTCM1033 ascii[%u]: %s", static_cast<unsigned>(i), fragments[i].c_str());
  }
}

std::string FormatGpsPrns(const std::vector<uint8_t>& prns) {
  std::string out;
  for (size_t i = 0; i < prns.size(); ++i) {
    if (i > 0) out += ",";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "G%02u", static_cast<unsigned>(prns[i]));
    out += buf;
  }
  return out;
}

int DaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

bool ReadExact(FILE* f, void* data, size_t len) {
  return f && data && fread(data, 1, len, f) == len;
}

bool SkipBytes(FILE* f, uint32_t len) {
  return f && fseek(f, static_cast<long>(len), SEEK_CUR) == 0;
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
  ESP_LOGI(TAG_GPS, "RTCM candidate header: %s payload_len=%u buffered=%u",
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
    ESP_LOGW(TAG_GPS, "Dropping RTCM type=%u with bad CRC", getRtcmMessageType(frame));
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
  ESP_LOGI(TAG_GPS, "RTCM frame parsed type=%u payload_len=%u total=%u",
           parsed.message_type, payload_len, static_cast<unsigned>(parsed.raw.size()));
  out = std::move(parsed);
  buffer.erase(buffer.begin(), buffer.begin() + frame_len);
  return true;
}

uint64_t GpsDateTimeToUnixUtc(const GpsDateTime& dt) {
  if (!dt.valid) {
    return 0;
  }
  const int days = DaysFromCivil(dt.year, dt.month, dt.day);
  if (days < 0) {
    return 0;
  }
  return static_cast<uint64_t>(days) * 86400ULL +
         static_cast<uint64_t>(dt.hour) * 3600ULL +
         static_cast<uint64_t>(dt.minute) * 60ULL +
         static_cast<uint64_t>(dt.second);
}

void FillGnssFileHeader(GnssFileHeader* header) {
  if (!header) return;
  std::memset(header, 0, sizeof(*header));
  std::memcpy(header->magic, "GNSSLOG1", sizeof(header->magic));
  header->version = 1;
  header->header_size = sizeof(GnssFileHeader);
  header->frame_period_s = 30;
  header->flags = 0;
}

bool exportRtcmStream(const char* input_path, const char* output_path) {
  FILE* in = fopen(input_path, "rb");
  if (!in) {
    ESP_LOGE(TAG_GPS, "Cannot open GNSS log %s for RTCM export (errno %d)", input_path, errno);
    return false;
  }
  FILE* out = fopen(output_path, "wb");
  if (!out) {
    ESP_LOGE(TAG_GPS, "Cannot open RTCM export %s (errno %d)", output_path, errno);
    fclose(in);
    return false;
  }

  GnssFileHeader file_header{};
  bool ok = ReadExact(in, &file_header, sizeof(file_header)) &&
            std::memcmp(file_header.magic, "GNSSLOG1", 8) == 0 &&
            file_header.version == 1;
  while (ok) {
    GnssFrameHeader frame_header{};
    if (fread(&frame_header, 1, sizeof(frame_header), in) != sizeof(frame_header)) {
      break;
    }
    if (std::memcmp(frame_header.magic, "FRM1", 4) != 0) {
      ok = false;
      break;
    }
    for (uint16_t i = 0; i < frame_header.record_count; ++i) {
      GnssRecordHeader rec{};
      if (!ReadExact(in, &rec, sizeof(rec)) || std::memcmp(rec.magic, "REC1", 4) != 0) {
        ok = false;
        break;
      }
      if ((rec.flags & REC_FLAG_RTCM_BINARY) != 0) {
        std::vector<uint8_t> payload(rec.payload_size);
        if (!payload.empty() && !ReadExact(in, payload.data(), payload.size())) {
          ok = false;
          break;
        }
        if (!payload.empty() && fwrite(payload.data(), 1, payload.size(), out) != payload.size()) {
          ok = false;
          break;
        }
      } else if (!SkipBytes(in, rec.payload_size)) {
        ok = false;
        break;
      }
    }
  }

  fflush(out);
  fclose(out);
  fclose(in);
  return ok;
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
    ESP_LOGI(TAG_GPS, "TX: %s", cmd.c_str());
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

void GpsUnicoreClient::configurePeriodicOutput(bool auto_base_config) {
  if (auto_base_config) {
    ESP_LOGI(TAG_GPS, "Configuring UM982 BASE mode before RTCM stream");
    sendCommand("UNLOG");
    vTaskDelay(pdMS_TO_TICKS(1000));
    sendCommand("MODE BASE TIME 60");
    ESP_LOGI(TAG_GPS, "Waiting 60 seconds for UM982 BASE TIME initialization");
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
  }

  static constexpr std::array<const char*, 4> kStreamCommands = {
      "GPZDA COM2 30",
      "RTCM1006 COM2 30",
      "RTCM1033 COM2 30",
      "RTCM1004 COM2 30",
  };
  ESP_LOGI(TAG_GPS, "Configuring UM982 periodic GNSS output on COM2 every 30 seconds");
  for (const char* cmd : kStreamCommands) {
    sendCommand(cmd);
    vTaskDelay(kCommandDelay);
  }
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
  const bool ok = current_frame_active_ &&
                  current_frame_.timestamp.valid &&
                  current_frame_.rtcm_by_type.count(1004) > 0 &&
                  current_frame_.rtcm_by_type.count(1006) > 0 &&
                  current_frame_.rtcm_by_type.count(1033) > 0;
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
  ESP_LOGI(TAG_GPS, "Frame %u collected: time=%s gpzda=%s 1006=%s 1033=%s 1004=%s",
           static_cast<unsigned>(out.frame_index),
           out.timestamp.valid ? "yes" : "no",
           out.gpzda_line.empty() ? "no" : "yes",
           out.rtcm_by_type.count(1006) ? "yes" : "no",
           out.rtcm_by_type.count(1033) ? "yes" : "no",
           out.rtcm_by_type.count(1004) ? "yes" : "no");
  return true;
}

bool GpsUnicoreClient::writeFrameToFile(const CurrentFrame& frame, FILE* file) {
  if (!file) return false;

  std::vector<uint8_t> records;
  uint16_t record_count = 0;
  uint32_t flags = 0;
  if (frame.timestamp.valid) {
    flags |= FRAME_FLAG_TIME_VALID;
  }

  if (!frame.gpzda_line.empty()) {
    std::string line = frame.gpzda_line;
    if (line.size() < 2 || line.substr(line.size() - 2) != "\r\n") {
      line += "\r\n";
    }
    AppendRecord(&records, REC_GPZDA_TEXT, REC_FLAG_TEXT,
                 reinterpret_cast<const uint8_t*>(line.data()), static_cast<uint32_t>(line.size()));
    record_count++;
  }

  static constexpr std::array<uint16_t, 3> kRtcmOrder = {1006, 1033, 1004};
  for (uint16_t type : kRtcmOrder) {
    auto it = frame.rtcm_by_type.find(type);
    if (it == frame.rtcm_by_type.end() || !it->second.crc_ok || it->second.raw.empty()) {
      continue;
    }
    uint32_t rec_flags = REC_FLAG_RTCM_BINARY | REC_FLAG_CRC_OK;
    AppendRecord(&records, type, static_cast<uint16_t>(rec_flags),
                 it->second.raw.data(), static_cast<uint32_t>(it->second.raw.size()));
    record_count++;
    if (type == 1004) flags |= FRAME_FLAG_HAS_1004;
    if (type == 1006) flags |= FRAME_FLAG_HAS_1006;
    if (type == 1033) flags |= FRAME_FLAG_HAS_1033;
  }

  GnssFrameHeader header{};
  std::memcpy(header.magic, "FRM1", sizeof(header.magic));
  header.frame_index = frame.frame_index;
  header.unix_time = GpsDateTimeToUnixUtc(frame.timestamp);
  header.year = frame.timestamp.year;
  header.month = frame.timestamp.month;
  header.day = frame.timestamp.day;
  header.hour = frame.timestamp.hour;
  header.minute = frame.timestamp.minute;
  header.second = frame.timestamp.second;
  header.millisecond = frame.timestamp.millisecond;
  header.record_count = record_count;
  header.frame_payload_size = static_cast<uint32_t>(records.size());
  header.flags = flags;

  if (fwrite(&header, 1, sizeof(header), file) != sizeof(header)) {
    return false;
  }
  if (!records.empty() && fwrite(records.data(), 1, records.size(), file) != records.size()) {
    return false;
  }
  return true;
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
  if (!data_mutex_ || xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  const bool ok = has_datetime_;
  if (ok) {
    out = last_datetime_;
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

void GpsUnicoreClient::ReadTaskThunk(void* arg) {
  static_cast<GpsUnicoreClient*>(arg)->uartReadTask();
}

void GpsUnicoreClient::handleBytes(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  const bool chunk_has_d3 = std::find(data, data + len, 0xD3) != data + len;
  if (chunk_has_d3) {
    ESP_LOGI(TAG_GPS, "RX chunk contains RTCM preamble 0xD3, bytes=%u parser_buffer_before=%u",
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
      ESP_LOGI(TAG_GPS, "Frame %u time %04u-%02u-%02u %02u:%02u:%02u.%03u UTC",
               static_cast<unsigned>(frame_index), parsed.year, parsed.month, parsed.day,
               parsed.hour, parsed.minute, parsed.second, parsed.millisecond);
    } else {
      ESP_LOGI(TAG_GPS, "UTC %04u-%02u-%02u %02u:%02u:%02u.%03u",
               parsed.year, parsed.month, parsed.day, parsed.hour, parsed.minute, parsed.second, parsed.millisecond);
    }
    return;
  }

  if (line.rfind("$command", 0) == 0) {
    ESP_LOGI(TAG_GPS, "Command response: %s", line.c_str());
  } else if (line.rfind("#VERSION", 0) == 0 || line.rfind("#MODE", 0) == 0) {
    ESP_LOGI(TAG_GPS, "Receiver response: %s", line.c_str());
  } else {
    ESP_LOGD(TAG_GPS, "Text RX ignored: %s", line.c_str());
  }
}

void GpsUnicoreClient::handleRtcmFrame(const RtcmFrame& frame) {
  if (!frame.crc_ok) {
    ESP_LOGW(TAG_GPS, "RTCM frame type=%u failed CRC", frame.message_type);
    return;
  }
  if (!IsExpectedRtcmType(frame.message_type)) {
    ESP_LOGD(TAG_GPS, "RTCM type=%u len=%u ignored", frame.message_type, static_cast<unsigned>(frame.raw.size()));
    return;
  }

  uint32_t frame_index = 0;
  bool active = false;
  if (data_mutex_ && xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    storeRtcmLocked(frame);
    if (current_frame_active_) {
      frame_index = current_frame_.frame_index;
      active = true;
    }
    xSemaphoreGive(data_mutex_);
  }

  const char* prefix = active ? "Frame" : "Last";
  if (frame.message_type == 1006) {
    Rtcm1006Debug dbg{};
    if (DecodeRtcm1006(frame, &dbg)) {
      ESP_LOGI(TAG_GPS, "%s %u got RTCM1006 size=%u crc=OK, used for APPROX POSITION XYZ, station=%u xyz=%.4f,%.4f,%.4f h=%.4f",
               prefix, static_cast<unsigned>(frame_index), static_cast<unsigned>(frame.raw.size()),
               dbg.station_id, dbg.ecef_x_m, dbg.ecef_y_m, dbg.ecef_z_m, dbg.antenna_height_m);
    } else {
      ESP_LOGI(TAG_GPS, "%s %u got RTCM1006 size=%u crc=OK, used for APPROX POSITION XYZ",
               prefix, static_cast<unsigned>(frame_index), static_cast<unsigned>(frame.raw.size()));
    }
  } else if (frame.message_type == 1033) {
    ESP_LOGI(TAG_GPS, "%s %u got RTCM1033 size=%u crc=OK, receiver/antenna metadata",
             prefix, static_cast<unsigned>(frame_index), static_cast<unsigned>(frame.raw.size()));
    Log1033AsciiFragments(frame);
  } else if (frame.message_type == 1004) {
    Rtcm1004Debug dbg{};
    if (DecodeRtcm1004(frame, &dbg)) {
      const std::string prns = FormatGpsPrns(dbg.prns);
      ESP_LOGI(TAG_GPS, "%s %u got RTCM1004 size=%u crc=OK, GPS obs, station=%u epoch_ms=%u sat_count=%u prns=%s",
               prefix, static_cast<unsigned>(frame_index), static_cast<unsigned>(frame.raw.size()),
               dbg.station_id, static_cast<unsigned>(dbg.gps_epoch_time_ms),
               static_cast<unsigned>(dbg.satellite_count), prns.c_str());
    } else {
      ESP_LOGI(TAG_GPS, "%s %u got RTCM1004 size=%u crc=OK, GPS obs",
               prefix, static_cast<unsigned>(frame_index), static_cast<unsigned>(frame.raw.size()));
    }
  }
}

void GpsUnicoreClient::storeRtcmLocked(const RtcmFrame& frame) {
  switch (frame.message_type) {
    case static_cast<uint16_t>(RtcmMessageType::kGpsL1L2Observables):
      if (!Rtcm1004HasObservations(frame)) {
        ESP_LOGW(TAG_GPS, "RTCM1004 has no decoded observations, not storing in GNSS frame");
        return;
      }
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
      break;
  }
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
