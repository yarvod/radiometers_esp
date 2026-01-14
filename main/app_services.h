#pragma once

#include <string>
#include <cctype>

#include <app_state.h>
#include "esp_err.h"

bool SanitizeFilename(const std::string& name, std::string* out_full);
bool SanitizePath(const std::string& rel_path, std::string* out_full);
std::string SanitizePostfix(const std::string& raw);
std::string IsoUtcNow();
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

void HeaterSetPowerPercent(float percent);
void FanSetPowerPercent(float percent);

void InitWifi(const std::string& ssid, const std::string& password, bool ap_mode);

bool EnsureTimeSynced(int timeout_ms);

bool MountLogSd();
void UnmountLogSd();
bool EnsureUploadDirs();
bool QueueCurrentLogForUpload();
bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid, UsbMode current_usb_mode);
bool OpenLogFileWithPostfix(const std::string& postfix);

bool StartLoggingToFile(const std::string& postfix_raw, UsbMode current_usb_mode);
void StopLogging();
void UploadTask(void*);
