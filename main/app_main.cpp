#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <string>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/i2c.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ltc2440.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "tinyusb.h"
#include "tusb.h"
#include "class/msc/msc.h"
#include "tusb_msc_storage.h"
#include "esp_rom_sys.h"

namespace {

constexpr char TAG[] = "APP";

// ADC pins
constexpr gpio_num_t ADC_MISO = GPIO_NUM_4;
constexpr gpio_num_t ADC_MOSI = GPIO_NUM_5;
constexpr gpio_num_t ADC_SCK = GPIO_NUM_6;
constexpr gpio_num_t ADC_CS1 = GPIO_NUM_16;
constexpr gpio_num_t ADC_CS2 = GPIO_NUM_15;
constexpr gpio_num_t ADC_CS3 = GPIO_NUM_7;

// INA219 pins (I2C)
constexpr gpio_num_t INA_SDA = GPIO_NUM_42;
constexpr gpio_num_t INA_SCL = GPIO_NUM_41;

// Heater
constexpr gpio_num_t HEATER_PWM = GPIO_NUM_14;

// Fans
constexpr gpio_num_t FAN_PWM = GPIO_NUM_2;
constexpr gpio_num_t FAN1_TACH = GPIO_NUM_1;
constexpr gpio_num_t FAN2_TACH = GPIO_NUM_21;

// SD card pins (1-bit SDMMC)
constexpr gpio_num_t SD_CLK = GPIO_NUM_39;
constexpr gpio_num_t SD_CMD = GPIO_NUM_38;
constexpr gpio_num_t SD_D0 = GPIO_NUM_40;

// Relay and stepper pins
constexpr gpio_num_t RELAY_PIN = GPIO_NUM_17;
constexpr gpio_num_t STEPPER_EN = GPIO_NUM_35;
constexpr gpio_num_t STEPPER_DIR = GPIO_NUM_36;
constexpr gpio_num_t STEPPER_STEP = GPIO_NUM_37;

constexpr float VREF = 4.096f;  // ±Vref/2 range, matches original sketch
constexpr float ADC_SCALE = (VREF / 2.0f) / static_cast<float>(1 << 23);

constexpr char DEFAULT_WIFI_SSID[] = "ASC_WiFi";
constexpr char DEFAULT_WIFI_PASS[] = "ran/fian/asc/2010";
constexpr char HOSTNAME[] = "miap-device";
constexpr bool USE_CUSTOM_MAC = true;
uint8_t CUSTOM_MAC[6] = {0x10, 0x00, 0x3B, 0x6E, 0x83, 0x70};
constexpr char MSC_BASE_PATH[] = "/usb_msc";
constexpr char CONFIG_MOUNT_POINT[] = "/sdcard";
constexpr char CONFIG_FILE_PATH[] = "/sdcard/config.txt";
constexpr size_t WIFI_SSID_MAX_LEN = 32;
constexpr size_t WIFI_PASSWORD_MAX_LEN = 64;
// INA219 constants (32V, 2A, Rshunt=0.1 ohm)
constexpr uint8_t INA219_ADDR = 0x40;
constexpr uint16_t INA219_CONFIG = 0x399F;           // 32V range, gain /8 (320mV), 12-bit, continuous
constexpr uint16_t INA219_CALIBRATION = 4096;        // current_LSB=100uA for 0.1R, power_LSB=2mW
constexpr float INA219_CURRENT_LSB = 0.0001f;        // 100 uA
constexpr float INA219_POWER_LSB = INA219_CURRENT_LSB * 20.0f;
constexpr float INA219_BUS_LSB = 0.004f;             // 4 mV
// Wi-Fi helpers
constexpr int WIFI_CONNECTED_BIT = BIT0;
constexpr int WIFI_FAIL_BIT = BIT1;
EventGroupHandle_t wifi_event_group = nullptr;
int retry_count = 0;

volatile uint32_t fan1_pulse_count = 0;
volatile uint32_t fan2_pulse_count = 0;

static void IRAM_ATTR FanTachIsr(void* arg) {
  uint32_t gpio = (uint32_t)arg;
  if (gpio == static_cast<uint32_t>(FAN1_TACH)) {
    __atomic_fetch_add(&fan1_pulse_count, 1, __ATOMIC_RELAXED);
  } else if (gpio == static_cast<uint32_t>(FAN2_TACH)) {
    __atomic_fetch_add(&fan2_pulse_count, 1, __ATOMIC_RELAXED);
  }
}

struct AppConfig {
  std::string wifi_ssid = DEFAULT_WIFI_SSID;
  std::string wifi_password = DEFAULT_WIFI_PASS;
  bool wifi_from_file = false;
  bool usb_mass_storage = false;
  bool usb_mass_storage_from_file = false;
};

AppConfig app_config{};

// State shared across tasks and HTTP handlers
struct SharedState {
  float voltage1 = 0.0f;
  float voltage2 = 0.0f;
  float voltage3 = 0.0f;
  float offset1 = 0.0f;
  float offset2 = 0.0f;
  float offset3 = 0.0f;
  float ina_bus_voltage = 0.0f;
  float ina_current = 0.0f;
  float ina_power = 0.0f;
  float heater_power = 0.0f;
  float fan_power = 0.0f;       // 0..100%
  uint32_t fan1_rpm = 0;
  uint32_t fan2_rpm = 0;
  bool stepper_enabled = false;
  bool stepper_moving = false;
  bool stepper_direction_forward = true;
  int stepper_speed_us = 500;
  int stepper_target = 0;
  int stepper_position = 0;
  int64_t last_step_timestamp_us = 0;
  uint64_t last_update_ms = 0;
  bool calibrating = false;
  bool usb_msc_mode = false;
  std::string usb_error;
};

SharedState state{};
SemaphoreHandle_t state_mutex = nullptr;

enum class UsbMode : uint8_t { kCdc = 0, kMsc = 1 };
UsbMode usb_mode = UsbMode::kCdc;

// Devices
LTC2440 adc1(ADC_CS1);
LTC2440 adc2(ADC_CS2);
LTC2440 adc3(ADC_CS3);
sdmmc_card_t* sd_card = nullptr;

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

// HTTP server
httpd_handle_t http_server = nullptr;
TaskHandle_t calibration_task = nullptr;

// HTML UI (kept close to original Arduino page)
constexpr char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>LTC2440 ADC Monitor & Stepper Control</title>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
    .container { max-width: 1200px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    .header { text-align: center; margin-bottom: 30px; }
    .adc-readings { display: flex; justify-content: space-around; flex-wrap: wrap; margin-bottom: 30px; }
    .adc-channel { background: #e8f4fd; padding: 20px; border-radius: 8px; margin: 10px; flex: 1; min-width: 200px; text-align: center; }
    .voltage { font-size: 24px; font-weight: bold; color: #2c3e50; margin: 10px 0; }
    .channel-name { font-size: 18px; color: #34495e; }
    .controls { display: flex; flex-wrap: wrap; justify-content: space-between; margin: 20px 0; }
    .control-panel { background: #f8f9fa; padding: 20px; border-radius: 8px; margin: 10px; flex: 1; min-width: 300px; }
    .btn { background: #3498db; color: white; border: none; padding: 12px 24px; margin: 5px; border-radius: 5px; cursor: pointer; font-size: 16px; }
    .btn:hover { background: #2980b9; }
    .btn-calibrate { background: #e74c3c; }
    .btn-calibrate:hover { background: #c0392b; }
    .btn-stepper { background: #27ae60; }
    .btn-stepper:hover { background: #219a52; }
    .btn-stop { background: #e67e22; }
    .btn-stop:hover { background: #d35400; }
    .status { text-align: center; margin: 10px 0; color: #7f8c8d; }
    .stepper-status { background: #fff3cd; padding: 10px; border-radius: 5px; margin: 10px 0; }
    .form-group { margin: 10px 0; }
    label { display: block; margin-bottom: 5px; font-weight: bold; }
    input, select { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }
    .speed-info { font-size: 12px; color: #666; margin-top: 5px; }
    .usb-panel { background: #eef7e8; padding: 20px; border-radius: 8px; margin-top: 10px; }
    .note { font-size: 12px; color: #666; margin-top: 5px; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>LTC2440 ADC Monitor & Stepper Control</h1>
      <p>Real-time voltage measurements and TMC2208 stepper motor control</p>
    </div>
    
    <div class="adc-readings">
      <div class="adc-channel">
        <div class="channel-name">ADC Channel 1</div>
        <div class="voltage" id="voltage1">0.000000 V</div>
      </div>
      <div class="adc-channel">
        <div class="channel-name">ADC Channel 2</div>
        <div class="voltage" id="voltage2">0.000000 V</div>
      </div>
      <div class="adc-channel">
        <div class="channel-name">ADC Channel 3</div>
        <div class="voltage" id="voltage3">0.000000 V</div>
      </div>
    </div>

    <div class="adc-readings">
      <div class="adc-channel">
        <div class="channel-name">INA219 Bus Voltage</div>
        <div class="voltage" id="inaVoltage">0.000 V</div>
        <div>Current: <span id="inaCurrent">0.000</span> A</div>
        <div>Power: <span id="inaPower">0.000</span> W</div>
      </div>
      <div class="adc-channel">
        <div class="channel-name">Fan Status</div>
        <div class="voltage" id="fanPowerDisplay">0 %</div>
        <div>FAN1 RPM: <span id="fan1RpmDisplay">0</span></div>
        <div>FAN2 RPM: <span id="fan2RpmDisplay">0</span></div>
      </div>
    </div>
    
    <div class="controls">
      <div class="control-panel">
        <h3>ADC Controls</h3>
        <button class="btn" onclick="refreshData()">Refresh Data</button>
        <button class="btn btn-calibrate" onclick="calibrate()">Calibrate Zero</button>
      </div>

      <div class="control-panel">
        <h3>Heater Control</h3>
        <div class="form-group">
          <label for="heaterPower">Power (%)</label>
          <input type="number" id="heaterPower" value="0" min="0" max="100" step="0.1">
        </div>
        <button class="btn" onclick="setHeater()">Set Heater</button>
      </div>

      <div class="control-panel">
        <h3>Fan Control</h3>
        <div class="form-group">
          <label for="fanPower">Fan Power (%)</label>
          <input type="number" id="fanPower" value="100" min="0" max="100" step="1">
        </div>
        <button class="btn" onclick="setFan()">Set Fan</button>
      </div>
      
      <div class="control-panel">
        <h3>Stepper Motor Control</h3>
        <div class="stepper-status">
          Status: <span id="stepperStatus">Disabled</span><br>
          Position: <span id="stepperPosition">0</span> steps<br>
          Target: <span id="stepperTarget">0</span> steps<br>
          Moving: <span id="stepperMoving">No</span>
        </div>
        
        <div class="form-group">
          <label for="steps">Number of Steps:</label>
          <input type="number" id="steps" value="400" min="1" max="10000">
        </div>
        
        <div class="form-group">
          <label for="direction">Direction:</label>
          <select id="direction">
            <option value="forward">Forward</option>
            <option value="backward">Backward</option>
          </select>
        </div>
        
        <div class="form-group">
          <label for="speed">Speed (microseconds delay):</label>
          <input type="number" id="speed" value="1000" min="50" max="2000">
          <div class="speed-info">Lower value = faster speed (50-2000 microseconds)</div>
        </div>
        
        <button class="btn btn-stepper" onclick="enableStepper()">Enable Motor</button>
        <button class="btn btn-stop" onclick="disableStepper()">Disable Motor</button>
        <button class="btn" onclick="moveStepper()">Move</button>
        <button class="btn btn-stop" onclick="stopStepper()">Stop</button>
        <button class="btn" onclick="setZero()">Set Position to Zero</button>
      </div>
    </div>
    
    <div class="status">
      Last update: <span id="lastUpdate">-</span>
      <br>USB mode: <span id="usbModeLabel">Serial (logs/flash)</span>
    </div>

    <div class="usb-panel">
      <h3>USB Mode (requires reboot)</h3>
      <label for="usbModeSelect">Select mode:</label>
      <select id="usbModeSelect">
        <option value="cdc">Serial (logs + flashing)</option>
        <option value="msc">Mass Storage (SD card over USB)</option>
      </select>
      <div class="note">MSC отключает доступ к логам/прошивке, даёт доступ к SD по USB. Переключение перезагружает устройство.</div>
      <button class="btn" onclick="applyUsbMode()">Apply & Reboot</button>
      <div class="note" id="usbError" style="color:#c0392b; display:none;"></div>
    </div>
  </div>

  <script>
    function updateData(data) {
      document.getElementById('voltage1').textContent = data.voltage1.toFixed(6) + ' V';
      document.getElementById('voltage2').textContent = data.voltage2.toFixed(6) + ' V';
      document.getElementById('voltage3').textContent = data.voltage3.toFixed(6) + ' V';
      document.getElementById('inaVoltage').textContent = data.inaBusVoltage.toFixed(3) + ' V';
      document.getElementById('inaCurrent').textContent = data.inaCurrent.toFixed(3);
      document.getElementById('inaPower').textContent = data.inaPower.toFixed(3);
      document.getElementById('heaterPower').value = data.heaterPower.toFixed(1);
      document.getElementById('fanPowerDisplay').textContent = data.fanPower.toFixed(0) + ' %';
      document.getElementById('fan1RpmDisplay').textContent = data.fan1Rpm;
      document.getElementById('fan2RpmDisplay').textContent = data.fan2Rpm;
      document.getElementById('stepperStatus').textContent = data.stepperEnabled ? 'Enabled' : 'Disabled';
      document.getElementById('stepperPosition').textContent = data.stepperPosition;
      document.getElementById('stepperTarget').textContent = data.stepperTarget;
      document.getElementById('stepperMoving').textContent = data.stepperMoving ? 'Yes' : 'No';
      document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
      const modeLabel = data.usbMode === 'msc' ? 'Mass Storage (SD over USB)' : 'Serial (logs/flash)';
      document.getElementById('usbModeLabel').textContent = modeLabel;
      const usbModeSelect = document.getElementById('usbModeSelect');
      usbModeSelect.value = data.usbMode === 'msc' ? 'msc' : 'cdc';
      usbModeSelect.disabled = !data.usbMscBuilt && data.usbMode !== 'msc';
      if (!data.usbMscBuilt) {
        usbErrorEl.textContent = 'Прошивка собрана без MSC. Сборка с CONFIG_TINYUSB_MSC_ENABLED=y обязательна.';
        usbErrorEl.style.display = 'block';
      }
      const usbErrorEl = document.getElementById('usbError');
      if (data.usbError) {
        usbErrorEl.textContent = data.usbError;
        usbErrorEl.style.display = 'block';
      } else {
        usbErrorEl.style.display = 'none';
      }
    }
    
    function refreshData() {
      fetch('/data')
        .then(response => response.json())
        .then(data => updateData(data))
        .catch(error => console.error('Error:', error));
    }
    
    function calibrate() {
      if(confirm('Start zero calibration? This will take about 20 seconds.')) {
        fetch('/calibrate', { method: 'POST' })
          .then(response => {
            alert('Calibration started in background...');
            refreshData();
          })
          .catch(error => {
            console.error('Error:', error);
            alert('Calibration started in background...');
            refreshData();
          });
      }
    }
    
    function enableStepper() {
      fetch('/stepper/enable', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Stepper motor enabled');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Stepper motor enabled');
          refreshData();
        });
    }
    
    function disableStepper() {
      fetch('/stepper/disable', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Stepper motor disabled');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Stepper motor disabled');
          refreshData();
        });
    }
    
    function moveStepper() {
      const steps = document.getElementById('steps').value;
      const direction = document.getElementById('direction').value;
      const speed = document.getElementById('speed').value;
      
      fetch('/stepper/move', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          steps: parseInt(steps),
          direction: direction,
          speed: parseInt(speed)
        })
      })
      .then(response => response.json())
      .then(data => {
        alert('Movement started');
        refreshData();
      })
      .catch(error => {
        console.error('Error:', error);
        alert('Movement started');
        refreshData();
      });
    }
    
    function stopStepper() {
      fetch('/stepper/stop', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Movement stopped');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Movement stopped');
          refreshData();
        });
    }
    
    function setZero() {
      fetch('/stepper/zero', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          alert('Position set to zero');
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          alert('Position set to zero');
          refreshData();
        });
    }

    function setHeater() {
      const p = parseFloat(document.getElementById('heaterPower').value);
      fetch('/heater/set', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ power: p })
      })
      .then(() => refreshData())
      .catch(() => refreshData());
    }

    function setFan() {
      const p = parseFloat(document.getElementById('fanPower').value);
      fetch('/fan/set', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ power: p })
      })
      .then(() => refreshData())
      .catch(() => refreshData());
    }
    
    // Auto-refresh every 2 seconds
    setInterval(refreshData, 2000);
    
    // Initial load
    refreshData();

    function applyUsbMode() {
      const mode = document.getElementById('usbModeSelect').value;
      const confirmText = mode === 'msc'
        ? 'Переключиться в Mass Storage. Логи/прошивка по USB станут недоступны. Перезагрузить устройство?'
        : 'Вернуть USB в режим Serial (логи/прошивка). Устройство перезагрузится. Продолжить?';
      if (!confirm(confirmText)) return;
      fetch('/usb/mode', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({mode})
      }).then(res => {
        if (!res.ok) {
          return res.text().then(t => { throw new Error(t || 'Request failed'); });
        }
        return res.text();
      }).then(() => {
        alert('Переключаюсь, устройство перезагрузится');
      }).catch(err => {
        alert('Не удалось переключить: ' + err.message);
      });
    }
  </script>
</body>
</html>
)rawliteral";

std::string Trim(const std::string& str) {
  size_t start = 0;
  while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
    ++start;
  }
  size_t end = str.size();
  while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
    --end;
  }
  return str.substr(start, end - start);
}

