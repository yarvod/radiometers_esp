#include "gps_module.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "app_utils.h"
#include "error_manager.h"
#include "gps_unicore.h"
#include "network_manager.h"
#include "storage_manager.h"
#include "upload_pipeline.h"

static constexpr char kTag[] = "GPS";

// ---------- private globals ----------

static GpsUnicoreClient s_gps_client;
static volatile bool    s_reconfigure_requested = false;
static TaskHandle_t     s_gps_log_task = nullptr;

static uint64_t     s_gnss_log_start_us = 0;
static std::string  s_gnss_log_path;
static constexpr uint64_t kGnssLogRotateUs = 3'600'000'000ULL;

// ---------- UTC time internals ----------

static constexpr time_t kValidUtcThreshold = 1'700'000'000;  // ~2023-11-14

static int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe  = static_cast<unsigned>(year - era * 400);
  const unsigned doy  = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

static bool GpsDateTimeToUnix(const GpsDateTime& dt, time_t* out) {
  if (!out || !dt.valid) return false;
  if (dt.year < 2020 || dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
      dt.hour > 23 || dt.minute > 59 || dt.second > 60) return false;
  const int64_t days    = DaysFromCivil(dt.year, dt.month, dt.day);
  const int64_t seconds = days * 86400LL + static_cast<int64_t>(dt.hour) * 3600LL +
                          static_cast<int64_t>(dt.minute) * 60LL + dt.second;
  if (seconds <= kValidUtcThreshold) return false;
  *out = static_cast<time_t>(seconds);
  return true;
}

static bool SystemUtcNow(time_t* out) {
  if (!out) return false;
  time_t now = time(nullptr);
  if (now <= kValidUtcThreshold) return false;
  *out = now;
  return true;
}

static bool GpsUtcNow(UtcTimeSnapshot* out) {
  if (!out) return false;
  GpsDateTime dt{};
  int64_t received_us = 0;
  if (!s_gps_client.getLastDateTime(dt, &received_us)) return false;
  time_t gps_unix = 0;
  if (!GpsDateTimeToUnix(dt, &gps_unix)) return false;
  int64_t elapsed_us = received_us > 0 ? esp_timer_get_time() - received_us : 0;
  if (elapsed_us < 0) elapsed_us = 0;
  const time_t add_s   = static_cast<time_t>(elapsed_us / 1'000'000LL);
  const uint16_t add_ms = static_cast<uint16_t>((elapsed_us % 1'000'000LL) / 1000LL);
  uint32_t ms = static_cast<uint32_t>(dt.millisecond) + add_ms;
  out->unix_time   = gps_unix + add_s + static_cast<time_t>(ms / 1000U);
  out->millisecond = static_cast<uint16_t>(ms % 1000U);
  out->source      = UtcTimeSource::kGps;
  out->valid       = true;
  return true;
}

static void MaybeDisciplineSystemTimeFromGps(const UtcTimeSnapshot& gps_time) {
  if (!gps_time.valid || gps_time.source != UtcTimeSource::kGps || IsSntpUsable()) return;
  time_t system_now = 0;
  const bool sys_ok = SystemUtcNow(&system_now);
  if (sys_ok && std::llabs(static_cast<long long>(system_now - gps_time.unix_time)) < 2) return;
  timeval tv{};
  tv.tv_sec  = gps_time.unix_time;
  tv.tv_usec = static_cast<suseconds_t>(gps_time.millisecond) * 1000;
  if (settimeofday(&tv, nullptr) == 0) {
    ESP_LOGI(kTag, "System time disciplined from GPS (%lld.%03u)",
             static_cast<long long>(gps_time.unix_time), static_cast<unsigned>(gps_time.millisecond));
  }
}

static UtcTimeSnapshot MakeSystemTimeSnapshot(UtcTimeSource source, bool valid) {
  UtcTimeSnapshot out{};
  time_t now = 0;
  if (SystemUtcNow(&now)) {
    out.unix_time = now;
    out.source    = source;
    out.valid     = valid;
    return out;
  }
  out.unix_time = static_cast<time_t>(esp_timer_get_time() / 1'000'000ULL);
  out.source    = UtcTimeSource::kMonotonic;
  out.valid     = false;
  return out;
}

// ---------- public UTC time utilities ----------

