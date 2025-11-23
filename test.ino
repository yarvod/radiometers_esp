#include "ltc2440.h"
#include <SPI.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <WebServer.h>
#include <ArduinoJson.h>

// Пины подключения
#define ADC_MISO 4
#define ADC_MOSI 5
#define ADC_SCK 6
#define ADC_CS1 16
#define ADC_CS2 15
#define ADC_CS3 7
#define RELAY_PIN 17

// Пины шагового двигателя TMC2208
#define STEPPER_EN 35
#define STEPPER_DIR 36
#define STEPPER_STEP 37

// Опорное напряжение (вольт) - диапазон ±Vref/2
#define VREF 4.096f

// Настройки WiFi
const char* ssid = "ASC_WiFi";
const char* password = "ran/fian/asc/2010";
const char* deviceHostname = "miap-device"; // Задайте удобное имя хоста для поиска в сети
bool useCustomMac = true;                  // Включите true, если хотите задать свой MAC
uint8_t customMac[6] = {0x10, 0x00, 0x3B, 0x6E, 0x83, 0x70}; // Фиксированный MAC, найденный при сканировании

// Веб-сервер на порту 80
WebServer server(80);

// Создание объектов для каждого АЦП
LTC2440 adc1(ADC_CS1);
LTC2440 adc2(ADC_CS2);
LTC2440 adc3(ADC_CS3);

// Поправки нуля для каждого канала
float offset1 = 0;
float offset2 = 0;
float offset3 = 0;

// Переменные для хранения последних измерений
float lastVoltage1 = 0;
float lastVoltage2 = 0;
float lastVoltage3 = 0;
unsigned long lastUpdate = 0;

// Переменные для управления шаговым двигателем
bool stepperEnabled = false;
bool stepperDirection = true;
int stepperSpeed = 500; // Задержка между шагами в микросекундах (по умолчанию быстрее)
int targetSteps = 0;
int currentSteps = 0;
bool moving = false;
unsigned long lastStepTime = 0;
unsigned long lastADCTime = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  
  // Настройка пина реле
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  
  // Настройка пинов шагового двигателя
  pinMode(STEPPER_EN, OUTPUT);
  pinMode(STEPPER_DIR, OUTPUT);
  pinMode(STEPPER_STEP, OUTPUT);
  
  // Выключение двигателя при старте
  digitalWrite(STEPPER_EN, HIGH);
  digitalWrite(STEPPER_DIR, LOW);
  digitalWrite(STEPPER_STEP, LOW);
  
  Serial.println("Initializing LTC2440 ADCs...");
  
  // Настройка пинов SPI для ESP32
  SPI.begin(ADC_SCK, ADC_MISO, ADC_MOSI);
  
  // Инициализация АЦП
  adc1.Init();
  adc2.Init();
  adc3.Init();
  
  Serial.println("LTC2440 ADCs initialized successfully!");
  
  // Подключение к WiFi
  connectToWiFi();
  
  // Настройка веб-сервера
  setupWebServer();
  
  // Калибровка нуля
  calibrateZero();
  
  Serial.println("System ready!");
  Serial.println("ADC1(V)\t\tADC2(V)\t\tADC3(V)");
  Serial.println("----------------------------------------");
  
  lastADCTime = millis();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  if (strlen(deviceHostname) > 0) {
    bool hostnameOk = WiFi.setHostname(deviceHostname);
    Serial.printf("Setting hostname to '%s'... %s\n", deviceHostname, hostnameOk ? "ok" : "failed");
  }
  if (useCustomMac) {
    esp_err_t macResult = esp_wifi_set_mac(WIFI_IF_STA, customMac);
    Serial.printf("Applying custom MAC %02X:%02X:%02X:%02X:%02X:%02X... %s\n",
                  customMac[0], customMac[1], customMac[2], customMac[3], customMac[4], customMac[5],
                  macResult == ESP_OK ? "ok" : "failed");
  }
  uint8_t currentMac[6];
  WiFi.macAddress(currentMac);
  Serial.printf("Current MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                currentMac[0], currentMac[1], currentMac[2], currentMac[3], currentMac[4], currentMac[5]);

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");
    const char* currentHostname = WiFi.getHostname();
    Serial.printf("Hostname: %s\n", currentHostname ? currentHostname : "<unset>");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Signal (RSSI): %ld dBm\n", WiFi.RSSI());
  } else {
    Serial.println();
    Serial.printf("Failed to connect to WiFi! Status: %d, attempts: %d\n", WiFi.status(), attempts);
    const char* lastHostname = WiFi.getHostname();
    Serial.printf("Last hostname: %s\n", lastHostname ? lastHostname : "<unset>");
  }
}

void setupWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
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
    
    <div class="controls">
      <div class="control-panel">
        <h3>ADC Controls</h3>
        <button class="btn" onclick="refreshData()">Refresh Data</button>
        <button class="btn btn-calibrate" onclick="calibrate()">Calibrate Zero</button>
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
          <input type="number" id="speed" value="500" min="50" max="2000">
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
    </div>
  </div>

  <script>
    function updateData(data) {
      document.getElementById('voltage1').textContent = data.voltage1.toFixed(6) + ' V';
      document.getElementById('voltage2').textContent = data.voltage2.toFixed(6) + ' V';
      document.getElementById('voltage3').textContent = data.voltage3.toFixed(6) + ' V';
      document.getElementById('stepperStatus').textContent = data.stepperEnabled ? 'Enabled' : 'Disabled';
      document.getElementById('stepperPosition').textContent = data.stepperPosition;
      document.getElementById('stepperTarget').textContent = data.stepperTarget;
      document.getElementById('stepperMoving').textContent = data.stepperMoving ? 'Yes' : 'No';
      document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
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
    
    // Auto-refresh every 2 seconds
    setInterval(refreshData, 2000);
    
    // Initial load
    refreshData();
  </script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  // API endpoints
  server.on("/data", HTTP_GET, handleGetData);
  server.on("/calibrate", HTTP_POST, handleCalibrate);
  server.on("/stepper/enable", HTTP_POST, handleStepperEnable);
  server.on("/stepper/disable", HTTP_POST, handleStepperDisable);
  server.on("/stepper/move", HTTP_POST, handleStepperMove);
  server.on("/stepper/stop", HTTP_POST, handleStepperStop);
  server.on("/stepper/zero", HTTP_POST, handleStepperZero);

  server.begin();
  Serial.println("HTTP server started");
}

void handleGetData() {
  StaticJsonDocument<300> doc;
  doc["voltage1"] = lastVoltage1;
  doc["voltage2"] = lastVoltage2;
  doc["voltage3"] = lastVoltage3;
  doc["timestamp"] = lastUpdate;
  doc["stepperEnabled"] = stepperEnabled;
  doc["stepperPosition"] = currentSteps;
  doc["stepperTarget"] = targetSteps;
  doc["stepperMoving"] = moving;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleCalibrate() {
  server.send(200, "application/json", "{\"status\":\"calibration_started\"}");
  calibrateZero();
}

void handleStepperEnable() {
  enableStepper();
  server.send(200, "application/json", "{\"status\":\"stepper_enabled\"}");
}

void handleStepperDisable() {
  disableStepper();
  server.send(200, "application/json", "{\"status\":\"stepper_disabled\"}");
}

void handleStepperMove() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }
    
    int steps = doc["steps"] | 400;
    String direction = doc["direction"] | "forward";
    int speed = doc["speed"] | 500;
    
    // Ограничиваем скорость для безопасности
    if (speed < 50) speed = 50;
    if (speed > 2000) speed = 2000;
    
    moveStepper(steps, direction == "forward", speed);
    server.send(200, "application/json", "{\"status\":\"movement_started\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"No data\"}");
  }
}

void handleStepperStop() {
  stopStepper();
  server.send(200, "application/json", "{\"status\":\"movement_stopped\"}");
}

void handleStepperZero() {
  currentSteps = 0;
  targetSteps = 0;
  server.send(200, "application/json", "{\"status\":\"position_zeroed\"}");
}

void enableStepper() {
  digitalWrite(STEPPER_EN, LOW);
  stepperEnabled = true;
  Serial.println("Stepper motor enabled");
}