bool ParseBool(const std::string& value, bool* out) {
  if (!out) {
    return false;
  }
  std::string lower;
  lower.reserve(value.size());
  for (char c : value) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
    *out = true;
    return true;
  }
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseConfigFile(FILE* file, AppConfig* config) {
  if (!file || !config) {
    return false;
  }

  char line[256];
  bool ssid_set = false;
  bool pass_set = false;
  bool usb_set = false;
  bool usb_value = false;
  std::string ssid;
  std::string password;

  while (fgets(line, sizeof(line), file)) {
    std::string raw(line);
    std::string trimmed = Trim(raw);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      continue;
    }

    std::string key = Trim(trimmed.substr(0, eq));
    std::string value = Trim(trimmed.substr(eq + 1));

    if (key == "wifi_ssid") {
      if (!value.empty() && value.size() < WIFI_SSID_MAX_LEN) {
        ssid = value;
        ssid_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid wifi_ssid in config.txt");
      }
    } else if (key == "wifi_password") {
      if (value.size() >= 8 && value.size() < WIFI_PASSWORD_MAX_LEN) {
        password = value;
        pass_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid wifi_password in config.txt");
      }
    } else if (key == "usb_mass_storage") {
      if (ParseBool(value, &usb_value)) {
        usb_set = true;
      } else {
        ESP_LOGW(TAG, "Invalid usb_mass_storage value in config.txt");
      }
    }
  }

  if (ssid_set && pass_set) {
    config->wifi_ssid = ssid;
    config->wifi_password = password;
    config->wifi_from_file = true;
  }
  if (usb_set) {
    config->usb_mass_storage = usb_value;
    config->usb_mass_storage_from_file = true;
  }
  return config->wifi_from_file || config->usb_mass_storage_from_file;
}

