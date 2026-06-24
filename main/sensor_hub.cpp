#include "sensor_hub.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "app_utils.h"
#include "error_manager.h"
#include "hw_pins.h"
#include "ltc2440.h"
#include "onewire_m1820.h"

static constexpr char kTag[] = "SENS";

// ---------- ADC constants ----------

static constexpr float kVref     = 4.096f;
static constexpr float kAdcScale = (kVref / 2.0f) / static_cast<float>(1 << 23);

// ---------- INA219 constants ----------

static constexpr uint8_t    kIna219Addr        = 0x40;
static constexpr uint16_t   kIna219Config      = 0x399F;   // 32V, gain/8, 12-bit, continuous
static constexpr uint16_t   kIna219Calibration = 4096;
static constexpr float      kIna219CurrentLsb  = 0.0002f;  // 200 uA
static constexpr float      kIna219PowerLsb    = kIna219CurrentLsb * 20.0f;
static constexpr float      kIna219BusLsb      = 0.004f;   // 4 mV
static constexpr int        kIna219I2cFreqHz   = 100000;
static constexpr TickType_t kIna219I2cTimeout  = pdMS_TO_TICKS(250);

// ---------- module globals ----------

static bool s_spi_bus_inited = false;

static LTC2440 s_adc1(ADC_CS1, ADC_MISO, false);
static LTC2440 s_adc2(ADC_CS2, ADC_MISO);
static LTC2440 s_adc3(ADC_CS3, ADC_MISO);

static i2c_master_bus_handle_t s_i2c_bus   = nullptr;
static i2c_master_dev_handle_t s_ina219_dev = nullptr;

static volatile uint32_t s_fan1_pulses = 0;
static volatile uint32_t s_fan2_pulses = 0;

// ---------- ISR ----------

static void IRAM_ATTR FanTachIsr(void* arg) {
  uint32_t gpio = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  if (gpio == static_cast<uint32_t>(FAN1_TACH)) {
    __atomic_fetch_add(&s_fan1_pulses, 1, __ATOMIC_RELAXED);
  } else if (gpio == static_cast<uint32_t>(FAN2_TACH)) {
    __atomic_fetch_add(&s_fan2_pulses, 1, __ATOMIC_RELAXED);
  }
}

// ---------- public utilities ----------

void EnsureGpioIsrServiceInstalled() {
  static bool s_installed = false;
  if (s_installed) return;
  esp_err_t err = gpio_install_isr_service(0);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    s_installed = true;
    return;
  }
  ESP_LOGE(kTag, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
  ESP_ERROR_CHECK(err);
}

static inline uint64_t GpioMaskSafe(gpio_num_t pin) {
  if (pin == GPIO_NUM_NC || static_cast<int>(pin) < 0) return 0;
  return 1ULL << static_cast<unsigned>(pin);
}

void SensorHubInitGpios() {
  if (ETH_INT != GPIO_NUM_NC || FAN1_TACH != GPIO_NUM_NC || FAN2_TACH != GPIO_NUM_NC) {
    EnsureGpioIsrServiceInstalled();
  }

  const uint64_t pin_mask = GpioMaskSafe(FAN1_TACH) | GpioMaskSafe(FAN2_TACH);
  if (pin_mask == 0) return;

  gpio_config_t tach_conf = {};
  tach_conf.intr_type     = GPIO_INTR_POSEDGE;
  tach_conf.mode          = GPIO_MODE_INPUT;
  tach_conf.pin_bit_mask  = pin_mask;
  tach_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
  tach_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&tach_conf));

  if (FAN1_TACH != GPIO_NUM_NC) {
    ESP_ERROR_CHECK(gpio_isr_handler_add(FAN1_TACH, FanTachIsr,
                                         reinterpret_cast<void*>(static_cast<uintptr_t>(FAN1_TACH))));
  }
  if (FAN2_TACH != GPIO_NUM_NC) {
    ESP_ERROR_CHECK(gpio_isr_handler_add(FAN2_TACH, FanTachIsr,
                                         reinterpret_cast<void*>(static_cast<uintptr_t>(FAN2_TACH))));
  }
}

