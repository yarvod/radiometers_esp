#pragma once

#include <cstdint>
#include <string>

#include "app_services.h"

std::string Trim(const std::string& str);
bool ParseBool(const std::string& value, bool* out);
uint16_t ClampSensorMask(uint16_t mask, int count);
int FirstSetBitIndex(uint16_t mask);
int RssiToQuality(int rssi_dbm);