void LoadConfigFromSdCard(AppConfig* config) {
  if (!config) {
    return;
  }

  sdmmc_card_t* card = nullptr;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags |= SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SD_CLK;
  slot_config.cmd = SD_CMD;
  slot_config.d0 = SD_D0;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 4;
  mount_config.allocation_unit_size = 0;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "SD mount failed for config.txt: %s", esp_err_to_name(ret));
    return;
  }

  FILE* file = fopen(CONFIG_FILE_PATH, "r");
  if (!file) {
    ESP_LOGW(TAG, "Config file not found at %s", CONFIG_FILE_PATH);
    esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, card);
    return;
  }

  const bool parsed = ParseConfigFile(file, config);
  fclose(file);
  esp_vfs_fat_sdcard_unmount(CONFIG_MOUNT_POINT, card);

  if (parsed) {
    if (config->wifi_from_file) {
      ESP_LOGI(TAG, "Wi-Fi config loaded from config.txt (SSID: %s)", config->wifi_ssid.c_str());
    }
    if (config->usb_mass_storage_from_file) {
      ESP_LOGI(TAG, "usb_mass_storage=%s (config.txt)", config->usb_mass_storage ? "true" : "false");
    }
  } else {
    ESP_LOGW(TAG, "config.txt present but values are missing/invalid, using defaults");
  }
}