esp_err_t InitSpiBus() {
  if (s_spi_bus_inited) return ESP_OK;
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num   = SPI_MOSI;
  buscfg.miso_io_num   = SPI_MISO;
  buscfg.sclk_io_num   = SPI_SCK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 1536;
  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret == ESP_ERR_INVALID_STATE) {
    s_spi_bus_inited = true;
    return ESP_OK;
  }
  if (ret == ESP_OK) {
    s_spi_bus_inited = true;
  }
  return ret;
}

esp_err_t SensorHubInitAdcs() {
  ESP_RETURN_ON_ERROR(InitSpiBus(), kTag, "SPI bus init failed");
  ESP_RETURN_ON_ERROR(s_adc1.Init(SPI2_HOST, ADC_SPI_FREQ_HZ), kTag, "ADC1 init failed");
  ESP_RETURN_ON_ERROR(s_adc2.Init(SPI2_HOST, ADC_SPI_FREQ_HZ), kTag, "ADC2 init failed");
  ESP_RETURN_ON_ERROR(s_adc3.Init(SPI2_HOST, ADC_SPI_FREQ_HZ), kTag, "ADC3 init failed");
  return ESP_OK;
}

esp_err_t SensorHubInitIna() {
  if (!s_i2c_bus) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port              = I2C_NUM_0;
    bus_cfg.sda_io_num            = INA_SDA;
    bus_cfg.scl_io_num            = INA_SCL;
    bus_cfg.clk_source            = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt     = 7;
    bus_cfg.intr_priority         = 0;
    bus_cfg.trans_queue_depth     = 0;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), kTag, "I2C bus init failed");
  }

  if (!s_ina219_dev) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.device_address = kIna219Addr;
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = kIna219I2cFreqHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_ina219_dev), kTag,
                        "INA219 attach failed");
  }

  auto write_reg = [](uint8_t reg, uint16_t value) -> esp_err_t {
    uint8_t payload[3] = {
        reg,
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
    return i2c_master_transmit(s_ina219_dev, payload, sizeof(payload), kIna219I2cTimeout);
  };

  ESP_RETURN_ON_ERROR(write_reg(0x00, kIna219Config), kTag, "INA219 config failed");
  ESP_RETURN_ON_ERROR(write_reg(0x05, kIna219Calibration), kTag, "INA219 calibration failed");
  ESP_LOGI(kTag, "INA219 initialized");
  return ESP_OK;
}

bool WaitForTempSensors(int timeout_ms) {
  if (timeout_ms <= 0) {
    return CopyState().temp_sensor_count > 0;
  }
  const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (esp_timer_get_time() < deadline) {
    if (CopyState().temp_sensor_count > 0) return true;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return CopyState().temp_sensor_count > 0;
}

// ---------- private helpers ----------

static esp_err_t ReadIna219() {
  if (!s_ina219_dev) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning, "INA219 not initialized");
    return ESP_ERR_INVALID_STATE;
  }
  auto read_reg = [](uint8_t reg, uint16_t* value) -> esp_err_t {
    if (!value) return ESP_ERR_INVALID_ARG;
    uint8_t reg_addr = reg;
    uint8_t rx[2]    = {};
    esp_err_t res =
        i2c_master_transmit_receive(s_ina219_dev, &reg_addr, sizeof(reg_addr), rx, sizeof(rx), kIna219I2cTimeout);
    if (res == ESP_OK) {
      *value = static_cast<uint16_t>(static_cast<uint16_t>(rx[0]) << 8 | static_cast<uint16_t>(rx[1]));
    }
    return res;
  };

  uint16_t bus_raw = 0, current_raw = 0, power_raw = 0;

  esp_err_t err = read_reg(0x02, &bus_raw);
  if (err != ESP_OK) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning,
                         std::string("INA219 read bus failed (i2c): ") + esp_err_to_name(err));
    return err;
  }
  err = read_reg(0x04, &current_raw);
  if (err != ESP_OK) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning,
                         std::string("INA219 read current failed (i2c): ") + esp_err_to_name(err));
    return err;
  }
  err = read_reg(0x03, &power_raw);
  if (err != ESP_OK) {
    ErrorManagerSetLocal(ErrorCode::kInaRead, ErrorSeverity::kWarning,
                         std::string("INA219 read power failed (i2c): ") + esp_err_to_name(err));
    return err;
  }

  const float bus_v     = static_cast<float>((bus_raw >> 3) & 0x1FFF) * kIna219BusLsb;
  const float current_a = static_cast<float>(static_cast<int16_t>(current_raw)) * kIna219CurrentLsb;
  const float power_w   = static_cast<float>(static_cast<uint16_t>(power_raw)) * kIna219PowerLsb;

  UpdateState([&](SharedState& s) {
    s.ina_bus_voltage = bus_v;
    s.ina_current     = current_a;
    s.ina_power       = power_w;
  });
  ErrorManagerClearLocal(ErrorCode::kInaRead);
  return ESP_OK;
}

