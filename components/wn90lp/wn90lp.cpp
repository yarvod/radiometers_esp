#include "wn90lp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <sys/stat.h>

#include "app_utils.h"
#include "storage_manager.h"
#include "app_state.h"
#include "driver/uart.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "hw_pins.h"

namespace {

constexpr char kTag[] = "WN90LP";
constexpr uart_port_t kUart = UART_NUM_1;
constexpr int kBaud = 9600;
constexpr size_t kRxBufSize = 256;

constexpr uint8_t kDevAddr = 0x90;   // factory default Modbus address
constexpr uint8_t kReqLen = 8;
constexpr uint8_t kNumRegs = 9;      // 0x0165..0x016D
// Response: addr(1) + func(1) + byte_count(1) + data(18) + crc(2) = 23 bytes.
constexpr size_t kRespLen = 3 + kNumRegs * 2 + 2;

constexpr TickType_t kRespTimeoutTicks = pdMS_TO_TICKS(600);
// Poll interval is read from app_config.meteo_poll_interval_s at runtime.

constexpr uint16_t kInvalidU16  = 0xFFFF;
constexpr uint16_t kInvalidTemp = 0x07FF;

// Build basename for the current-hour meteo log file.
// Format: meteo_YYYYMMDD_HH.txt
static void MeteoFilename(const struct tm& t, char* out, size_t len) {
  snprintf(out, len, "meteo_%04d%02d%02d_%02d.txt",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
}

}  // namespace

// ---------------------------------------------------------------------------
// CRC16 Modbus (reflected poly 0xA001)
// ---------------------------------------------------------------------------
uint16_t Wn90lpClient::crc16Modbus(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 1) ? ((crc >> 1) ^ 0xA001u) : (crc >> 1);
    }
  }
  return crc;
}

// ---------------------------------------------------------------------------
// UART init
// ---------------------------------------------------------------------------
esp_err_t Wn90lpClient::initUart() {
  mutex_ = xSemaphoreCreateMutex();

  uart_config_t cfg = {};
  cfg.baud_rate  = kBaud;
  cfg.data_bits  = UART_DATA_8_BITS;
  cfg.parity     = UART_PARITY_DISABLE;
  cfg.stop_bits  = UART_STOP_BITS_1;
  cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
  cfg.rx_flow_ctrl_thresh = 0;
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.source_clk = UART_SCLK_DEFAULT;
#endif

  esp_err_t err = uart_driver_install(kUart, kRxBufSize, 0, 0, nullptr, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "uart_driver_install: %s", esp_err_to_name(err));
    return err;
  }
  err = uart_param_config(kUart, &cfg);
  if (err != ESP_OK) { ESP_LOGE(kTag, "uart_param_config: %s", esp_err_to_name(err)); return err; }

  err = uart_set_pin(kUart, METEO_RS485_TX, METEO_RS485_RX,
                     METEO_RS485_RTS, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) { ESP_LOGE(kTag, "uart_set_pin: %s", esp_err_to_name(err)); return err; }

  err = uart_set_mode(kUart, UART_MODE_RS485_HALF_DUPLEX);
  if (err != ESP_OK) { ESP_LOGE(kTag, "uart_set_mode RS485: %s", esp_err_to_name(err)); return err; }

  uart_flush_input(kUart);
  ESP_LOGI(kTag, "UART1 RS485 ready: tx=%d rx=%d rts=%d baud=%d",
           METEO_RS485_TX, METEO_RS485_RX, METEO_RS485_RTS, kBaud);
  return ESP_OK;
}

esp_err_t Wn90lpClient::startTask() {
  if (xTaskCreatePinnedToCore(&Wn90lpClient::TaskThunk, "wn90lp",
                               8192, this, 2, nullptr, 0) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void Wn90lpClient::TaskThunk(void* arg) {
  static_cast<Wn90lpClient*>(arg)->taskLoop();
}

void Wn90lpClient::taskLoop() {
  while (true) {
    if (poll()) {
      MeteoData d = getData();
      AppendMeteoLog(d);
      UpdateState([&d](SharedState& s) { s.meteo = d; });
    } else {
      UpdateState([](SharedState& s) { s.meteo.online = false; });
    }
    const int interval_s = std::max(10, app_config.meteo_poll_interval_s);
    vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(interval_s) * 1000u));
  }
}

