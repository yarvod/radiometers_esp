#pragma once

#include <cstddef>
#include <string>

#include "app_state.h"

// ---------- GPS receiver control ----------

esp_err_t StartGpsModule();
void StartGpsLogTask(UsbMode mode);

std::string GetGpsCurrentMode();
bool GetGpsCurrentModeText(char* out, size_t out_len);
bool RequestGpsPositionOnce(int timeout_ms, GpsPositionSnapshot* out);
bool RequestGpsUtcTimeOnce(int timeout_ms, UtcTimeSnapshot* out);
GpsReceiverStatus GetGpsReceiverStatus();
void RequestGpsReconfigure();
void ProbeGpsMode();

bool IsGpsLogFilename(const char* name);
bool IsMeteoLogFilename(const char* name);

// ---------- UTC time utilities ----------

UtcTimeSnapshot GetBestUtcTimeForData();
UtcTimeSnapshot GetBestUtcTimeForGps();
const char*     UtcTimeSourceName(UtcTimeSource source);
uint64_t        UtcTimeToUnixMs(const UtcTimeSnapshot& snapshot);
std::string     FormatUtcIso(const UtcTimeSnapshot& snapshot);