struct TempMeta {
  std::array<std::string, MAX_TEMP_SENSORS> labels{};
  std::array<std::string, MAX_TEMP_SENSORS> addresses{};
};

static std::string FormatOneWireAddress(uint64_t address) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(address));
  return std::string(buf);
}

static TempMeta BuildTempMeta(int count) {
  TempMeta meta{};
  uint64_t addrs[MAX_TEMP_SENSORS]{};
  const int addr_count = M1820GetAddresses(addrs, MAX_TEMP_SENSORS);
  const int capped     = std::min(count, MAX_TEMP_SENSORS);
  for (int i = 0; i < capped; ++i) {
    if (i < addr_count) {
      meta.addresses[i] = FormatOneWireAddress(addrs[i]);
      meta.labels[i]    = "T" + std::to_string(i + 1);
    }
  }
  return meta;
}

// ---------- public ReadAllAdc ----------

esp_err_t ReadAllAdc(float* v1, float* v2, float* v3) {
  int32_t raw1 = 0, raw2 = 0, raw3 = 0;
  esp_err_t err = s_adc1.Read(&raw1);
  if (err != ESP_OK) {
    raw1 = 0;
  }
  err = s_adc2.Read(&raw2);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "ADC2 read failed: %s", esp_err_to_name(err));
    ErrorManagerSet(ErrorCode::kAdcRead, ErrorSeverity::kError, "ADC2 read failed");
    return err;
  }
  err = s_adc3.Read(&raw3);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "ADC3 read failed: %s", esp_err_to_name(err));
    ErrorManagerSet(ErrorCode::kAdcRead, ErrorSeverity::kError, "ADC3 read failed");
    return err;
  }
  *v1 = static_cast<float>(raw1) * kAdcScale;
  *v2 = static_cast<float>(raw2) * kAdcScale;
  *v3 = static_cast<float>(raw3) * kAdcScale;
  ErrorManagerClear(ErrorCode::kAdcRead);
  return ESP_OK;
}

// ---------- tasks ----------

