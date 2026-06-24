#pragma once

#include <cstdint>
#include <ctime>
#include <cstddef>
#include <string>
#include <vector>
#include <cctype>

#include "app_state.h"
#include "esp_err.h"

bool SanitizeFilename(const std::string& name, std::string* out_full);
bool SanitizePath(const std::string& rel_path, std::string* out_full);
std::string SanitizePostfix(const std::string& raw);
std::string IsoUtcNow();

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

#include "gps_module.h"
#include "sensor_hub.h"
#include "data_logger.h"
#include "motion_controller.h"
#include "network_manager.h"
#include "upload_pipeline.h"
// Config load/save/parse — declarations live in config_loader.h
#include "config_loader.h"
