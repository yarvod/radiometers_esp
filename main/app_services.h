#pragma once

#include <cstdint>
#include <ctime>
#include <cstddef>
#include <string>
#include <vector>
#include <cctype>

#include "app_state.h"
#include "app_utils.h"
#include "storage_manager.h"
#include "esp_err.h"

#include "gps_module.h"
#include "sensor_hub.h"
#include "data_logger.h"
#include "motion_controller.h"
#include "network_manager.h"
#include "upload_pipeline.h"
// Config load/save/parse — declarations live in config_loader.h
#include "config_loader.h"