static void AdcTask(void*) {
  while (true) {
    float v1 = 0.0f, v2 = 0.0f, v3 = 0.0f;
    if (ReadAllAdc(&v1, &v2, &v3) == ESP_OK) {
      const uint64_t now_ms = esp_timer_get_time() / 1000ULL;
      UpdateState([&](SharedState& s) {
        s.voltage1     = v1 - s.offset1;
        s.voltage2     = v2 - s.offset2;
        s.voltage3     = v3 - s.offset3;
        s.voltage1_cal = v1;
        s.voltage2_cal = v2;
        s.voltage3_cal = v3;
        s.last_update_ms = now_ms;
      });
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

static void Ina219Task(void*) {
  int     consecutive_failures = 0;
  int64_t last_log_us          = 0;
  int64_t last_reinit_us       = 0;
  while (true) {
    esp_err_t err = ReadIna219();
    if (err != ESP_OK) {
      consecutive_failures++;
      const int64_t now_us = esp_timer_get_time();
      if (consecutive_failures == 1 || now_us - last_log_us > 10'000'000) {
        ESP_LOGW(kTag, "INA219 read failed: %s (consecutive=%d)", esp_err_to_name(err),
                 consecutive_failures);
        last_log_us = now_us;
      }
      if (consecutive_failures >= 3 && now_us - last_reinit_us > 10'000'000) {
        last_reinit_us = now_us;
        ESP_LOGW(kTag, "Reinitializing INA219 after I2C failures");
        esp_err_t init_err = SensorHubInitIna();
        if (init_err != ESP_OK) {
          ESP_LOGW(kTag, "INA219 reinit failed: %s", esp_err_to_name(init_err));
        }
      }
    } else if (consecutive_failures > 0) {
      ESP_LOGI(kTag, "INA219 recovered after %d failed read(s)", consecutive_failures);
      consecutive_failures = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void FanTachTask(void*) {
  const uint32_t pulses_per_rev = 2;
  while (true) {
    uint32_t c1 = s_fan1_pulses;
    uint32_t c2 = s_fan2_pulses;
    s_fan1_pulses = 0;
    s_fan2_pulses = 0;
    const uint32_t rpm1 = (c1 * 60U) / pulses_per_rev;
    const uint32_t rpm2 = (c2 * 60U) / pulses_per_rev;
    UpdateState([&](SharedState& s) {
      s.fan1_rpm = rpm1;
      s.fan2_rpm = rpm2;
    });
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void TempTask(void*) {
  std::array<float, MAX_TEMP_SENSORS> temps{};
  while (true) {
    int count = 0;
    if (M1820ReadTemperatures(temps.data(), MAX_TEMP_SENSORS, &count)) {
      ErrorManagerClear(ErrorCode::kTempSensor);
      const auto meta = BuildTempMeta(count);
      UpdateState([&](SharedState& s) {
        s.temp_sensor_count = count;
        s.temps_c           = temps;
        s.temp_labels       = meta.labels;
        s.temp_addresses    = meta.addresses;
        if (count > 0) {
          const uint16_t available_mask =
              static_cast<uint16_t>((1u << std::min(count, MAX_TEMP_SENSORS)) - 1u);
          uint16_t mask = static_cast<uint16_t>(s.pid_sensor_mask & available_mask);
          if (mask == 0) {
            int idx = s.pid_sensor_index;
            if (idx < 0 || idx >= count) idx = 0;
            mask = static_cast<uint16_t>(1u << idx);
          }
          s.pid_sensor_mask = mask;
          if (s.pid_sensor_index >= count || s.pid_sensor_index < 0) {
            s.pid_sensor_index = FirstSetBitIndex(mask);
          }
        }
      });
      if (count > 0) {
        ESP_LOGD(kTag, "Temps (%d):", count);
        for (int i = 0; i < count; ++i) {
          ESP_LOGD(kTag, "  Sensor %d: %.2f C", i + 1, temps[i]);
        }
      }
    } else {
      ESP_LOGW(kTag, "M1820ReadTemperatures failed");
      ErrorManagerSet(ErrorCode::kTempSensor, ErrorSeverity::kWarning, "M1820 read failed");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ---------- SensorHubStartTasks ----------

void SensorHubStartTasks(bool ina_ok, bool temp_ok) {
  // ADC on core 0, prio 4 — keep separated from stepper (core 1, prio 3)
  xTaskCreatePinnedToCore(&AdcTask, "adc_task", 4096, nullptr, 4, nullptr, 0);
  if (ina_ok) {
    xTaskCreatePinnedToCore(&Ina219Task, "ina219_task", 5120, nullptr, 2, nullptr, 0);
  }
  if (FAN1_TACH != GPIO_NUM_NC || FAN2_TACH != GPIO_NUM_NC) {
    xTaskCreatePinnedToCore(&FanTachTask, "fan_tach_task", 2048, nullptr, 2, nullptr, 0);
  }
  if (temp_ok) {
    xTaskCreatePinnedToCore(&TempTask, "temp_task", 8192, nullptr, 2, nullptr, 0);
  }
}