UtcTimeSnapshot GetBestUtcTimeForData() {
  if (IsSntpUsable()) return MakeSystemTimeSnapshot(UtcTimeSource::kSntp, true);
  UtcTimeSnapshot gps{};
  if (GpsUtcNow(&gps)) {
    MaybeDisciplineSystemTimeFromGps(gps);
    return gps;
  }
  time_t sys = 0;
  if (SystemUtcNow(&sys)) return MakeSystemTimeSnapshot(UtcTimeSource::kSystemCached, true);
  return MakeSystemTimeSnapshot(UtcTimeSource::kMonotonic, false);
}

UtcTimeSnapshot GetBestUtcTimeForGps() {
  UtcTimeSnapshot gps{};
  if (GpsUtcNow(&gps)) {
    MaybeDisciplineSystemTimeFromGps(gps);
    return gps;
  }
  if (IsSntpUsable()) return MakeSystemTimeSnapshot(UtcTimeSource::kSntp, true);
  time_t sys = 0;
  if (SystemUtcNow(&sys)) return MakeSystemTimeSnapshot(UtcTimeSource::kSystemCached, true);
  return MakeSystemTimeSnapshot(UtcTimeSource::kMonotonic, false);
}

// ---------- GPS receiver control ----------

esp_err_t StartGpsModule() {
  esp_err_t err = s_gps_client.initUart();
  if (err == ESP_OK) err = s_gps_client.startTasks();
  if (err != ESP_OK) ESP_LOGE(kTag, "GPS init failed: %s", esp_err_to_name(err));
  return err;
}

std::string GetGpsCurrentMode() {
  std::string mode;
  return s_gps_client.getCurrentMode(mode) ? mode : "";
}

bool GetGpsCurrentModeText(char* out, size_t out_len) {
  return s_gps_client.getCurrentMode(out, out_len);
}

static bool CopyGpsPositionSnapshot(const GpsPosition& pos, int64_t rx_us, GpsPositionSnapshot* out) {
  if (!out || !pos.valid || rx_us <= 0) return false;
  out->latitude_deg  = pos.latitude_deg;
  out->longitude_deg = pos.longitude_deg;
  out->altitude_m    = pos.altitude_m;
  out->fix_quality   = pos.fix_quality;
  out->satellites    = pos.satellites;
  out->age_ms        = std::max<int64_t>((esp_timer_get_time() - rx_us) / 1000, 0);
  out->valid         = true;
  return true;
}

