#pragma once

#include <cstdint>

constexpr int MAX_TEMP_SENSORS = 8;
inline constexpr char CONFIG_MOUNT_POINT[] = "/sdcard";

enum class NetMode : uint8_t { kWifiOnly = 0, kEthOnly = 1, kWifiEth = 2 };
enum class NetPriority : uint8_t { kWifi = 0, kEth = 1 };

struct AppConfig;
struct PidConfig;

enum class UsbMode : uint8_t;