// Helpers to guard shared state
SharedState CopyState() {
  SharedState snapshot;
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
    snapshot = state;
    xSemaphoreGive(state_mutex);
  } else {
    // Fallback: relaxed read without lock to avoid long blocking in time-critical loops
    snapshot = state;
  }
  return snapshot;
}

void UpdateState(const std::function<void(SharedState&)>& updater) {
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    updater(state);
    xSemaphoreGive(state_mutex);
  }
}

void ScheduleRestart() {
  xTaskCreate(
      [](void*) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
      },
      "restart_task", 2048, nullptr, 5, nullptr);
}

UsbMode LoadUsbModeFromNvs() {
  nvs_handle_t handle;
  uint8_t mode = static_cast<uint8_t>(UsbMode::kCdc);
  if (nvs_open("usb", NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u8(handle, "mode", &mode);
    nvs_close(handle);
  }
  return mode == static_cast<uint8_t>(UsbMode::kMsc) ? UsbMode::kMsc : UsbMode::kCdc;
}

void SaveUsbModeToNvs(UsbMode mode) {
  nvs_handle_t handle;
  if (nvs_open("usb", NVS_READWRITE, &handle) == ESP_OK) {
    uint8_t v = static_cast<uint8_t>(mode);
    nvs_set_u8(handle, "mode", v);
    nvs_commit(handle);
    nvs_close(handle);
  }
}

void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (retry_count < 5) {
      esp_wifi_connect();
      retry_count++;
      ESP_LOGW(TAG, "Retry Wi-Fi connection (%d)", retry_count);
    } else {
      xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    retry_count = 0;
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void InitWifi(const std::string& ssid, const std::string& password) {
  wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t* netif = esp_netif_create_default_wifi_sta();
  if (netif && strlen(HOSTNAME) > 0) {
    esp_netif_set_hostname(netif, HOSTNAME);
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, nullptr));

  wifi_config_t wifi_config = {};
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password.c_str(), sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
  wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg = {.capable = true, .required = false};

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  if (USE_CUSTOM_MAC) {
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, CUSTOM_MAC));
  }
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  EventBits_t bits = xEventGroupWaitBits(
      wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15'000));

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to SSID:%s", ssid.c_str());
  } else {
    ESP_LOGE(TAG, "Failed to connect to SSID:%s", ssid.c_str());
  }
}

esp_err_t InitSpiBus() {
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = ADC_MOSI;
  buscfg.miso_io_num = ADC_MISO;
  buscfg.sclk_io_num = ADC_SCK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 4;
  return spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
}