bool RequestGpsPositionOnce(int timeout_ms, GpsPositionSnapshot* out) {
  if (out) *out = GpsPositionSnapshot{};
  GpsPosition prev{};
  int64_t prev_us = 0;
  (void)s_gps_client.getLastPosition(prev, &prev_us);
  s_gps_client.sendCommand("GPGGA COM2");
  const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(std::max(timeout_ms, 0)) * 1000;
  while (esp_timer_get_time() < deadline) {
    GpsPosition pos{};
    int64_t rx_us = 0;
    if (s_gps_client.getLastPosition(pos, &rx_us) && rx_us > prev_us)
      return CopyGpsPositionSnapshot(pos, rx_us, out);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  GpsPosition cached{};
  int64_t cached_us = 0;
  if (s_gps_client.getLastPosition(cached, &cached_us))
    return CopyGpsPositionSnapshot(cached, cached_us, out);
  return false;
}

bool RequestGpsUtcTimeOnce(int timeout_ms, UtcTimeSnapshot* out) {
  int64_t prev_us = 0;
  GpsDateTime prev{};
  (void)s_gps_client.getLastDateTime(prev, &prev_us);
  s_gps_client.sendCommand("GPZDA COM2");
  const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (esp_timer_get_time() < deadline) {
    GpsDateTime dt{};
    int64_t rx_us = 0;
    if (s_gps_client.getLastDateTime(dt, &rx_us) && rx_us > prev_us) {
      UtcTimeSnapshot gps_time{};
      if (GpsUtcNow(&gps_time)) {
        MaybeDisciplineSystemTimeFromGps(gps_time);
        if (out) *out = gps_time;
        return true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

GpsReceiverStatus GetGpsReceiverStatus() {
  GpsReceiverStatus status{};
  GpsPosition pos{};
  int64_t pos_rx_us = 0;
  if (s_gps_client.getLastPosition(pos, &pos_rx_us)) {
    status.position_valid  = true;
    status.latitude_deg    = pos.latitude_deg;
    status.longitude_deg   = pos.longitude_deg;
    status.altitude_m      = pos.altitude_m;
    status.fix_quality     = pos.fix_quality;
    status.satellites      = pos.satellites;
    status.position_age_ms = std::max<int64_t>((esp_timer_get_time() - pos_rx_us) / 1000, 0);
  }
  GpsDateTime dt{};
  int64_t time_rx_us = 0;
  if (s_gps_client.getLastDateTime(dt, &time_rx_us)) {
    UtcTimeSnapshot gps_time{};
    if (GpsUtcNow(&gps_time)) {
      status.time_valid    = true;
      // Inline FormatUtcIsoToBuffer
      time_t t = gps_time.unix_time;
      if (t <= 0) t = 0;
      struct tm tm_utc{};
      gmtime_r(&t, &tm_utc);
      strftime(status.time_iso, sizeof(status.time_iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
      status.time_age_ms = std::max<int64_t>((esp_timer_get_time() - time_rx_us) / 1000, 0);
    }
  }
  return status;
}

void RequestGpsReconfigure() {
  s_reconfigure_requested = true;
}

void ProbeGpsMode() {
  s_gps_client.sendCommand("MODE");
}

bool IsGpsLogFilename(const char* name) {
  if (!name) return false;
  return std::strncmp(name, "gps_", 4) == 0;
}

bool IsMeteoLogFilename(const char* name) {
  if (!name) return false;
  return std::strncmp(name, "meteo_", 6) == 0;
}

// ---------- GNSS log helpers (private) ----------

static std::string BuildGnssLogFilename(const GpsDateTime* frame_time) {
  char ts[32] = {};
  if (frame_time && frame_time->valid) {
    const GpsDateTime& dt = *frame_time;
    std::snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  } else {
    const UtcTimeSnapshot best = GetBestUtcTimeForGps();
    if (!best.valid) {
      char fallback[32] = {};
      std::snprintf(fallback, sizeof(fallback), "gps_log_%06u.rtcm3",
                    static_cast<unsigned>(GetBootId()));
      return fallback;
    }
    struct tm tm_info{};
    gmtime_r(&best.unix_time, &tm_info);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);
  }
  std::string name = "gps_rtcm3_";
  name += ts;
  name += "_";
  name += std::to_string(GetBootId());
  name += ".rtcm3";
  return name;
}

static int MoveRootGpsFilesToUploadLocked(const std::string& active_path) {
  if (!EnsureUploadDirs()) return 0;
  const char* mount  = ActiveStorageMountPoint();
  const std::string to_upload = ActiveToUploadDir();
  DIR* dir = opendir(mount);
  if (!dir) return 0;
  const std::string active_name = Basename(active_path);
  int moved = 0;
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
    if (!IsGpsLogFilename(ent->d_name)) continue;
    if (!active_name.empty() && active_name == ent->d_name) continue;
    const std::string src = std::string(mount) + "/" + ent->d_name;
    if (MoveFileToDir(src, to_upload.c_str(), nullptr)) moved++;
  }
  closedir(dir);
  return moved;
}

static bool QueueCurrentGnssLogForUploadLocked() {
  if (s_gnss_log_path.empty()) return true;
  struct stat st{};
  if (stat(s_gnss_log_path.c_str(), &st) != 0) {
    s_gnss_log_path.clear();
    s_gnss_log_start_us = 0;
    return errno == ENOENT;
  }
  if (!S_ISREG(st.st_mode)) {
    s_gnss_log_path.clear();
    s_gnss_log_start_us = 0;
    return true;
  }
  if (st.st_size <= 0) {
    remove(s_gnss_log_path.c_str());
    s_gnss_log_path.clear();
    s_gnss_log_start_us = 0;
    return true;
  }
  if (!EnsureUploadDirs()) return false;
  const std::string queued = Basename(s_gnss_log_path);
  if (!MoveFileToDir(s_gnss_log_path, ActiveToUploadDir().c_str(), nullptr)) return false;
  s_gnss_log_path.clear();
  s_gnss_log_start_us = 0;
  ESP_LOGI(kTag, "Queued RTCM3 for upload: %s", queued.c_str());
  return true;
}

static bool EnsureRtcmLogFileLocked(const GpsDateTime* frame_time) {
  if (s_gnss_log_path.empty() || s_gnss_log_start_us == 0) {
    (void)MoveRootGpsFilesToUploadLocked("");
    const std::string filename = BuildGnssLogFilename(frame_time);
    s_gnss_log_path     = std::string(ActiveStorageMountPoint()) + "/" + filename;
    s_gnss_log_start_us = esp_timer_get_time();
    ESP_LOGI(kTag, "Starting RTCM3 log: %s", s_gnss_log_path.c_str());
  }
  FILE* f = fopen(s_gnss_log_path.c_str(), "ab");
  if (!f) {
    ESP_LOGE(kTag, "Cannot create RTCM3 log %s (errno %d)", s_gnss_log_path.c_str(), errno);
    return false;
  }
  fclose(f);
  return true;
}

static bool IsGnssLogRotationDue(uint64_t now_us) {
  return !s_gnss_log_path.empty() && s_gnss_log_start_us > 0 &&
         now_us - s_gnss_log_start_us >= kGnssLogRotateUs;
}

static bool RotateStaleGnssLogLocked(uint64_t now_us) {
  if (!IsGnssLogRotationDue(now_us)) return true;
  const std::string old = s_gnss_log_path;
  if (!QueueCurrentGnssLogForUploadLocked()) {
    ESP_LOGW(kTag, "RTCM3 rotation due, but failed to queue %s", old.c_str());
    ErrorManagerSet(ErrorCode::kGpsRtcm, ErrorSeverity::kWarning, "RTCM3 log rotation failed");
    return false;
  }
  ESP_LOGI(kTag, "RTCM3 log rotated: %s", old.c_str());
  return true;
}

static bool WriteGnssFrameLocked(const CurrentFrame& frame) {
  const bool first_open = s_gnss_log_start_us == 0;
  const uint64_t now_us = esp_timer_get_time();
  if (first_open || s_gnss_log_path.empty()) {
    if (!EnsureRtcmLogFileLocked(frame.timestamp.valid ? &frame.timestamp : nullptr)) return false;
  } else if (IsGnssLogRotationDue(now_us)) {
    if (!QueueCurrentGnssLogForUploadLocked()) return false;
    const std::string fname = BuildGnssLogFilename(frame.timestamp.valid ? &frame.timestamp : nullptr);
    s_gnss_log_path     = std::string(ActiveStorageMountPoint()) + "/" + fname;
    s_gnss_log_start_us = now_us;
    ESP_LOGI(kTag, "Starting RTCM3 log: %s", s_gnss_log_path.c_str());
  }
  FILE* f = fopen(s_gnss_log_path.c_str(), "ab");
  if (!f) {
    ESP_LOGE(kTag, "Cannot open RTCM3 log %s (errno %d)", s_gnss_log_path.c_str(), errno);
    return false;
  }
  bool ok = s_gps_client.writeRtcmFramesToFile(frame, f);
  fflush(f);
  const int fd = fileno(f);
  if (fd >= 0) fsync(fd);
  if (fclose(f) != 0) ok = false;
  if (ok) {
    if (app_config.storage_backend == StorageBackend::kSd) UpdateSdStatsLocked();
    ESP_LOGD(kTag, "GNSS frame %u → %s", static_cast<unsigned>(frame.frame_index), s_gnss_log_path.c_str());
  } else {
    ESP_LOGE(kTag, "Failed to write GNSS frame %u", static_cast<unsigned>(frame.frame_index));
  }
  return ok;
}

static void WarnMissingGnssFrameData(const CurrentFrame& frame) {
  for (uint16_t type : app_config.gps_rtcm_types) {
    if (frame.rtcm_by_type.count(type) == 0) {
      ESP_LOGD(kTag, "GNSS frame %u missing RTCM%u",
               static_cast<unsigned>(frame.frame_index), static_cast<unsigned>(type));
    }
  }
}

static bool HasAnyGnssFrameData(const CurrentFrame& frame) {
  return !frame.rtcm_by_type.empty();
}

// ---------- GpsLogTask ----------

static void GpsLogTask(void*) {
  constexpr TickType_t kInterval       = pdMS_TO_TICKS(30 * 1000);
  constexpr int64_t    kCollectWindowUs = 35'000'000;
  constexpr uint32_t   kEmptyWarnFrames = 4;
  uint32_t frame_index       = 0;
  uint32_t empty_rtcm_frames = 0;

  vTaskDelay(pdMS_TO_TICKS(5000));
  s_gps_client.probeReceiver();
  vTaskDelay(pdMS_TO_TICKS(1000));
  s_gps_client.configurePeriodicOutput(app_config.gps_rtcm_types, app_config.gps_mode);

  {
    SdLockGuard guard(pdMS_TO_TICKS(1000));
    if (guard.locked() && MountActiveStorage()) {
      (void)MoveRootGpsFilesToUploadLocked("");
      if (app_config.storage_backend == StorageBackend::kSd && !log_file) UnmountLogSd();
    } else {
      ESP_LOGW(kTag, "Storage unavailable at GNSS start; RTCM3 log deferred");
    }
  }

  while (true) {
    const int64_t cycle_start = esp_timer_get_time();
    if (s_reconfigure_requested) {
      s_reconfigure_requested = false;
      s_gps_client.configurePeriodicOutput(app_config.gps_rtcm_types, app_config.gps_mode);
    }
    if (IsGnssLogRotationDue(static_cast<uint64_t>(esp_timer_get_time()))) {
      SdLockGuard guard(pdMS_TO_TICKS(1000));
      if (guard.locked() && MountActiveStorage()) {
        const bool already = IsLogSdMounted();
        (void)RotateStaleGnssLogLocked(static_cast<uint64_t>(esp_timer_get_time()));
        if (app_config.storage_backend == StorageBackend::kSd && !already && !log_file) UnmountLogSd();
      } else {
        ESP_LOGW(kTag, "Storage unavailable, cannot rotate stale RTCM3 log");
      }
    }

    s_gps_client.startFrame(frame_index);
    s_gps_client.pollFrame();
    const int64_t deadline = esp_timer_get_time() + kCollectWindowUs;
    while (esp_timer_get_time() < deadline) {
      if (s_gps_client.isCurrentFrameComplete()) break;
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    CurrentFrame frame{};
    if (!s_gps_client.finishFrame(frame)) {
      s_gps_client.stopFrameOutput();
      ESP_LOGW(kTag, "Failed to finish GNSS frame %u", static_cast<unsigned>(frame_index));
      frame_index++;
      continue;
    }
    s_gps_client.stopFrameOutput();
    WarnMissingGnssFrameData(frame);

    if (!HasAnyGnssFrameData(frame)) {
      empty_rtcm_frames++;
      if (empty_rtcm_frames >= kEmptyWarnFrames) {
        ErrorManagerSet(ErrorCode::kGpsRtcm, ErrorSeverity::kWarning, "GNSS RTCM frames are empty");
      }
      ESP_LOGW(kTag, "GNSS frame %u empty", static_cast<unsigned>(frame.frame_index));
      frame_index++;
      const int64_t elapsed = esp_timer_get_time() - cycle_start;
      vTaskDelay(pdMS_TO_TICKS(elapsed < 30'000'000 ? (30'000'000 - elapsed) / 1000 : 100));
      continue;
    }
    empty_rtcm_frames = 0;
    ErrorManagerClear(ErrorCode::kGpsRtcm);

    {
      SdLockGuard guard(pdMS_TO_TICKS(1000));
      if (!guard.locked()) {
        ESP_LOGW(kTag, "Storage busy, skip GNSS frame");
        frame_index++;
        vTaskDelay(kInterval);
        continue;
      }
      const bool already = IsLogSdMounted();
      if (MountActiveStorage()) {
        (void)WriteGnssFrameLocked(frame);
        if (app_config.storage_backend == StorageBackend::kSd && !already && !log_file) UnmountLogSd();
      }
    }
    frame_index++;
    const int64_t elapsed = esp_timer_get_time() - cycle_start;
    vTaskDelay(pdMS_TO_TICKS(elapsed < 30'000'000 ? (30'000'000 - elapsed) / 1000 : 100));
  }
}

void StartGpsLogTask() {
  if (s_gps_log_task == nullptr) {
    xTaskCreatePinnedToCore(&GpsLogTask, "gps_log", 6144, nullptr, 1, &s_gps_log_task, 0);
  }
}
