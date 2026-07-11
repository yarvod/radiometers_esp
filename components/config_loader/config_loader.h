#pragma once

#include <cstdio>
#include <string>

#include "app_state.h"

// Parse config.txt text into *config (and side-effect into pid_config / log_config globals).
// Returns true if at least one key was recognised and applied.
bool ParseConfigText(const std::string& text, AppConfig* config);
bool ParseConfigFile(FILE* file, AppConfig* config);

// NVS-backed (internal flash) — no SD needed.
bool LoadConfigTextFromInternalFlash(std::string* out);
bool LoadConfigFromInternalFlash(AppConfig* config);
bool SaveConfigToInternalFlash(const AppConfig& cfg, const PidConfig& pid);
bool SyncConfigToInternalFlash();

struct ConfigSaveResult {
  bool nvs_saved = false;
  bool sd_saved = false;

  bool fully_synced() const { return nvs_saved && sd_saved; }
};

// SD-backed — opens its own early-boot SD mount independent of StorageManager.
void LoadConfigFromSdCard(AppConfig* config);
ConfigSaveResult SaveConfigEverywhere(const AppConfig& cfg, const PidConfig& pid);
bool SaveConfigToSdCard(const AppConfig& cfg, const PidConfig& pid);

// Serialise current config to text (used by save functions and HTTP export).
std::string BuildConfigText(const AppConfig& cfg, const PidConfig& pid);