void InitGpios() {
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask =
      (1ULL << RELAY_PIN) | (1ULL << STEPPER_EN) | (1ULL << STEPPER_DIR) | (1ULL << STEPPER_STEP) |
      (1ULL << HEATER_PWM) | (1ULL << FAN_PWM);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  gpio_set_level(RELAY_PIN, 0);
  gpio_set_level(STEPPER_EN, 1);   // disable motor by default
  gpio_set_level(STEPPER_DIR, 0);
  gpio_set_level(STEPPER_STEP, 0);
  gpio_set_level(HEATER_PWM, 0);

  // Tachometer inputs with interrupts
  gpio_config_t tach_conf = {};
  tach_conf.intr_type = GPIO_INTR_POSEDGE;
  tach_conf.mode = GPIO_MODE_INPUT;
  tach_conf.pin_bit_mask = (1ULL << FAN1_TACH) | (1ULL << FAN2_TACH);
  tach_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  tach_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&tach_conf));

  esp_err_t isr_err = gpio_install_isr_service(0);
  if (isr_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(isr_err);
  }
  ESP_ERROR_CHECK(gpio_isr_handler_add(FAN1_TACH, FanTachIsr, (void*)FAN1_TACH));
  ESP_ERROR_CHECK(gpio_isr_handler_add(FAN2_TACH, FanTachIsr, (void*)FAN2_TACH));
}

void InitHeaterPwm() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_10_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = 1000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));

  ledc_channel_config_t c = {};
  c.gpio_num = HEATER_PWM;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_0;
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void HeaterSetPowerPercent(float p) {
  if (p < 0.0f) p = 0.0f;
  if (p > 100.0f) p = 100.0f;
  const uint32_t duty = static_cast<uint32_t>(p * 1023.0f / 100.0f);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  UpdateState([&](SharedState& s) { s.heater_power = p; });
}

[[maybe_unused]] void HeaterOn() { HeaterSetPowerPercent(100.0f); }
[[maybe_unused]] void HeaterOff() { HeaterSetPowerPercent(0.0f); }

void FanSetPowerPercent(float p) {
  if (p < 0.0f) p = 0.0f;
  if (p > 100.0f) p = 100.0f;
  const uint32_t duty = static_cast<uint32_t>(p * 1023.0f / 100.0f);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
  UpdateState([&](SharedState& s) { s.fan_power = p; });
}

void InitFanPwm() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_10_BIT;
  t.timer_num = LEDC_TIMER_1;
  t.freq_hz = 25000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));

  ledc_channel_config_t c = {};
  c.gpio_num = FAN_PWM;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_1;
  c.timer_sel = LEDC_TIMER_1;
  c.duty = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));

  // Default to full power
  FanSetPowerPercent(100.0f);
}

esp_err_t InitIna219() {
  i2c_config_t conf = {};
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = INA_SDA;
  conf.scl_io_num = INA_SCL;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = 400000;
  conf.clk_flags = 0;

  ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &conf), TAG, "I2C param config failed");
  ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0), TAG, "I2C install failed");

  auto write_reg = [](uint8_t reg, uint16_t value) -> esp_err_t {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (INA219_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, static_cast<uint8_t>((value >> 8) & 0xFF), true);
    i2c_master_write_byte(cmd, static_cast<uint8_t>(value & 0xFF), true);
    i2c_master_stop(cmd);
    esp_err_t res = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return res;
  };

  ESP_RETURN_ON_ERROR(write_reg(0x00, INA219_CONFIG), TAG, "INA219 config failed");
  ESP_RETURN_ON_ERROR(write_reg(0x05, INA219_CALIBRATION), TAG, "INA219 calibration failed");
  ESP_LOGI(TAG, "INA219 initialized");
  return ESP_OK;
}

