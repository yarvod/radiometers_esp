#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <set>
#include <ctime>
#include <cstdint>
#include <sys/stat.h>
#include <sys/time.h>
#include <cerrno>
#include <dirent.h>
#include <memory>
#include "isrgrootx1.pem.h"

#include "cJSON.h"
#include "app_state.h"
#include "app_services.h"
#include "app_utils.h"
#include "web_ui.h"
#include "http_handlers.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/i2c_master.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "ltc2440.h"
#include "nvs_flash.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mqtt_bridge.h"
#include "error_manager.h"
#include "sd_maintenance.h"
#include "gps_unicore.h"
#include "nvs.h"
#include "esp_partition.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "tinyusb.h"
#include "tusb.h"
#include "class/msc/msc.h"
#include "tusb_msc_storage.h"
#include "esp_rom_sys.h"
#include "onewire_m1820.h"
#include "sdkconfig.h"
#include <dirent.h>
#include <sys/stat.h>

#include "hw_pins.h"
#include "config_loader.h"
#include "gps_module.h"
#include "motion_controller.h"
#include "network_manager.h"
#include "sensor_hub.h"
#include "upload_pipeline.h"

static constexpr char kTag[] = "APP";

#if 0  // legacy addresses kept for reference
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_RAD = 0x77062223A096AD28ULL;
[[maybe_unused]] constexpr uint64_t TEMP_ADDR_LOAD = 0xE80000105FF4E228ULL;
#endif


constexpr char MSC_BASE_PATH[] = "/usb_msc";


static uint64_t GpioMask(gpio_num_t pin) {
  if (pin == GPIO_NUM_NC) {
    return 0;
  }
  return 1ULL << static_cast<uint32_t>(pin);
}



std::string Basename(const std::string& path) {
  const size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

bool MoveFileToDir(const std::string& src_path, const char* dest_dir, std::string* out_new_path) {
  if (src_path.empty() || !dest_dir) return false;
  if (!EnsureDirExists(dest_dir)) return false;
  std::string dest = std::string(dest_dir) + "/" + Basename(src_path);
  if (rename(src_path.c_str(), dest.c_str()) != 0) {
    ESP_LOGE(kTag, "Failed to move %s -> %s (errno %d)", src_path.c_str(), dest.c_str(), errno);
    return false;
  }
  if (out_new_path) {
    *out_new_path = dest;
  }
  return true;
}


// TinyUSB MSC descriptors (single-interface device)
constexpr uint8_t EPNUM_MSC_OUT = 0x01;
constexpr uint8_t EPNUM_MSC_IN = 0x81;
constexpr uint8_t ITF_MSC = 0;

tusb_desc_device_t kMscDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,  // Espressif VID
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const kMscConfigDescriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

const char* kMscStringDescriptor[] = {
    (const char[]) {0x09, 0x04},  // English (0x0409)
    "Espressif",                  // Manufacturer
    "SD Mass Storage",            // Product
    "123456",                     // Serial
    "MSC",                        // MSC interface
};

#if (TUD_OPT_HIGH_SPEED)
const tusb_desc_device_qualifier_t kDeviceQualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};
#endif


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


esp_err_t InitSdCardForMsc(sdmmc_card_t** out_card) {
  bool host_init = false;
  sdmmc_card_t* card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
  ESP_RETURN_ON_FALSE(card, ESP_ERR_NO_MEM, kTag, "No mem for sdmmc_card_t");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags |= SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SD_CLK;
  slot_config.cmd = SD_CMD;
  slot_config.d0 = SD_D0;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_RETURN_ON_ERROR((*host.init)(), kTag, "Host init failed");
  host_init = true;

  esp_err_t err = sdmmc_host_init_slot(host.slot, &slot_config);
  if (err != ESP_OK) {
    if (host_init) {
      if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host.deinit_p(host.slot);
      } else {
        (*host.deinit)();
      }
    }
    free(card);
    return err;
  }

  // Retry until card present
  while (sdmmc_card_init(&host, card) != ESP_OK) {
    ESP_LOGW(kTag, "Insert SD card");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  sdmmc_card_print_info(stdout, card);
  *out_card = card;
  return ESP_OK;
}

