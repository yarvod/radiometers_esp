#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app_services.h"
#include "app_state.h"
#include "app_utils.h"
#include "driver/gpio.h"
#include "error_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps_module.h"
#include "http_handlers.h"
#include "hw_pins.h"
#include "motion_controller.h"
#include "mqtt_bridge.h"
#include "network_manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "onewire_m1820.h"
#include "sd_maintenance.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "sensor_hub.h"
#include "upload_pipeline.h"
#include "web_ui.h"
#include "wn90lp.h"

static constexpr char kTag[] = "APP";

static Wn90lpClient s_meteo_client;


static uint64_t GpioMask(gpio_num_t pin) {
  if (pin == GPIO_NUM_NC) {
    return 0;
  }
  return 1ULL << static_cast<uint32_t>(pin);
}







void InitGpios() {
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask =
      GpioMask(RELAY_PIN) | GpioMask(STEPPER_EN) | GpioMask(STEPPER_DIR) | GpioMask(STEPPER_STEP) |
      GpioMask(HEATER_PWM) | GpioMask(FAN_PWM) | GpioMask(EXT_PWR_ON) |
      GpioMask(STATUS_LED_RED) | GpioMask(STATUS_LED_GREEN) |
      GpioMask(ADC_CS1) | GpioMask(ADC_CS2) | GpioMask(ADC_CS3) | GpioMask(ETH_CS) | GpioMask(ETH_RST);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  if (io_conf.pin_bit_mask != 0) {
    ESP_ERROR_CHECK(gpio_config(&io_conf));
  }

  gpio_set_level(RELAY_PIN, 0);
  gpio_set_level(STEPPER_EN, 1);   // disable motor by default
  gpio_set_level(STEPPER_DIR, 0);
  gpio_set_level(STEPPER_STEP, 0);
  gpio_set_level(HEATER_PWM, 0);
  SetExternalPower(true);
  gpio_set_level(STATUS_LED_RED, 0);
  gpio_set_level(STATUS_LED_GREEN, 0);
  gpio_set_level(ADC_CS1, 1);
  gpio_set_level(ADC_CS2, 1);
  gpio_set_level(ADC_CS3, 1);
  gpio_set_level(ETH_CS, 1);
  gpio_set_level(ETH_RST, 0);

  // Hall sensor input
  gpio_config_t hall_conf = {};
  hall_conf.intr_type = GPIO_INTR_DISABLE;
  hall_conf.mode = GPIO_MODE_INPUT;
  hall_conf.pin_bit_mask = (1ULL << MT_HALL_SEN);
  hall_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  hall_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&hall_conf));

  if (GPS_ANT_SHORT != GPIO_NUM_NC) {
    gpio_config_t gps_ant_conf = {};
    gps_ant_conf.intr_type = GPIO_INTR_DISABLE;
    gps_ant_conf.mode = GPIO_MODE_INPUT;
    gps_ant_conf.pin_bit_mask = GpioMask(GPS_ANT_SHORT);
    gps_ant_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gps_ant_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&gps_ant_conf));
  }

  SensorHubInitGpios();
}