esp_err_t ReadIna219() {
  auto read_reg = [](uint8_t reg, uint16_t* value) -> esp_err_t {
    if (!value) {
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t msb = 0;
    uint8_t lsb = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (INA219_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (INA219_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &msb, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &lsb, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t res = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (res == ESP_OK) {
      *value = static_cast<uint16_t>(static_cast<uint16_t>(msb) << 8 | static_cast<uint16_t>(lsb));
    }
    return res;
  };

  uint16_t bus_raw = 0;
  uint16_t current_raw = 0;
  uint16_t power_raw = 0;

  ESP_RETURN_ON_ERROR(read_reg(0x02, &bus_raw), TAG, "INA219 read bus failed");
  ESP_RETURN_ON_ERROR(read_reg(0x04, &current_raw), TAG, "INA219 read current failed");
  ESP_RETURN_ON_ERROR(read_reg(0x03, &power_raw), TAG, "INA219 read power failed");

  // Bus voltage: bits 3..15, LSB = 4 mV
  const float bus_v = static_cast<float>((bus_raw >> 3) & 0x1FFF) * INA219_BUS_LSB;
  const float current_a = static_cast<int16_t>(current_raw) * INA219_CURRENT_LSB;
  const float power_w = static_cast<uint16_t>(power_raw) * INA219_POWER_LSB;

  UpdateState([&](SharedState& s) {
    s.ina_bus_voltage = bus_v;
    s.ina_current = current_a;
    s.ina_power = power_w;
  });
  return ESP_OK;
}

esp_err_t InitSdCardForMsc(sdmmc_card_t** out_card) {
  bool host_init = false;
  sdmmc_card_t* card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
  ESP_RETURN_ON_FALSE(card, ESP_ERR_NO_MEM, TAG, "No mem for sdmmc_card_t");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags |= SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SD_CLK;
  slot_config.cmd = SD_CMD;
  slot_config.d0 = SD_D0;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_RETURN_ON_ERROR((*host.init)(), TAG, "Host init failed");
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
    ESP_LOGW(TAG, "Insert SD card");
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
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_init_sdmmc(&config_sdmmc), TAG, "Init MSC storage failed");
  ESP_RETURN_ON_ERROR(tinyusb_msc_storage_mount(MSC_BASE_PATH), TAG, "Mount MSC storage failed");

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
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB install failed");
  ESP_LOGI(TAG, "USB in MSC mode");
  return ESP_OK;
}

void EnableStepper() {
  gpio_set_level(STEPPER_EN, 0);
  UpdateState([](SharedState& s) {
    s.stepper_enabled = true;
  });
}

void DisableStepper() {
  gpio_set_level(STEPPER_EN, 1);
  UpdateState([](SharedState& s) {
    s.stepper_enabled = false;
    s.stepper_moving = false;
  });
}

void StopStepper() {
  UpdateState([](SharedState& s) {
    s.stepper_moving = false;
  });
}

void StartStepperMove(int steps, bool forward, int speed_us) {
  // Clamp speed to avoid starving CPU/idle (min 1000 us)
  const int clamped_speed = std::clamp(speed_us, 1000, 2000);
  UpdateState([=](SharedState& s) {
    if (!s.stepper_enabled) {
      return;
    }
    s.stepper_direction_forward = forward;
    s.stepper_speed_us = clamped_speed;
    s.stepper_target = s.stepper_position + (forward ? steps : -steps);
    s.stepper_moving = true;
    s.last_step_timestamp_us = esp_timer_get_time();
  });
  gpio_set_level(STEPPER_DIR, forward ? 1 : 0);
}

void StepperTask(void*) {
  const TickType_t idle_delay_active = pdMS_TO_TICKS(1);
  const TickType_t idle_delay_idle = pdMS_TO_TICKS(5);
  while (true) {
    SharedState snapshot = CopyState();
    if (snapshot.stepper_enabled && snapshot.stepper_moving) {
      // Reassert direction each loop to avoid spurious flips from noise
      gpio_set_level(STEPPER_DIR, snapshot.stepper_direction_forward ? 1 : 0);

      const int64_t now = esp_timer_get_time();
      if (now - snapshot.last_step_timestamp_us >= snapshot.stepper_speed_us) {
        gpio_set_level(STEPPER_STEP, 1);
        esp_rom_delay_us(4);
        gpio_set_level(STEPPER_STEP, 0);

        UpdateState([&](SharedState& s) {
          if (!s.stepper_moving || !s.stepper_enabled) {
            return;
          }
          s.stepper_position += s.stepper_direction_forward ? 1 : -1;
          s.last_step_timestamp_us = now;
          if ((s.stepper_direction_forward && s.stepper_position >= s.stepper_target) ||
              (!s.stepper_direction_forward && s.stepper_position <= s.stepper_target)) {
            s.stepper_moving = false;
          }
        });
        // Yield after step to let lower-priority/idle run
        vTaskDelay(idle_delay_active);
      }
    }
    vTaskDelay(snapshot.stepper_moving ? idle_delay_active : idle_delay_idle);
  }
}

esp_err_t ReadAllAdc(float* v1, float* v2, float* v3) {
  int32_t raw1 = 0, raw2 = 0, raw3 = 0;
  ESP_RETURN_ON_ERROR(adc1.Read(&raw1), TAG, "ADC1 read failed");
  ESP_RETURN_ON_ERROR(adc2.Read(&raw2), TAG, "ADC2 read failed");
  ESP_RETURN_ON_ERROR(adc3.Read(&raw3), TAG, "ADC3 read failed");

  *v1 = static_cast<float>(raw1) * ADC_SCALE;
  *v2 = static_cast<float>(raw2) * ADC_SCALE;
  *v3 = static_cast<float>(raw3) * ADC_SCALE;
  return ESP_OK;
}

void AdcTask(void*) {
  while (true) {
    float v1 = 0.0f, v2 = 0.0f, v3 = 0.0f;
    if (ReadAllAdc(&v1, &v2, &v3) == ESP_OK) {
      const uint64_t now_ms = esp_timer_get_time() / 1000ULL;
      UpdateState([&](SharedState& s) {
        s.voltage1 = v1 - s.offset1;
        s.voltage2 = v2 - s.offset2;
        s.voltage3 = v3 - s.offset3;
        s.last_update_ms = now_ms;
      });
      ESP_LOGD(TAG, "ADC: %.6f %.6f %.6f", v1, v2, v3);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void Ina219Task(void*) {
  while (true) {
    esp_err_t err = ReadIna219();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "INA219 read failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void FanTachTask(void*) {
  const uint32_t pulses_per_rev = 2;
  while (true) {
    uint32_t c1 = fan1_pulse_count;
    uint32_t c2 = fan2_pulse_count;
    fan1_pulse_count = 0;
    fan2_pulse_count = 0;

    const uint32_t rpm1 = (c1 * 60U) / pulses_per_rev;
    const uint32_t rpm2 = (c2 * 60U) / pulses_per_rev;

    UpdateState([&](SharedState& s) {
      s.fan1_rpm = rpm1;
      s.fan2_rpm = rpm2;
    });

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void CalibrateZero() {
  bool already_running = false;
  UpdateState([&](SharedState& s) {
    already_running = s.calibrating;
    if (!s.calibrating) {
      s.calibrating = true;
    }
  });
  if (already_running) {
    ESP_LOGW(TAG, "Calibration already in progress");
    return;
  }

  gpio_set_level(RELAY_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(1000));

  const int samples = 100;
  const int ignore_samples = 10;
  float sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
  int valid = 0;

  for (int i = 0; i < samples; ++i) {
    float v1 = 0.0f, v2 = 0.0f, v3 = 0.0f;
    if (ReadAllAdc(&v1, &v2, &v3) == ESP_OK && i >= ignore_samples) {
      sum1 += v1;
      sum2 += v2;
      sum3 += v3;
      valid++;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  if (valid > 0) {
    const float new_offset1 = sum1 / valid;
    const float new_offset2 = sum2 / valid;
    const float new_offset3 = sum3 / valid;
    UpdateState([&](SharedState& s) {
      s.offset1 = new_offset1;
      s.offset2 = new_offset2;
      s.offset3 = new_offset3;
      s.calibrating = false;
    });
    ESP_LOGI(TAG, "Calibration done: offsets %.6f, %.6f, %.6f", new_offset1, new_offset2, new_offset3);
  } else {
    UpdateState([](SharedState& s) {
      s.calibrating = false;
    });
    ESP_LOGW(TAG, "Calibration collected no samples");
  }

  gpio_set_level(RELAY_PIN, 0);
}

void CalibrationTask(void*) {
  CalibrateZero();
  calibration_task = nullptr;
  vTaskDelete(nullptr);
}

// HTTP handlers
esp_err_t RootHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t DataHandler(httpd_req_t* req) {
  SharedState snapshot = CopyState();
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "voltage1", snapshot.voltage1);
  cJSON_AddNumberToObject(root, "voltage2", snapshot.voltage2);
  cJSON_AddNumberToObject(root, "voltage3", snapshot.voltage3);
  cJSON_AddNumberToObject(root, "inaBusVoltage", snapshot.ina_bus_voltage);
  cJSON_AddNumberToObject(root, "inaCurrent", snapshot.ina_current);
  cJSON_AddNumberToObject(root, "inaPower", snapshot.ina_power);
  cJSON_AddNumberToObject(root, "heaterPower", snapshot.heater_power);
  cJSON_AddNumberToObject(root, "fanPower", snapshot.fan_power);
  cJSON_AddNumberToObject(root, "fan1Rpm", snapshot.fan1_rpm);
  cJSON_AddNumberToObject(root, "fan2Rpm", snapshot.fan2_rpm);
  cJSON_AddNumberToObject(root, "timestamp", snapshot.last_update_ms);
  cJSON_AddBoolToObject(root, "stepperEnabled", snapshot.stepper_enabled);
  cJSON_AddNumberToObject(root, "stepperPosition", snapshot.stepper_position);
  cJSON_AddNumberToObject(root, "stepperTarget", snapshot.stepper_target);
  cJSON_AddBoolToObject(root, "stepperMoving", snapshot.stepper_moving);
  cJSON_AddStringToObject(root, "usbMode", snapshot.usb_msc_mode ? "msc" : "cdc");
  cJSON_AddBoolToObject(root, "usbMscBuilt", CONFIG_TINYUSB_MSC_ENABLED);
  if (!snapshot.usb_error.empty()) {
    cJSON_AddStringToObject(root, "usbError", snapshot.usb_error.c_str());
  }

  const char* resp = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, resp);
  cJSON_free((void*)resp);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t CalibrateHandler(httpd_req_t* req) {
  if (calibration_task == nullptr) {
    xTaskCreatePinnedToCore(&CalibrationTask, "calibrate", 4096, nullptr, 4, &calibration_task, tskNO_AFFINITY);
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"calibration_started\"}");
}

esp_err_t StepperEnableHandler(httpd_req_t* req) {
  EnableStepper();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"stepper_enabled\"}");
}

esp_err_t StepperDisableHandler(httpd_req_t* req) {
  DisableStepper();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"stepper_disabled\"}");
}

esp_err_t StepperMoveHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 256);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  int steps = cJSON_GetObjectItem(root, "steps") ? cJSON_GetObjectItem(root, "steps")->valueint : 400;
  const char* direction_str = cJSON_GetObjectItem(root, "direction") && cJSON_GetObjectItem(root, "direction")->valuestring
                                  ? cJSON_GetObjectItem(root, "direction")->valuestring
                                  : "forward";
  int speed = cJSON_GetObjectItem(root, "speed") ? cJSON_GetObjectItem(root, "speed")->valueint : 500;
  cJSON_Delete(root);

  steps = std::clamp(steps, 1, 10000);
  speed = std::clamp(speed, 50, 2000);

  SharedState snapshot = CopyState();
  if (!snapshot.stepper_enabled) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Stepper not enabled");
    return ESP_FAIL;
  }

  bool forward = std::strcmp(direction_str, "forward") == 0;
  StartStepperMove(steps, forward, speed);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"movement_started\"}");
}

esp_err_t StepperStopHandler(httpd_req_t* req) {
  StopStepper();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"movement_stopped\"}");
}

esp_err_t StepperZeroHandler(httpd_req_t* req) {
  UpdateState([](SharedState& s) {
    s.stepper_position = 0;
    s.stepper_target = 0;
  });
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"position_zeroed\"}");
}

esp_err_t HeaterSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  float power = 0.0f;
  cJSON* power_item = cJSON_GetObjectItem(root, "power");
  if (power_item && cJSON_IsNumber(power_item)) {
    power = static_cast<float>(power_item->valuedouble);
  } else {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing power");
    return ESP_FAIL;
  }
  cJSON_Delete(root);

  HeaterSetPowerPercent(power);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t FanSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  cJSON* val = cJSON_GetObjectItem(root, "power");
  if (!val || !cJSON_IsNumber(val)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing power");
    return ESP_FAIL;
  }
  float p = static_cast<float>(val->valuedouble);
  cJSON_Delete(root);

  FanSetPowerPercent(p);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t UsbModeGetHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  if (usb_mode == UsbMode::kMsc) {
    return httpd_resp_sendstr(req, "{\"mode\":\"msc\"}");
  }
  return httpd_resp_sendstr(req, "{\"mode\":\"cdc\"}");
}