// ---------------------------------------------------------------------------
// Modbus request/response
// ---------------------------------------------------------------------------
bool Wn90lpClient::sendRequest() {
  uint8_t req[kReqLen];
  req[0] = kDevAddr;
  req[1] = 0x03;
  req[2] = 0x01;
  req[3] = 0x65;                          // start register 0x0165
  req[4] = 0x00;
  req[5] = kNumRegs;                      // 9 registers
  const uint16_t crc = crc16Modbus(req, 6);
  req[6] = static_cast<uint8_t>(crc & 0xFF);
  req[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  uart_flush_input(kUart);
  return uart_write_bytes(kUart, reinterpret_cast<const char*>(req), kReqLen) == kReqLen;
}

bool Wn90lpClient::readResponse(uint8_t* buf, size_t expected) {
  size_t received = 0;
  const TickType_t deadline = xTaskGetTickCount() + kRespTimeoutTicks;
  while (received < expected) {
    const TickType_t now = xTaskGetTickCount();
    if (now >= deadline) return false;
    const int n = uart_read_bytes(kUart, buf + received, expected - received, deadline - now);
    if (n <= 0) return false;
    received += static_cast<size_t>(n);
  }
  return true;
}

bool Wn90lpClient::parseResponse(const uint8_t* buf, MeteoData& out) {
  if (buf[0] != kDevAddr || buf[1] != 0x03 || buf[2] != kNumRegs * 2) {
    ESP_LOGD(kTag, "bad header: %02X %02X %02X", buf[0], buf[1], buf[2]);
    return false;
  }
  const uint16_t calc = crc16Modbus(buf, kRespLen - 2);
  const uint16_t recv = static_cast<uint16_t>(buf[kRespLen - 2]) |
                        (static_cast<uint16_t>(buf[kRespLen - 1]) << 8);
  if (calc != recv) {
    ESP_LOGD(kTag, "CRC mismatch calc=%04X recv=%04X", calc, recv);
    return false;
  }

  auto reg = [&](int i) -> uint16_t {
    return (static_cast<uint16_t>(buf[3 + i * 2]) << 8) |
            static_cast<uint16_t>(buf[3 + i * 2 + 1]);
  };

  const uint16_t r_light = reg(0);
  const uint16_t r_uvi   = reg(1);
  const uint16_t r_temp  = reg(2);
  const uint16_t r_hum   = reg(3);
  const uint16_t r_wind  = reg(4);
  const uint16_t r_gust  = reg(5);
  const uint16_t r_dir   = reg(6);
  const uint16_t r_rain  = reg(7);
  const uint16_t r_pres  = reg(8);

  out.light_lux     = (r_light != kInvalidU16)  ? r_light * 10.0f              : NAN;
  out.uvi           = (r_uvi   != kInvalidU16)  ? r_uvi   * 0.1f               : NAN;
  out.temp_c        = (r_temp  != kInvalidTemp) ? (r_temp  - 400) * 0.1f       : NAN;
  out.humidity_pct  = (r_hum   != kInvalidU16)  ? static_cast<float>(r_hum)    : NAN;
  out.wind_speed_ms = (r_wind  != kInvalidU16)  ? r_wind  * 0.1f               : NAN;
  out.gust_speed_ms = (r_gust  != kInvalidU16)  ? r_gust  * 0.1f               : NAN;
  out.wind_dir_deg  = (r_dir   != kInvalidU16)  ? static_cast<int>(r_dir)      : -1;
  out.rainfall_mm   = (r_rain  != kInvalidU16)  ? r_rain  * 0.1f               : NAN;
  out.pressure_hpa  = (r_pres  != kInvalidU16)  ? r_pres  * 0.1f               : NAN;
  out.online        = true;
  out.timestamp_ms  = static_cast<uint64_t>(esp_timer_get_time() / 1000);
  return true;
}

bool Wn90lpClient::poll() {
  if (!sendRequest()) return false;
  uint8_t buf[kRespLen] = {};
  if (!readResponse(buf, kRespLen)) {
    ESP_LOGD(kTag, "no response (station absent or offline)");
    return false;
  }
  MeteoData d{};
  if (!parseResponse(buf, d)) return false;

  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  last_ = d;
  if (mutex_) xSemaphoreGive(mutex_);

  ESP_LOGI(kTag, "ok t=%.1f°C h=%.0f%% ws=%.1fm/s wd=%d° p=%.1fhPa lux=%.0f",
           d.temp_c, d.humidity_pct, d.wind_speed_ms,
           d.wind_dir_deg, d.pressure_hpa, d.light_lux);
  return true;
}

MeteoData Wn90lpClient::getData() const {
  if (mutex_) xSemaphoreTake(const_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const MeteoData copy = last_;
  if (mutex_) xSemaphoreGive(const_cast<SemaphoreHandle_t>(mutex_));
  return copy;
}

// ---------------------------------------------------------------------------
// CSV logging with hourly rotation and upload queue
// ---------------------------------------------------------------------------

// Track the currently open meteo log across calls (single writer task).
static std::string s_current_meteo_path;

static bool RenameIntoDir(const std::string& src, const std::string& dest_dir) {
  // dest_dir must already exist
  const size_t slash = src.find_last_of('/');
  const std::string basename = (slash == std::string::npos) ? src : src.substr(slash + 1);
  const std::string dest = dest_dir + "/" + basename;
  if (rename(src.c_str(), dest.c_str()) == 0) return true;
  ESP_LOGW("METEO", "rename %s -> %s failed (errno %d)", src.c_str(), dest.c_str(), errno);
  return false;
}

void AppendMeteoLog(const MeteoData& d) {
  SdLockGuard guard;
  if (!guard.locked()) return;

  if (!MountActiveStorage()) return;
  if (!EnsureUploadDirs()) return;

  const time_t now_sec = static_cast<time_t>(d.timestamp_ms / 1000);
  struct tm tm_buf {};
  gmtime_r(&now_sec, &tm_buf);

  char fname[64];
  MeteoFilename(tm_buf, fname, sizeof(fname));
  const std::string path = std::string(ActiveStorageMountPoint()) + "/" + fname;

  // Hourly rotation: if we've moved to a new file, queue the previous one for upload.
  // Also handles storage-backend switch: if the old path is on a different mount point,
  // skip rename (file lives on the old storage) and just start fresh on the new one.
  if (!s_current_meteo_path.empty() && s_current_meteo_path != path) {
    const std::string mount = ActiveStorageMountPoint();
    const bool same_storage = (s_current_meteo_path.rfind(mount, 0) == 0);
    if (same_storage) {
      struct stat st {};
      if (stat(s_current_meteo_path.c_str(), &st) == 0 && st.st_size > 0) {
        const std::string upload_dir = ActiveToUploadDir();
        RenameIntoDir(s_current_meteo_path, upload_dir);
        ESP_LOGI("METEO", "rotated and queued: %s", s_current_meteo_path.c_str());
      }
    }
    s_current_meteo_path.clear();
  }

  FILE* f = fopen(path.c_str(), "a");
  if (!f) {
    ESP_LOGW("METEO", "fopen %s failed (errno %d)", path.c_str(), errno);
    return;
  }

  // Write CSV header once per new file.
  fseek(f, 0, SEEK_END);
  if (ftell(f) == 0) {
    fprintf(f, "timestamp_iso,timestamp_ms,"
               "light_lux,uvi,temp_c,humidity_pct,"
               "wind_speed_ms,gust_speed_ms,wind_dir_deg,"
               "rainfall_mm,pressure_hpa\n");
  }

  char iso[64];
  snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
           tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

  auto fv = [](float v) -> double { return std::isnan(v) ? 0.0 : static_cast<double>(v); };

  fprintf(f, "%s,%llu,%.1f,%.1f,%.1f,%.0f,%.1f,%.1f,%d,%.1f,%.1f\n",
          iso,
          static_cast<unsigned long long>(d.timestamp_ms),
          fv(d.light_lux), fv(d.uvi), fv(d.temp_c), fv(d.humidity_pct),
          fv(d.wind_speed_ms), fv(d.gust_speed_ms),
          d.wind_dir_deg,
          fv(d.rainfall_mm), fv(d.pressure_hpa));

  fclose(f);
  s_current_meteo_path = path;
}
