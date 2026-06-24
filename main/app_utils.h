#pragma once

#include <cstdint>
#include <string>

#include "app_state.h"

// ---------- String / parse utilities ----------

std::string Trim(const std::string& str);
bool ParseBool(const std::string& value, bool* out);
bool ParseNetMode(const std::string& value, NetMode* out);
bool ParseNetPriority(const std::string& value, NetPriority* out);
bool ParseStorageBackend(const std::string& value, StorageBackend* out);
std::string NormalizeMqttUri(const std::string& raw);
std::string NetModeToString(NetMode mode);
std::string NetPriorityToString(NetPriority priority);
std::string StorageBackendToString(StorageBackend backend);
uint16_t ClampSensorMask(uint16_t mask, int count);
int FirstSetBitIndex(uint16_t mask);
int RssiToQuality(int rssi_dbm);

// ---------- Filename / path helpers ----------

bool SanitizeFilename(const std::string& name, std::string* out_full);
bool SanitizePath(const std::string& rel_path, std::string* out_full);
std::string SanitizePostfix(const std::string& raw);
std::string Basename(const std::string& path);
bool MoveFileToDir(const std::string& src_path, const char* dest_dir, std::string* out_new_path);

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

// ---------- UTC time utilities ----------

// Format a UtcTimeSnapshot as ISO-8601 string ("2024-01-02T03:04:05Z").
std::string FormatUtcIso(const UtcTimeSnapshot& snapshot);

// Convert a UtcTimeSnapshot to Unix epoch milliseconds.
uint64_t UtcTimeToUnixMs(const UtcTimeSnapshot& snapshot);

// Return a human-readable name for the given UtcTimeSource.
const char* UtcTimeSourceName(UtcTimeSource source);