extern "C" void app_main(void) {
  bool init_ok = true;
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(ret);
  }
  const uint32_t boot_id = LoadAndIncrementBootId();
  ESP_LOGI(kTag, "Boot ID: %u", static_cast<unsigned>(boot_id));
  LogMemoryStatus("boot");
  ESP_LOGI(kTag, "TLS config: in=%d out=%d dynamic_buffer=%s mbedtls_alloc=%s",
           CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
           CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
           "yes",
#else
           "no",
#endif
           MbedtlsAllocModeName());

  // Optionally disable task watchdog to avoid false positives from tight stepper loop
  esp_task_wdt_deinit();

  state_mutex = xSemaphoreCreateMutex();
  StorageManagerInit();

  LoadConfigFromSdCard(&app_config);

  UpdateState([](SharedState& s) { s.usb_error.clear(); });

  InitGpios();
  ErrorManagerInit();
  ErrorManagerSetPublisher(&PublishErrorPayload);
  if (app_config.storage_backend == StorageBackend::kInternalFlash) {
    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked() || !MountInternalFlashFs()) {
      ESP_LOGW(kTag, "Internal flash storage is selected but not mounted yet");
    }
  }

  MotionControllerInit();
  bool temp_ok = M1820Init(TEMP_1WIRE);
  if (!temp_ok) {
    ESP_LOGW(kTag, "M1820 init failed or no sensors found");
    ErrorManagerSet(ErrorCode::kTempSensor, ErrorSeverity::kError, "M1820 init failed");
    init_ok = false;
  }
  ESP_ERROR_CHECK(SensorHubInitAdcs());
  esp_err_t ina_err = SensorHubInitIna();
  if (ina_err != ESP_OK) {
    ESP_LOGE(kTag, "INA219 init failed: %s", esp_err_to_name(ina_err));
    ErrorManagerSet(ErrorCode::kInaInit, ErrorSeverity::kError, "INA219 init failed");
    init_ok = false;
  } else {
    ErrorManagerClear(ErrorCode::kInaInit);
  }

  if (!app_config.wifi_from_file) {
    ESP_LOGI(kTag, "Using default Wi-Fi config (SSID: %s)", app_config.wifi_ssid.c_str());
  }
  UpdateState([](SharedState& s) {
    s.pid_kp = pid_config.kp;
    s.pid_ki = pid_config.ki;
    s.pid_kd = pid_config.kd;
    s.pid_setpoint = pid_config.setpoint;
    s.pid_sensor_index = pid_config.sensor_index;
    s.pid_sensor_mask = pid_config.sensor_mask;
    s.stepper_speed_us = app_config.stepper_speed_us;
    s.stepper_home_offset_steps = app_config.stepper_home_offset_steps;
    s.motor_hall_active_level = app_config.motor_hall_active_level;
    if (s.stepper_home_status.empty()) {
      s.stepper_home_status = "idle";
    }
  });
  if (app_config.meteo_enabled && METEO_RS485_TX != GPIO_NUM_NC) {
    esp_err_t wn_err = s_meteo_client.initUart();
    if (wn_err == ESP_OK) wn_err = s_meteo_client.startTask();
    if (wn_err != ESP_OK)
      ESP_LOGW(kTag, "WN90LP init failed: %s", esp_err_to_name(wn_err));
  }
  const esp_err_t gps_err = StartGpsModule();
  ErrorManagerSetTimeGetter(GetBestUtcTimeForData);
  ApplyNetworkConfig();
  StartSntp();
  if (EnsureTimeSynced(8000)) {
    ESP_LOGI(kTag, "Time synced via NTP");
    ErrorManagerClear(ErrorCode::kTimeSync);
  } else {
    UtcTimeSnapshot fallback_time{};
    if (gps_err == ESP_OK) {
      (void)RequestGpsUtcTimeOnce(2500, &fallback_time);
    }
    if (!fallback_time.valid) {
      fallback_time = GetBestUtcTimeForData();
    }
    if (fallback_time.source == UtcTimeSource::kGps && fallback_time.valid) {
      ESP_LOGW(kTag, "NTP sync timed out, using GPZDA UTC time");
      ErrorManagerClear(ErrorCode::kTimeSync);
    } else if (fallback_time.source == UtcTimeSource::kSystemCached && fallback_time.valid) {
      ESP_LOGW(kTag, "NTP sync timed out, using cached system UTC time");
      ErrorManagerClear(ErrorCode::kTimeSync);
    } else {
      ESP_LOGW(kTag, "NTP sync timed out, using monotonic timestamp fallback");
      ErrorManagerSet(ErrorCode::kTimeSync, ErrorSeverity::kWarning, "NTP sync timed out");
    }
  }
  StartHttpServer();

  SensorHubStartTasks(ina_err == ESP_OK, temp_ok);
  MotionControllerStartTasks();
  StartNetworkTasks();
  if (upload_task == nullptr) {
    // Upload task uses esp_http_client and std::string, needs a bit more stack to avoid overflow.
    xTaskCreatePinnedToCore(&UploadTask, "upload_task", 12288, nullptr, 1, &upload_task, 0);
  }
  StartGpsLogTask();
  xTaskCreatePinnedToCore(&SdStatsTask, "sd_stats", 3072, nullptr, 1, nullptr, 0);

  if (app_config.logging_active) {
    const std::string postfix = SanitizePostfix(app_config.logging_postfix);
    log_config.postfix = postfix;
    log_config.use_motor = app_config.logging_use_motor;
    log_config.duration_s = app_config.logging_duration_s;
    if (!StartLoggingToFile(postfix)) {
      ESP_LOGW(kTag, "Auto-start logging failed");
      app_config.logging_active = false;
      (void)SaveConfigToSdCard(app_config, pid_config);
    } else {
      app_config.logging_active = true;
      app_config.logging_postfix = postfix;
      app_config.logging_use_motor = log_config.use_motor;
      app_config.logging_duration_s = log_config.duration_s;
      UpdateState([&](SharedState& s) {
        s.logging = true;
        s.log_use_motor = log_config.use_motor;
        s.log_duration_s = log_config.duration_s;
      });
    }
  }

  init_ok = init_ok && (ina_err == ESP_OK);
  ESP_LOGI(kTag, "System ready");

  // Start MQTT after init (non-blocking)
  StartMqttBridge();
}
