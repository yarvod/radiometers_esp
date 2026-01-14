#pragma once

#include <cstdint>

constexpr int MAX_TEMP_SENSORS = 8;
inline constexpr char CONFIG_MOUNT_POINT[] = "/sdcard";

struct AppConfig;
struct PidConfig;

enum class UsbMode : uint8_t;