esp_err_t UsbModeSetHandler(httpd_req_t* req) {
  const size_t buf_len = std::min<size_t>(req->content_len, 64);
  if (buf_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
    return ESP_FAIL;
  }
  std::string body(buf_len, '\0');
  int received = httpd_req_recv(req, body.data(), buf_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
    return ESP_FAIL;
  }
  body.resize(received);

  cJSON* root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  const char* mode_str = cJSON_GetObjectItem(root, "mode") && cJSON_GetObjectItem(root, "mode")->valuestring
                             ? cJSON_GetObjectItem(root, "mode")->valuestring
                             : "";
  UsbMode requested = (std::strcmp(mode_str, "msc") == 0) ? UsbMode::kMsc : UsbMode::kCdc;
  cJSON_Delete(root);

#if !CONFIG_TINYUSB_MSC_ENABLED
  if (requested == UsbMode::kMsc) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MSC not enabled in firmware");
    return ESP_FAIL;
  }
#endif

  if (requested == usb_mode) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"unchanged\"}");
  }

  // If leaving MSC, unmount before reboot to keep host happy
  if (usb_mode == UsbMode::kMsc && requested == UsbMode::kCdc) {
    tinyusb_msc_storage_unmount();
  }

  SaveUsbModeToNvs(requested);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"status\":\"restarting\",\"mode\":\"pending\"}");
  ScheduleRestart();
  return ESP_OK;
}

