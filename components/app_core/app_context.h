#pragma once

// DI container passed as void* arg to every FreeRTOS task.
// Grows with each refactoring phase; full form reached after Phase 9.
//
// Usage in task:
//   auto* ctx = static_cast<AppContext*>(arg);
//   ctx->config.device_id ...

#include "app_state.h"

struct AppContext {
  AppConfig&        config;
  PidConfig&        pid_config;
  SharedState&      state;         // access via UpdateState/CopyState helpers
  SemaphoreHandle_t state_mutex;

  // Modules added here as each phase lands:
  // Phase 2:  StorageManager  storage;
  // Phase 3:  NetworkManager  network;
  // Phase 4:  UploadPipeline  upload;
  // Phase 5:  DataLogger      data_logger;
  // Phase 6:  SensorHub*      sensors  = nullptr;
  // Phase 7:  MotionController* motion = nullptr;
  // Phase 8:  GpsModule*      gps      = nullptr;
  //           Wn90lpClient*   meteo    = nullptr;
};