esp_err_t StartUsbMsc(sdmmc_card_t* card) {
  tinyusb_msc_sdmmc_config_t config_sdmmc = {
      .card = card,
      .callback_mount_changed = nullptr,
      .callback_premount_changed = nullptr,
      .mount_config =
          {
              .format_if_mount_failed = false,
              .max_files = 4,
              .allocation_unit_size = 0,
              .disk_status_check_enable = false,
              .use_one_fat = false,
          },
  };
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_sdmmc(&config_sdmmc), kTag, "Init MSC storage failed");
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_mount(MSC_BASE_PATH), kTag, "Mount MSC storage failed");

  tinyusb_config_t tusb_cfg = {};
  tusb_cfg.device_descriptor = &kMscDeviceDescriptor;
  tusb_cfg.string_descriptor = kMscStringDescriptor;
  tusb_cfg.string_descriptor_count = sizeof(kMscStringDescriptor) / sizeof(kMscStringDescriptor[0]);
  tusb_cfg.external_phy = false;
#if (TUD_OPT_HIGH_SPEED)
  tusb_cfg.fs_configuration_descriptor = kMscConfigDescriptor;
  tusb_cfg.hs_configuration_descriptor = kMscConfigDescriptor;
  tusb_cfg.qualifier_descriptor = &kDeviceQualifier;
#else
  tusb_cfg.configuration_descriptor = kMscConfigDescriptor;
#endif
  tusb_cfg.self_powered = false;
  tusb_cfg.vbus_monitor_io = -1;
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), kTag, "TinyUSB install failed");
  ESP_LOGI(kTag, "USB in MSC mode");
  return ESP_OK;
}

extern "C" void app_main(void) {
  bool init_ok = true;
  bool msc_ok = true;
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

  bool usb_mode_found = false;
  usb_mode = LoadUsbModeFromNvs(&usb_mode_found);
  if (app_config.usb_mass_storage_from_file && !usb_mode_found) {
    usb_mode = app_config.usb_mass_storage ? UsbMode::kMsc : UsbMode::kCdc;
    ESP_LOGI(kTag, "USB mode set from config.txt (no NVS value): %s", usb_mode == UsbMode::kMsc ? "MSC" : "CDC");
    SaveUsbModeToNvs(usb_mode);
  }
  UpdateState([&](SharedState& s) { s.usb_msc_mode = (usb_mode == UsbMode::kMsc); });

  InitGpios();
  ErrorManagerInit();
  ErrorManagerSetPublisher(&PublishErrorPayload);
  if (app_config.storage_backend == StorageBackend::kInternalFlash) {
    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked() || !MountInternalFlashFs()) {
      ESP_LOGW(kTag, "Internal flash storage is selected but not mounted yet");
    }
  }

  if (usb_mode == UsbMode::kMsc) {
    esp_err_t msc_err = InitSdCardForMsc(&sd_card);
    if (msc_err == ESP_OK) {
      msc_err = StartUsbMsc(sd_card);
    }
    if (msc_err != ESP_OK) {
      msc_ok = false;
      ESP_LOGE(kTag, "USB MSC init failed, fallback to CDC mode: %s", esp_err_to_name(msc_err));
      usb_mode = UsbMode::kCdc;
      ErrorManagerSet(ErrorCode::kUsbMscInit, ErrorSeverity::kError,
                      std::string("MSC init failed: ") + esp_err_to_name(msc_err));
      UpdateState([&](SharedState& s) {
        s.usb_msc_mode = false;
        s.usb_error = "MSC init failed: " + std::string(esp_err_to_name(msc_err));
      });
      SaveUsbModeToNvs(usb_mode);
    } else {
      UpdateState([](SharedState& s) { s.usb_error.clear(); });
      ErrorManagerClear(ErrorCode::kUsbMscInit);
    }
  } else {
    UpdateState([](SharedState& s) { s.usb_error.clear(); });
    ErrorManagerClear(ErrorCode::kUsbMscInit);
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
  const esp_err_t gps_err = StartGpsModule();
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
  StartGpsLogTask(usb_mode);
  xTaskCreatePinnedToCore(&SdStatsTask, "sd_stats", 3072, nullptr, 1, nullptr, 0);

  if (app_config.logging_active && usb_mode == UsbMode::kCdc) {
    const std::string postfix = SanitizePostfix(app_config.logging_postfix);
    log_config.postfix = postfix;
    log_config.use_motor = app_config.logging_use_motor;
    log_config.duration_s = app_config.logging_duration_s;
    if (!StartLoggingToFile(postfix, usb_mode)) {
      ESP_LOGW(kTag, "Auto-start logging failed");
      app_config.logging_active = false;
      (void)SaveConfigToSdCard(app_config, pid_config, usb_mode);
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

  init_ok = init_ok && msc_ok && (ina_err == ESP_OK);
  ESP_LOGI(kTag, "System ready");

  // Start MQTT after init (non-blocking)
  StartMqttBridge();
}