httpd_handle_t StartHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 13;

  if (httpd_start(&http_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return nullptr;
  }

  httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = RootHandler, .user_ctx = nullptr};
  httpd_uri_t data_uri = {.uri = "/data", .method = HTTP_GET, .handler = DataHandler, .user_ctx = nullptr};
  httpd_uri_t calibrate_uri = {.uri = "/calibrate", .method = HTTP_POST, .handler = CalibrateHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_enable_uri = {.uri = "/stepper/enable", .method = HTTP_POST, .handler = StepperEnableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_disable_uri = {.uri = "/stepper/disable", .method = HTTP_POST, .handler = StepperDisableHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_move_uri = {.uri = "/stepper/move", .method = HTTP_POST, .handler = StepperMoveHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_stop_uri = {.uri = "/stepper/stop", .method = HTTP_POST, .handler = StepperStopHandler, .user_ctx = nullptr};
  httpd_uri_t stepper_zero_uri = {.uri = "/stepper/zero", .method = HTTP_POST, .handler = StepperZeroHandler, .user_ctx = nullptr};
  httpd_uri_t usb_mode_get_uri = {.uri = "/usb/mode", .method = HTTP_GET, .handler = UsbModeGetHandler, .user_ctx = nullptr};
  httpd_uri_t usb_mode_set_uri = {.uri = "/usb/mode", .method = HTTP_POST, .handler = UsbModeSetHandler, .user_ctx = nullptr};
  httpd_uri_t heater_set_uri = {.uri = "/heater/set", .method = HTTP_POST, .handler = HeaterSetHandler, .user_ctx = nullptr};
  httpd_uri_t fan_set_uri = {.uri = "/fan/set", .method = HTTP_POST, .handler = FanSetHandler, .user_ctx = nullptr};

  httpd_register_uri_handler(http_server, &root_uri);
  httpd_register_uri_handler(http_server, &data_uri);
  httpd_register_uri_handler(http_server, &calibrate_uri);
  httpd_register_uri_handler(http_server, &stepper_enable_uri);
  httpd_register_uri_handler(http_server, &stepper_disable_uri);
  httpd_register_uri_handler(http_server, &stepper_move_uri);
  httpd_register_uri_handler(http_server, &stepper_stop_uri);
  httpd_register_uri_handler(http_server, &stepper_zero_uri);
  httpd_register_uri_handler(http_server, &usb_mode_get_uri);
  httpd_register_uri_handler(http_server, &usb_mode_set_uri);
  httpd_register_uri_handler(http_server, &heater_set_uri);
  httpd_register_uri_handler(http_server, &fan_set_uri);
  ESP_LOGI(TAG, "HTTP server started");
  return http_server;
}

}  // namespace

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(ret);
  }

  // Optionally disable task watchdog to avoid false positives from tight stepper loop
  esp_task_wdt_deinit();

  state_mutex = xSemaphoreCreateMutex();

  LoadConfigFromSdCard(&app_config);

  usb_mode = LoadUsbModeFromNvs();
  if (app_config.usb_mass_storage_from_file) {
    usb_mode = app_config.usb_mass_storage ? UsbMode::kMsc : UsbMode::kCdc;
    ESP_LOGI(TAG, "USB mode overridden by config.txt: %s", usb_mode == UsbMode::kMsc ? "MSC" : "CDC");
  }
  UpdateState([&](SharedState& s) { s.usb_msc_mode = (usb_mode == UsbMode::kMsc); });

  if (usb_mode == UsbMode::kMsc) {
    esp_err_t msc_err = InitSdCardForMsc(&sd_card);
    if (msc_err == ESP_OK) {
      msc_err = StartUsbMsc(sd_card);
    }
    if (msc_err != ESP_OK) {
      ESP_LOGE(TAG, "USB MSC init failed, fallback to CDC mode: %s", esp_err_to_name(msc_err));
      usb_mode = UsbMode::kCdc;
      UpdateState([&](SharedState& s) {
        s.usb_msc_mode = false;
        s.usb_error = "MSC init failed: " + std::string(esp_err_to_name(msc_err));
      });
      SaveUsbModeToNvs(usb_mode);
    }
  } else {
    UpdateState([](SharedState& s) { s.usb_error.clear(); });
  }

  InitGpios();
  InitHeaterPwm();
  InitFanPwm();
  ESP_ERROR_CHECK(InitSpiBus());
  ESP_ERROR_CHECK(adc1.Init(SPI2_HOST));
  ESP_ERROR_CHECK(adc2.Init(SPI2_HOST));
  ESP_ERROR_CHECK(adc3.Init(SPI2_HOST));
  esp_err_t ina_err = InitIna219();
  if (ina_err != ESP_OK) {
    ESP_LOGE(TAG, "INA219 init failed: %s", esp_err_to_name(ina_err));
  }

  if (!app_config.wifi_from_file) {
    ESP_LOGI(TAG, "Using default Wi-Fi config (SSID: %s)", app_config.wifi_ssid.c_str());
  }
  InitWifi(app_config.wifi_ssid, app_config.wifi_password);
  StartHttpServer();

  // Pin tasks to different cores: ADC на core0 (prio 4), шаги на core1 (prio 3) — idle0 свободен для WDT
  xTaskCreatePinnedToCore(&AdcTask, "adc_task", 4096, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(&StepperTask, "stepper_task", 4096, nullptr, 3, nullptr, 1);
  if (ina_err == ESP_OK) {
    xTaskCreatePinnedToCore(&Ina219Task, "ina219_task", 3072, nullptr, 2, nullptr, 0);
  }
  xTaskCreatePinnedToCore(&FanTachTask, "fan_tach_task", 2048, nullptr, 2, nullptr, 0);

  ESP_LOGI(TAG, "System ready");
}
