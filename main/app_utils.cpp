#include "app_utils.h"

#include <algorithm>
#include <cctype>
#include <ctime>

#include <app_state.h>

std::string Trim(const std::string& str) {
  size_t start = 0;
  while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
    ++start;
  }
  size_t end = str.size();
  while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
    --end;
  }
  return str.substr(start, end - start);
}

bool ParseBool(const std::string& value, bool* out) {
  if (!out) {
    return false;
  }
  std::string lower;
  lower.reserve(value.size());
  for (char c : value) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
    *out = true;
    return true;
  }
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool SanitizeFilename(const std::string& name, std::string* out_full) {
  if (name.empty() || name.size() > 255) return false;
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
      return false;
    }
  }
  std::string full = std::string(CONFIG_MOUNT_POINT) + "/" + name;
  if (out_full) *out_full = full;
  return true;
}

bool SanitizePath(const std::string& rel_path_raw, std::string* out_full) {
  if (rel_path_raw.empty() || rel_path_raw.size() > 256) return false;
  std::string rel_path = rel_path_raw;
  if (!rel_path.empty() && rel_path[0] == '/') rel_path.erase(rel_path.begin());
  if (rel_path.empty() || rel_path.size() > 256) return false;
  if (rel_path.find("..") != std::string::npos) return false;
  if (rel_path.find("//") != std::string::npos) return false;
  for (char c : rel_path) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ' ||
          c == '(' || c == ')')) {
      return false;
    }
  }
  std::string full = std::string(CONFIG_MOUNT_POINT) + "/" + rel_path;
  if (out_full) *out_full = full;
  return true;
}

std::string SanitizePostfix(const std::string& raw) {
  // Keep postfix within overall filename limit (SanitizeFilename caps at 255 chars).
  // BuildLogFilename may further trim based on prefix length.
  constexpr size_t kMaxPostfixLen = 255;
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      out.push_back(c);
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      out.push_back('_');
    }
    if (out.size() >= kMaxPostfixLen) break;
  }
  // Trim trailing underscores from whitespace-only input
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::string IsoUtcNow() {
  time_t now = time(nullptr);
  if (now <= 0) {
    now = static_cast<time_t>(0);
  }
  struct tm tm_utc {};
  gmtime_r(&now, &tm_utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return std::string(buf);
}

uint16_t ClampSensorMask(uint16_t mask, int count) {
  if (count <= 0) return 0;
  const int capped = std::min(count, 16);
  const uint16_t allowed = static_cast<uint16_t>((1u << capped) - 1u);
  return static_cast<uint16_t>(mask & allowed);
}

int FirstSetBitIndex(uint16_t mask) {
  for (int i = 0; i < 16; ++i) {
    if (mask & (1u << i)) return i;
  }
  return 0;
}

int RssiToQuality(int rssi_dbm) {
  // Map -100..-50 dBm to 0..100%
  int q = 2 * (rssi_dbm + 100);
  if (q < 0) q = 0;
  if (q > 100) q = 100;
  return q;
}
