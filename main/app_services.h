#pragma once

#include <cstdint>
#include <ctime>
#include <cstddef>
#include <string>
#include <vector>
#include <cctype>

#include <app_state.h>
#include "esp_err.h"

bool SanitizeFilename(const std::string& name, std::string* out_full);
bool SanitizePath(const std::string& rel_path, std::string* out_full);
std::string SanitizePostfix(const std::string& raw);
std::string IsoUtcNow();
enum class UtcTimeSource : uint8_t {
  kNone = 0,
  kSntp = 1,
  kGps = 2,
  kSystemCached = 3,
  kMonotonic = 4,
};

struct UtcTimeSnapshot {
  time_t unix_time = 0;
  uint16_t millisecond = 0;
  UtcTimeSource source = UtcTimeSource::kNone;
  bool valid = false;
};

struct GpsPositionSnapshot {
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double altitude_m = 0.0;
  int fix_quality = 0;
  int satellites = 0;
  int64_t age_ms = 0;
  bool valid = false;
};

struct GpsReceiverStatus {
  bool position_valid = false;
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double altitude_m = 0.0;
  int fix_quality = 0;
  int satellites = 0;
  int64_t position_age_ms = 0;
  bool time_valid = false;
  char time_iso[32] = {};
  int64_t time_age_ms = 0;
};

struct ClearUploadedFilesResult {
  int scanned = 0;
  int deleted = 0;
  int failed = 0;
  bool sd_busy = false;
  bool mount_failed = false;
};

UtcTimeSnapshot GetBestUtcTimeForData();
UtcTimeSnapshot GetBestUtcTimeForGps();
const char* UtcTimeSourceName(UtcTimeSource source);
uint64_t UtcTimeToUnixMs(const UtcTimeSnapshot& snapshot);
std::string FormatUtcIso(const UtcTimeSnapshot& snapshot);
bool FlushLogFile();
inline std::string SanitizeId(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      out.push_back(c);
    }
  }
  if (out.empty()) out = "device";
  return out;
}

void CalibrateZero();
void CalibrationTask(void*);

void EnableStepper();
void DisableStepper();
void StopStepper();
void StartStepperMove(int steps, bool forward, int speed_us);
bool IsHallTriggered();
void FindZeroTask(void*);
bool StartFindZeroTask(std::string* out_message);

void HeaterSetPowerPercent(float percent);
void FanSetPowerPercent(float percent);
void SetExternalPower(bool enabled);
bool CycleExternalPower(uint32_t off_ms);
int GetGpsAntennaShortRaw();
bool IsGpsAntennaShort();

void InitWifi(const std::string& ssid, const std::string& password, bool ap_mode);
void ApplyNetworkConfig();

bool EnsureTimeSynced(int timeout_ms);

bool MountLogSd();
void UnmountLogSd();
bool EnsureUploadDirs();
bool StartUploadedClearTask(int max_files, std::string* out_status);
bool QueueCurrentLogForUpload();
bool SaveConfigToInternalFlash(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode);
bool SyncConfigToInternalFlash();
bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode);
bool OpenLogFileWithPostfix(const std::string& postfix);

bool StartLoggingToFile(const std::string& postfix_raw, UsbMode current_usb_mode);
void StopLogging();
void UploadTask(void*);

std::string GetGpsCurrentMode();
bool GetGpsCurrentModeText(char* out, size_t out_len);
bool RequestGpsPositionOnce(int timeout_ms, GpsPositionSnapshot* out);
GpsReceiverStatus GetGpsReceiverStatus();
void RequestGpsReconfigure();
void ProbeGpsMode();
