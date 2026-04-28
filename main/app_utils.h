#pragma once

#include <cstdint>
#include <string>

#include "app_services.h"

std::string Trim(const std::string& str);
bool ParseBool(const std::string& value, bool* out);
bool ParseNetMode(const std::string& value, NetMode* out);
bool ParseNetPriority(const std::string& value, NetPriority* out);
std::string NormalizeMqttUri(const std::string& raw);
std::string NetModeToString(NetMode mode);
std::string NetPriorityToString(NetPriority priority);
uint16_t ClampSensorMask(uint16_t mask, int count);
int FirstSetBitIndex(uint16_t mask);
int RssiToQuality(int rssi_dbm);