void disableStepper() {
  digitalWrite(STEPPER_EN, HIGH);
  stepperEnabled = false;
  moving = false;
  Serial.println("Stepper motor disabled");
}

void moveStepper(int steps, bool direction, int speed) {
  if (!stepperEnabled) {
    Serial.println("Error: Stepper motor not enabled");
    return;
  }
  
  targetSteps = currentSteps + (direction ? steps : -steps);
  stepperDirection = direction;
  stepperSpeed = speed;
  moving = true;
  
  digitalWrite(STEPPER_DIR, direction ? HIGH : LOW);
  
  Serial.printf("Starting stepper movement: %d steps, direction: %s, speed: %d us\n", 
                steps, direction ? "forward" : "backward", speed);
}

void stopStepper() {
  moving = false;
  Serial.println("Stepper movement stopped");
}

void doStepperStep() {
  if (moving && stepperEnabled) {
    unsigned long currentTime = micros();
    if (currentTime - lastStepTime >= stepperSpeed) {
      // Генерируем шаг
      digitalWrite(STEPPER_STEP, HIGH);
      delayMicroseconds(5); // Очень короткий импульс
      digitalWrite(STEPPER_STEP, LOW);
      
      if (stepperDirection) {
        currentSteps++;
      } else {
        currentSteps--;
      }
      
      lastStepTime = currentTime;
      
      // Проверяем достижение цели
      if ((stepperDirection && currentSteps >= targetSteps) ||
          (!stepperDirection && currentSteps <= targetSteps)) {
        moving = false;
        Serial.println("Stepper movement completed");
      }
    }
  }
}

void calibrateZero() {
  Serial.println("Starting zero calibration...");
  Serial.println("Closing relay to short inputs...");
  
  digitalWrite(RELAY_PIN, HIGH);
  delay(1000);
  
  const int numSamples = 100;
  const int ignoreSamples = 10;
  float sum1 = 0, sum2 = 0, sum3 = 0;
  int validSamples = 0;
  
  Serial.println("Taking calibration samples...");
  
  for (int i = 0; i < numSamples; i++) {
    server.handleClient();
    doStepperStep(); // Продолжаем управлять двигателем во время калибровки
    
    int32_t raw1 = adc1.Read();
    int32_t raw2 = adc2.Read();
    int32_t raw3 = adc3.Read();
    
    float voltage1 = (raw1 * VREF / 2.0) / (1 << 23);
    float voltage2 = (raw2 * VREF / 2.0) / (1 << 23);
    float voltage3 = (raw3 * VREF / 2.0) / (1 << 23);
    
    if (i >= ignoreSamples) {
      sum1 += voltage1;
      sum2 += voltage2;
      sum3 += voltage3;
      validSamples++;
    }
    
    unsigned long currentTime = millis();
    while (millis() - currentTime < 200) {
      server.handleClient();
      doStepperStep();
      delay(1);
    }
  }
  
  offset1 = sum1 / validSamples;
  offset2 = sum2 / validSamples;
  offset3 = sum3 / validSamples;
  
  digitalWrite(RELAY_PIN, LOW);
  
  Serial.println("Calibration completed!");
}

void loop() {
  server.handleClient();
  doStepperStep(); // Высокоприоритетная функция - вызывается как можно чаще
  
  // Чтение АЦП с интервалом 200мс
  if (millis() - lastADCTime >= 200) {
    int32_t raw1 = adc1.Read();
    int32_t raw2 = adc2.Read();
    int32_t raw3 = adc3.Read();
    
    lastVoltage1 = (raw1 * VREF / 2.0) / (1 << 23) - offset1;
    lastVoltage2 = (raw2 * VREF / 2.0) / (1 << 23) - offset2;
    lastVoltage3 = (raw3 * VREF / 2.0) / (1 << 23) - offset3;
    
    lastUpdate = millis();
    lastADCTime = millis();
    
    Serial.printf("%+10.6f\t%+10.6f\t%+10.6f\n", lastVoltage1, lastVoltage2, lastVoltage3);
  }
  
  delay(1); // Короткая задержка для стабильности
}
