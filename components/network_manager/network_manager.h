#pragma once

#include <string>

// (Re)initialize Wi-Fi with the given credentials.
void InitWifi(const std::string& ssid, const std::string& password, bool ap_mode);

// Apply network configuration from app_config (wifi/eth mode, priority).
void ApplyNetworkConfig();

// Start the SNTP polling service.
void StartSntp();

// Block until SNTP-synced or timeout.  Returns true if synced.
bool EnsureTimeSynced(int timeout_ms);

// True iff SNTP time is usable (synced + valid timestamp + network has IP).
bool IsSntpUsable();

// Start background monitoring tasks (net_mon, wifi_mon).  Call once at boot.
void StartNetworkTasks();
