#include "web_ui.h"

const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>Radiometers Termostatica</title>
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
    .btn-small { padding: 6px 12px; font-size: 14px; }
    .status { text-align: center; margin: 10px 0; color: #7f8c8d; }
    .stepper-status { background: #fff3cd; padding: 10px; border-radius: 5px; margin: 10px 0; }
    .form-group { margin: 10px 0; }
    label { display: block; margin-bottom: 5px; font-weight: bold; }
    input, select { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }
    .speed-info { font-size: 12px; color: #666; margin-top: 5px; }
    .note { font-size: 12px; color: #666; margin-top: 5px; }
    .files-panel { background: #f4f4ff; padding: 20px; border-radius: 8px; margin-top: 20px; }
    .file-actions { display: flex; align-items: center; justify-content: space-between; gap: 10px; flex-wrap: wrap; }
    .file-actions .file-buttons { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .file-row { display: flex; align-items: center; justify-content: space-between; padding: 6px 0; border-bottom: 1px solid #eee; }
    .file-info { display: flex; align-items: center; gap: 10px; }
    .file-checkbox { width: 18px; height: 18px; }
    .checkbox-label { display: flex; align-items: center; gap: 8px; font-weight: 600; }
    .file-name { word-break: break-all; }
    .temp-label { font-weight: 600; cursor: help; text-decoration: underline dotted; }
    .tabs { margin-top: 20px; }
    .tab-buttons { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 12px; }
    .tab-button { background: #ecf0f1; border: none; padding: 10px 16px; border-radius: 6px; cursor: pointer; font-weight: 600; }
    .tab-button.active { background: #3498db; color: #fff; }
    .tab-content { display: none; }
    .tab-content.active { display: block; }
    .charts { margin: 16px 0; border: 1px solid #dde; border-radius: 8px; padding: 12px; background: #fafafa; }
    .chart-row { display: flex; flex-wrap: wrap; gap: 12px; }
    .chart-box { flex: 1; min-width: 320px; background: #fff; border: 1px solid #eee; border-radius: 6px; padding: 10px; box-shadow: 0 2px 4px rgba(0,0,0,0.03); }
    .chart-controls { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin-top: 8px; }
    .chart-controls input { width: 140px; }
    canvas.chart { width: 100%; height: 240px; }
    .legend { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 6px; }
    .legend-item { display: flex; align-items: center; gap: 6px; cursor: pointer; user-select: none; padding: 4px 8px; border-radius: 4px; }
    .legend-item.disabled { opacity: 0.4; text-decoration: line-through; }
    .legend-color { width: 14px; height: 14px; border-radius: 3px; }
    .chip-select { display: flex; flex-wrap: wrap; gap: 8px; }
    .chip-option { background: #ecf0f1; border: 1px solid #d8dee9; color: #2c3e50; border-radius: 999px; padding: 6px 10px; font-size: 12px; font-weight: 600; cursor: pointer; }
    .chip-option.selected { background: #e6f8ed; border-color: rgba(46, 139, 87, 0.35); color: #2e8b57; }
  </style>
</head>
<body>
  <div class="container">
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
      <div class="adc-channel">
        <div class="channel-name">Temperatures</div>
        <div id="tempList">
          <div class="voltage">--.- &deg;C</div>
          <div>Sensor list will appear here</div>
        </div>
      </div>
    </div>

    <details class="charts" open>
      <summary><strong>Мониторинг графиков</strong></summary>
      <div class="chart-controls">
        <label for="monitorDuration">Окно, секунд (0 — без лимита):</label>
        <input type="number" id="monitorDuration" value="60" min="0" step="10">
        <button class="btn btn-small" id="monitorStartBtn">Запустить монитор</button>
        <button class="btn btn-small btn-stop" id="monitorStopBtn" disabled>Остановить</button>
        <span id="monitorStatus" class="note">Выключен</span>
      </div>
      <div class="chart-row">
        <div class="chart-box">
          <h4>ADC (от времени)</h4>
          <canvas id="chartAdc" class="chart"></canvas>
          <div id="legendAdc" class="legend"></div>
        </div>
        <div class="chart-box">
          <h4>Температуры (от времени)</h4>
          <canvas id="chartTemp" class="chart"></canvas>
          <div id="legendTemp" class="legend"></div>
        </div>
      </div>
    </details>
    
    <div class="tabs">
      <div class="tab-buttons">
        <button class="tab-button active" data-tab="tab-measurement">Measurement</button>
        <button class="tab-button" data-tab="tab-heat">Heat</button>
        <button class="tab-button" data-tab="tab-stepper">Stepper</button>
        <button class="tab-button" data-tab="tab-wifi">Wi‑Fi</button>
        <button class="tab-button" data-tab="tab-gps">GPS</button>
        <button class="tab-button" data-tab="tab-system">System</button>
        <button class="tab-button" data-tab="tab-files">Files</button>
        <button class="tab-button" data-tab="tab-flash">Flash</button>
      </div>

      <div id="tab-measurement" class="tab-content active">
        <div class="controls">
          <div class="control-panel">
            <h3>ADC Controls</h3>
            <button class="btn" onclick="refreshData()">Refresh Data</button>
            <button class="btn btn-calibrate" onclick="calibrate()">Calibrate Zero</button>
          </div>

          <div class="control-panel">
            <h3>Measurements</h3>
            <div class="form-group">
              <label for="logFilename">Filename (on SD)</label>
              <input type="text" id="logFilename" value="log.txt">
            </div>
            <div class="form-group">
              <label><input type="checkbox" id="logUseMotor"> Use motor (home + 90° sweep)</label>
            </div>
            <div class="form-group">
              <label for="logDuration">Averaging duration, sec</label>
              <input type="number" id="logDuration" value="1" min="0.1" step="0.1">
            </div>
            <div>Status: <span id="logStatus">Idle</span></div>
            <button class="btn" onclick="startLog()">Start Logging</button>
            <button class="btn btn-stop" onclick="stopLog()">Stop Logging</button>
          </div>
        </div>
        <div class="status">
          Last update: <span id="lastUpdate">-</span>
          <br>USB mode: <span id="usbModeLabel">Serial (logs/flash)</span>
        </div>
      </div>

      <div id="tab-heat" class="tab-content">
        <div class="controls">
          <div class="control-panel">
            <h3>Heater Control</h3>
            <div class="form-group">
              <label for="heaterPower">Power (%)</label>
              <input type="number" id="heaterPower" value="0" min="0" max="100" step="0.1">
            </div>
            <button class="btn" onclick="setHeater()">Set Heater</button>
          </div>

          <div class="control-panel">
            <h3>Heater PID Control</h3>
            <div class="form-group">
              <label for="pidSetpoint">Target Temp (°C)</label>
              <input type="number" id="pidSetpoint" value="25" step="0.1">
            </div>
            <div class="form-group">
              <label>PID sensors</label>
              <div id="pidSensorChips" class="chip-select"></div>
              <div class="note">Можно выбрать несколько датчиков.</div>
            </div>
            <div class="form-group">
              <label for="pidKp">Kp</label>
              <input type="number" id="pidKp" value="1" step="0.01">
            </div>
            <div class="form-group">
              <label for="pidKi">Ki</label>
              <input type="number" id="pidKi" value="0" step="0.01">
            </div>
            <div class="form-group">
              <label for="pidKd">Kd</label>
              <input type="number" id="pidKd" value="0" step="0.01">
            </div>
            <div>Status: <span id="pidStatus">Off</span></div>
            <button class="btn" onclick="applyPid()">Apply PID</button>
            <button class="btn btn-stepper" onclick="enablePid()">Enable PID</button>
            <button class="btn btn-stop" onclick="disablePid()">Disable PID</button>
          </div>

          <div class="control-panel">
            <h3>Fan Control</h3>
            <div class="form-group">
              <label for="fanPower">Fan Power (%)</label>
              <input type="number" id="fanPower" value="100" min="0" max="100" step="1">
            </div>
            <button class="btn" onclick="setFan()">Set Fan</button>
          </div>
        </div>
      </div>

      <div id="tab-stepper" class="tab-content">
        <div class="controls">
          <div class="control-panel">
            <h3>Stepper Motor Control</h3>
            <div class="stepper-status">
              Status: <span id="stepperStatus">Disabled</span><br>
              Position: <span id="stepperPosition">0</span> steps<br>
              Target: <span id="stepperTarget">0</span> steps<br>
              Home offset: <span id="stepperHomeOffsetDisplay">0</span> steps<br>
              Hall: raw <span id="motorHallRaw">?</span>, active <span id="motorHallActive">0</span>, triggered <span id="motorHallTriggered">No</span><br>
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
              <input type="number" id="speed" value="1000" step="1" onchange="saveStepperSettings()">
              <div class="speed-info">Lower value = faster speed. Значение берётся из веб-инпута без ограничений и сохраняется в config.txt.</div>
            </div>

            <div class="form-group">
              <label for="homeOffsetSteps">Home offset after Hall (steps):</label>
              <input type="number" id="homeOffsetSteps" value="0" step="1" onchange="saveStepperSettings()">
              <div class="speed-info">После Hall мотор отъезжает на этот signed offset, затем позиция становится 0.</div>
            </div>

            <div class="form-group">
              <label for="loggingMotorSteps">Logging load position steps:</label>
              <input type="number" id="loggingMotorSteps" value="100" min="1" max="20000" step="1" onchange="saveStepperSettings()">
              <div class="speed-info">В цикле логирования мотор уходит от нуля на это число шагов, чтобы смотреть на нагрузку.</div>
            </div>

            <div class="form-group">
              <label for="loggingReturnMode">Logging return mode:</label>
              <select id="loggingReturnMode" onchange="saveStepperSettings()">
                <option value="home">Find Hall + offset every cycle</option>
                <option value="steps">Return by saved step count</option>
              </select>
              <div class="speed-info">По шагам быстрее, но если мотор пропустит шаги, ошибка может накапливаться.</div>
            </div>

            <div class="form-group">
              <label for="hallActiveLevel">Hall active level:</label>
              <select id="hallActiveLevel" onchange="saveStepperSettings()">
                <option value="0">0 / LOW</option>
                <option value="1">1 / HIGH</option>
              </select>
            </div>
            
            <button class="btn btn-stepper" onclick="enableStepper()">Enable Motor</button>
            <button class="btn btn-stop" onclick="disableStepper()">Disable Motor</button>
            <button class="btn" onclick="moveStepper()">Move</button>
            <button class="btn btn-stop" onclick="stopStepper()">Stop</button>
            <button class="btn" onclick="setZero()">Set Position to Zero</button>
            <button class="btn" onclick="goToOffsetZero()">Go To Zero (Hall + Offset)</button>
            <button class="btn" onclick="saveStepperSettings()">Save Motor Settings</button>
          </div>
        </div>
      </div>

      <div id="tab-wifi" class="tab-content">
        <div class="controls">
          <div class="control-panel">
            <h3>Wi‑Fi</h3>
            <div class="form-group">
              <div>RSSI: <span id="wifiRssi">-- dBm</span></div>
              <div>Качество: <span id="wifiQuality">-- %</span></div>
            </div>
            <div class="form-group">
              <label for="wifiMode">Mode</label>
              <select id="wifiMode">
                <option value="sta">Connect to Wi‑Fi</option>
                <option value="ap">Share own Wi‑Fi</option>
              </select>
            </div>
            <div class="form-group">
              <label for="wifiSsid">SSID</label>
              <input type="text" id="wifiSsid" value="">
            </div>
            <div class="form-group">
              <label for="wifiPassword">Password</label>
              <input type="text" id="wifiPassword" value="">
            </div>
            <button class="btn" onclick="applyWifi()">Apply Wi‑Fi</button>
          </div>
          <div class="control-panel">
            <h3>Network</h3>
            <div class="form-group">
              <div>Wi‑Fi IP: <span id="wifiIpLabel">--</span></div>
              <div>Ethernet link: <span id="ethLink">--</span></div>
              <div>Ethernet IP: <span id="ethIp">--</span></div>
            </div>
            <div class="form-group">
              <label for="netMode">Interfaces</label>
              <select id="netMode">
                <option value="wifi">Wi‑Fi only</option>
                <option value="eth">Ethernet only</option>
                <option value="both">Wi‑Fi + Ethernet</option>
              </select>
            </div>
            <div class="form-group">
              <label for="netPriority">Priority</label>
              <select id="netPriority">
                <option value="wifi">Prefer Wi‑Fi</option>
                <option value="eth">Prefer Ethernet</option>
              </select>
            </div>
            <button class="btn" onclick="applyNetwork()">Apply Network</button>
          </div>
        </div>
      </div>

      <div id="tab-gps" class="tab-content">
        <div class="controls">
          <div class="control-panel">
            <h3>GPS / GNSS Station</h3>
            <div class="form-group">
              <div>Physical Unicore port: <strong>COM2</strong></div>
              <div>Actual receiver mode: <span id="gpsActualMode">--</span></div>
              <div>Antenna short: <span id="gpsAntennaShort">--</span> (raw <span id="gpsAntennaShortRaw">--</span>)</div>
              <div>Position fix: <span id="gpsPositionStatus">--</span></div>
              <div>GPS time: <span id="gpsTimeStatus">--</span></div>
            </div>
            <div class="form-group">
              <label for="gpsMode">Configured mode</label>
              <select id="gpsMode">
                <option value="base_time_60">BASE TIME 60</option>
                <option value="base">BASE</option>
                <option value="rover_uav">ROVER UAV</option>
                <option value="keep">Keep current mode</option>
              </select>
              <div class="note">BASE TIME 60 sends UNLOG, MODE BASE TIME 60, waits 60 seconds, then enables RTCM on COM2.</div>
            </div>
            <div class="form-group">
              <label for="gpsRtcmTypes">RTCM message types</label>
              <input type="text" id="gpsRtcmTypes" value="1004,1006,1033" placeholder="1004,1006,1033">
              <div class="note">Comma or space separated. Default: 1004, 1006, 1033. Output period is 30 seconds.</div>
            </div>
            <button class="btn" onclick="applyGpsConfig()">Save GPS Settings</button>
            <button class="btn" onclick="probeGpsMode()">Refresh Actual Mode</button>
            <div class="note" id="gpsConfigStatus">Settings are saved to config.txt. Reconfigure is queued without restarting the web UI.</div>
          </div>
        </div>
      </div>

      <div id="tab-system" class="tab-content">
        <div class="controls">
          <div class="control-panel">
            <h3>System</h3>
            <div class="form-group">
              <div>External modules power: <span id="externalPowerState">--</span></div>
              <div>Storage backend: <span id="storageBackendLabel">--</span></div>
              <div>SD mounted: <span id="sdMountedLabel">--</span></div>
              <div>Internal flash mounted: <span id="internalFlashMountedLabel">--</span></div>
              <div>Active storage mounted: <span id="activeStorageMountedLabel">--</span></div>
              <div>Heap free: <span id="heapFreeBytes">--</span> bytes</div>
              <div>Heap min free: <span id="heapMinFreeBytes">--</span> bytes</div>
              <div>Heap largest block: <span id="heapLargestFreeBlockBytes">--</span> bytes</div>
              <div>Internal heap free: <span id="heapInternalFreeBytes">--</span> bytes</div>
              <div>Internal largest block: <span id="heapInternalLargestFreeBlockBytes">--</span> bytes</div>
              <div>PSRAM heap free: <span id="heapPsramFreeBytes">--</span> bytes</div>
              <div>PSRAM largest block: <span id="heapPsramLargestFreeBlockBytes">--</span> bytes</div>
              <div>MinIO upload attempts: <span id="minioUploadAttempts">--</span></div>
              <div>MinIO last attempt: <span id="minioLastAttempt">--</span></div>
            </div>
            <div class="form-group">
              <label for="storageBackend">Storage for logs/uploads</label>
              <select id="storageBackend">
                <option value="sd">SD card</option>
                <option value="internal_flash">Internal flash</option>
              </select>
              <div class="note">Internal flash is a fallback when SD is unavailable; it has limited write endurance and space.</div>
            </div>
            <div class="form-group">
              <label for="externalPowerOffMs">Power cycle off time, ms</label>
              <input type="number" id="externalPowerOffMs" value="1000" min="100" max="30000" step="100">
            </div>
            <button class="btn" onclick="applyStorageBackend()">Apply Storage</button>
            <button class="btn" onclick="remountStorage()">Remount Active Storage</button>
            <button class="btn" onclick="setExternalPower(true)">External Power ON</button>
            <button class="btn btn-stop" onclick="setExternalPower(false)">External Power OFF</button>
            <button class="btn" onclick="cycleExternalPower()">Power Cycle</button>
            <button class="btn" onclick="syncConfigInternalFlash()">Sync config to ESP flash</button>
            <button class="btn btn-stop" onclick="restartDevice()">Restart ESP32</button>
            <div class="note" id="systemActionStatus">Config is normally saved to SD and ESP internal flash. Use sync if SD config was edited manually.</div>
          </div>
        </div>
      </div>

      <div id="tab-flash" class="tab-content">
        <div class="files-panel">
          <h3>Внутренняя flash ESP</h3>
          <div class="form-group file-actions">
            <label class="checkbox-label"><input type="checkbox" id="selectAllFlashFiles"> Выбрать все</label>
            <div class="file-buttons">
              <button class="btn" onclick="loadFlash()">Обновить</button>
              <button class="btn" onclick="transferFlashToSd()">to_upload → SD</button>
              <button class="btn btn-stop" onclick="clearFlashUploadedFiles()">Очистить uploaded</button>
              <button class="btn btn-stop" id="deleteSelectedFlashBtn" disabled>Удалить выбранные</button>
              <button class="btn" onclick="syncConfigInternalFlash()">Sync config to ESP flash</button>
            </div>
          </div>
          <div class="flash-summary">
            <div>Flash total: <span id="flashTotalBytes">--</span></div>
            <div>Partition bytes: <span id="flashPartitionBytes">--</span></div>
            <div>Internal FS: <span id="flashInternalFsBytes">--</span></div>
            <div>NVS entries: <span id="flashNvsEntries">--</span></div>
          </div>
          <div class="file-nav">
            <div>
              <button class="btn btn-small" onclick="flashGoUp()">⬆️ ..</button>
              <span id="flashPathLabel"></span>
            </div>
            <div class="file-pages">
              <button class="btn btn-small" onclick="flashPagePrev()">←</button>
              <span id="flashPageInfo"></span>
              <button class="btn btn-small" onclick="flashPageNext()">→</button>
            </div>
          </div>
          <div id="flashList"></div>
          <div class="note">config.txt хранится в NVS как резервная копия. FAT-раздел /flashfs используется только если выбран Internal flash. Удаление директорий не рекурсивное.</div>
        </div>
      </div>

      <div id="tab-files" class="tab-content">
        <div class="files-panel">
          <h3>Файлы на SD</h3>
          <div class="form-group file-actions">
            <label class="checkbox-label"><input type="checkbox" id="selectAllFiles"> Выбрать все</label>
            <div class="file-buttons">
              <button class="btn" onclick="loadFiles()">Обновить список</button>
              <button class="btn" onclick="transferSdToFlash()">to_upload → Flash</button>
              <button class="btn btn-stop" onclick="clearUploadedFiles()">Очистить uploaded</button>
              <button class="btn btn-stop" id="deleteSelectedBtn" disabled>Удалить выбранные</button>
            </div>
          </div>
          <div class="file-nav">
            <div>
              <button class="btn btn-small" onclick="goUp()">⬆️ ..</button>
              <span id="filePathLabel"></span>
            </div>
            <div class="file-pages">
              <button class="btn btn-small" onclick="pagePrev()">←</button>
              <span id="filePageInfo"></span>
              <button class="btn btn-small" onclick="pageNext()">→</button>
            </div>
          </div>
          <div id="fileList"></div>
          <div class="note">Можно скачать или удалить файл. config.txt защищён от удаления. Одновременная запись логов и скачивание синхронизированы мьютексом.</div>
        </div>
        <div class="control-panel">
          <h3>Облако (MinIO) и MQTT</h3>
          <div class="form-group">
            <label for="deviceId">Device ID</label>
            <input type="text" id="deviceId" placeholder="dev2">
          </div>
          <div class="form-group">
            <label><input type="checkbox" id="minioEnabled"> Включить MinIO</label>
          </div>
          <div class="form-group">
            <label for="minioEndpoint">MinIO endpoint (http://host:9000)</label>
            <input type="text" id="minioEndpoint" placeholder="http://...">
          </div>
          <div class="form-group">
            <label for="minioBucket">MinIO bucket</label>
            <input type="text" id="minioBucket" placeholder="bucket">
          </div>
          <div class="form-group">
            <label for="minioAccessKey">MinIO access key</label>
            <input type="text" id="minioAccessKey">
          </div>
          <div class="form-group">
            <label for="minioSecretKey">MinIO secret key</label>
            <input type="password" id="minioSecretKey">
          </div>
          <div class="form-group">
            <label for="mqttUri">MQTT URI (e.g. mqtt://host:1883)</label>
            <input type="text" id="mqttUri" placeholder="mqtt://...">
          </div>
          <div class="form-group">
            <label><input type="checkbox" id="mqttEnabled"> Включить MQTT</label>
          </div>
          <div class="form-group">
            <label for="mqttUser">MQTT user</label>
            <input type="text" id="mqttUser">
          </div>
          <div class="form-group">
            <label for="mqttPassword">MQTT password</label>
            <input type="password" id="mqttPassword">
          </div>
          <button class="btn" onclick="applyCloudConfig()">Сохранить настройки</button>
          <div class="note">S3-загрузки берут файлы из папки to_upload и после успеха перемещают в uploaded. MQTT будет использовать deviceId в именах очередей/топиков.</div>
        </div>
      </div>
    </div>
    </div>
  </div>

<script>
    let measurementsInitialized = false;
    let pidEditing = false;
    let pidEntries = [];
    const pidSelection = new Set();
    const selectedFiles = new Set();
    const selectedFlashFiles = new Set();
    let cachedFiles = [];
    let cachedFlashFiles = [];
    const monitorState = {
      enabled: false,
      durationMs: 60000,
      adc: {v1: [], v2: [], v3: []},
      temps: {},
      visible: {
        adc: {ADC1: true, ADC2: true, ADC3: true},
        temps: {}
      }
    };
    const filesState = { path: '', page: 0, pageSize: 10, total: 0, totalPages: 0 };
    const flashState = { path: '', page: 0, pageSize: 10, total: 0, totalPages: 0 };

    function getBaseName(path) {
      if (!path) return '';
      const idx = path.lastIndexOf('/');
      return idx >= 0 ? path.slice(idx + 1) : path;
    }

    function getFilePath(item) {
      if (item && typeof item === 'object') {
        return item.path || item.name || '';
      }
      return typeof item === 'string' ? item : '';
    }

    function setValueIfIdle(id, value) {
      const el = document.getElementById(id);
      if (el && document.activeElement !== el) {
        el.value = value;
      }
    }

    function getTempEntries(data) {
      const out = [];
      if (!data) return out;
      if (Array.isArray(data.tempSensors)) {
        const labels = Array.isArray(data.tempLabels) ? data.tempLabels : [];
        const addresses = Array.isArray(data.tempAddresses) ? data.tempAddresses : [];
        data.tempSensors.forEach((value, idx) => {
          const label = labels[idx] || `t${idx + 1}`;
          out.push({
            key: label || `t${idx + 1}`,
            label: label || `t${idx + 1}`,
            value,
            address: addresses[idx] || '',
          });
        });
        return out;
      }
      const obj = data.tempSensors;
      if (obj && typeof obj === 'object') {
        Object.entries(obj).forEach(([key, entry]) => {
          if (entry && typeof entry === 'object') {
            out.push({ key, label: key, value: entry.value, address: entry.address || '' });
          } else {
            out.push({ key, label: key, value: entry, address: '' });
          }
        });
      }
      return out;
    }

    function setPidSelectionFromMask(mask, count) {
      pidSelection.clear();
      for (let i = 0; i < count; i++) {
        if (mask & (1 << i)) pidSelection.add(i);
      }
      if (pidSelection.size === 0 && count > 0) {
        pidSelection.add(0);
      }
    }

    function renderPidChips(entries) {
      pidEntries = entries || [];
      const container = document.getElementById('pidSensorChips');
      if (!container) return;
      if (!pidEntries.length) {
        container.innerHTML = '<span class="note">Нет датчиков</span>';
        return;
      }
      container.innerHTML = '';
      pidEntries.forEach((entry, idx) => {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'chip-option' + (pidSelection.has(idx) ? ' selected' : '');
        const label = entry.label || `t${idx + 1}`;
        btn.textContent = entry.address ? `${label} (${entry.address})` : label;
        btn.onclick = () => togglePidSensor(idx);
        container.appendChild(btn);
      });
    }

    function togglePidSensor(idx) {
      pidEditing = true;
      if (pidSelection.has(idx)) {
        pidSelection.delete(idx);
      } else {
        pidSelection.add(idx);
      }
      renderPidChips(pidEntries);
    }

    function pruneSeries(series, now) {
      if (monitorState.durationMs <= 0) return series;
      const cutoff = now - monitorState.durationMs;
      let idx = 0;
      while (idx < series.length && series[idx].t < cutoff) idx++;
      if (idx > 0) {
        series.splice(0, idx);
      }
      return series;
    }

    function addMonitorSample(data) {
      if (!monitorState.enabled) return;
      const now = Date.now();
      monitorState.adc.v1.push({t: now, v: data.voltage1});
      monitorState.adc.v2.push({t: now, v: data.voltage2});
      monitorState.adc.v3.push({t: now, v: data.voltage3});
      pruneSeries(monitorState.adc.v1, now);
      pruneSeries(monitorState.adc.v2, now);
      pruneSeries(monitorState.adc.v3, now);

      const tempEntries = getTempEntries(data);
      const currentKeys = new Set(tempEntries.map(entry => entry.key));
      tempEntries.forEach(entry => {
        if (!monitorState.temps[entry.key]) monitorState.temps[entry.key] = [];
        if (monitorState.visible.temps[entry.key] === undefined) monitorState.visible.temps[entry.key] = true;
        monitorState.temps[entry.key].push({t: now, v: entry.value});
        pruneSeries(monitorState.temps[entry.key], now);
      });
      // Prune old sensor slots if window expired
      Object.keys(monitorState.temps).forEach(key => {
        const arr = monitorState.temps[key];
        pruneSeries(arr, now);
        if (!currentKeys.has(key) && arr.length === 0) {
          delete monitorState.temps[key];
        }
      });
      renderCharts();
    }

    function renderCharts() {
      renderAdcChart();
      renderTempChart();
      const statusEl = document.getElementById('monitorStatus');
      if (statusEl) {
        const totalSamples = monitorState.adc.v1.length;
        statusEl.textContent = monitorState.enabled
          ? `Работает, точек: ${totalSamples}, окно: ${monitorState.durationMs <= 0 ? '∞' : (monitorState.durationMs/1000 + ' c')}`
          : 'Выключен';
      }
    }

    function niceTicks(min, max, count) {
      const span = max - min;
      if (span === 0) return [min, max];
      const step0 = span / Math.max(1, count - 1);
      const mag = Math.pow(10, Math.floor(Math.log10(step0)));
      const norm = step0 / mag;
      let step = mag;
      if (norm > 5) step = 10 * mag;
      else if (norm > 2) step = 5 * mag;
      else if (norm > 1) step = 2 * mag;
      const tickMin = Math.floor(min / step) * step;
      const tickMax = Math.ceil(max / step) * step;
      const ticks = [];
      for (let v = tickMin; v <= tickMax + step * 0.5; v += step) {
        ticks.push(v);
      }
      return ticks;
    }

    function drawChart(canvasId, series, opts = {}) {
      const canvas = document.getElementById(canvasId);
      if (!canvas) return;
      const ctx = canvas.getContext('2d');
      const width = canvas.clientWidth || 600;
      const height = 240;
      canvas.width = width;
      canvas.height = height;
      ctx.clearRect(0, 0, width, height);

      const padding = {left: 50, right: 10, top: 20, bottom: 28};
      const allPoints = series.flatMap(s => s.data);
      if (allPoints.length < 2) {
        ctx.fillStyle = '#888';
        ctx.font = '14px sans-serif';
        ctx.fillText('Недостаточно данных', padding.left, height / 2);
        return;
      }
      const minT = Math.min(...allPoints.map(p => p.t));
      const maxT = Math.max(...allPoints.map(p => p.t));
      let minV = Math.min(...allPoints.map(p => p.v));
      let maxV = Math.max(...allPoints.map(p => p.v));
      if (minV === maxV) {
        minV -= 1;
        maxV += 1;
      }
      const xScale = (width - padding.left - padding.right) / Math.max(1, (maxT - minT));
      const yScale = (height - padding.top - padding.bottom) / (maxV - minV);

      const formatTime = t => {
        const d = new Date(t);
        return d.toLocaleTimeString();
      };
      const xTicks = 5;
      const yTicks = niceTicks(minV, maxV, 5);

      // Grid + axes
      ctx.strokeStyle = '#e0e0e0';
      ctx.lineWidth = 1;
      yTicks.forEach(v => {
        const y = height - padding.bottom - (v - minV) * yScale;
        ctx.beginPath();
        ctx.moveTo(padding.left, y);
        ctx.lineTo(width - padding.right, y);
        ctx.stroke();
        ctx.fillStyle = '#666';
        ctx.font = '12px sans-serif';
        ctx.fillText(v.toFixed(3), 4, y + 4);
      });
      for (let i = 0; i <= xTicks; i++) {
        const t = minT + (span => span !== undefined ? span : (maxT - minT))(maxT - minT) * (i / xTicks);
        const x = padding.left + (t - minT) * xScale;
        ctx.beginPath();
        ctx.moveTo(x, padding.top);
        ctx.lineTo(x, height - padding.bottom);
        ctx.stroke();
      }
      ctx.strokeStyle = '#bbb';
      ctx.beginPath();
      ctx.moveTo(padding.left, padding.top);
      ctx.lineTo(padding.left, height - padding.bottom);
      ctx.lineTo(width - padding.right, height - padding.bottom);
      ctx.stroke();

      ctx.fillStyle = '#666';
      ctx.font = '12px sans-serif';
      ctx.fillText(formatTime(minT), padding.left, height - 4);
      ctx.fillText(formatTime(maxT), width - padding.right - 80, height - 4);

      // Series
      series.forEach((s, idx) => {
        if (!s.data.length) return;
        ctx.strokeStyle = s.color || ['#2980b9', '#27ae60', '#e67e22', '#8e44ad'][idx % 4];
        ctx.lineWidth = 2;
        ctx.beginPath();
        s.data.forEach((p, i) => {
          const x = padding.left + (p.t - minT) * xScale;
          const y = height - padding.bottom - (p.v - minV) * yScale;
          if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        });
        ctx.stroke();
      });

      // Legend drawn outside (HTML)
    }

    function renderLegend(containerId, items, group) {
      const container = document.getElementById(containerId);
      if (!container) return;
      container.innerHTML = '';
      items.forEach(item => {
        const el = document.createElement('div');
        el.className = 'legend-item' + (item.visible ? '' : ' disabled');
        el.onclick = () => toggleSeries(group, item.key);
        const color = document.createElement('div');
        color.className = 'legend-color';
        color.style.background = item.color;
        const label = document.createElement('span');
        label.textContent = item.label;
        el.appendChild(color);
        el.appendChild(label);
        container.appendChild(el);
      });
    }

    function toggleSeries(group, key) {
      if (!monitorState.visible[group]) monitorState.visible[group] = {};
      const current = monitorState.visible[group][key] !== false;
      monitorState.visible[group][key] = !current;
      renderCharts();
    }

    function renderAdcChart() {
      const series = [];
      const vis = monitorState.visible.adc;
      if (monitorState.adc.v1.length && vis.ADC1 !== false) series.push({key: 'ADC1', label: 'ADC1', color: '#2980b9', data: monitorState.adc.v1});
      if (monitorState.adc.v2.length && vis.ADC2 !== false) series.push({key: 'ADC2', label: 'ADC2', color: '#27ae60', data: monitorState.adc.v2});
      if (monitorState.adc.v3.length && vis.ADC3 !== false) series.push({key: 'ADC3', label: 'ADC3', color: '#e67e22', data: monitorState.adc.v3});
      drawChart('chartAdc', series);
      renderLegend('legendAdc', [
        {key: 'ADC1', label: 'ADC1', color: '#2980b9', visible: vis.ADC1 !== false},
        {key: 'ADC2', label: 'ADC2', color: '#27ae60', visible: vis.ADC2 !== false},
        {key: 'ADC3', label: 'ADC3', color: '#e67e22', visible: vis.ADC3 !== false},
      ], 'adc');
    }

    function renderTempChart() {
      const series = [];
      Object.keys(monitorState.temps).forEach((key, idx) => {
        const arr = monitorState.temps[key];
        const visible = monitorState.visible.temps[key] !== false;
        if (arr && arr.length && visible) {
          series.push({key, label: key, color: ['#8e44ad', '#16a085', '#c0392b', '#34495e', '#2c3e50'][idx % 5], data: arr});
        }
      });
      drawChart('chartTemp', series);
      const legendItems = Object.keys(monitorState.temps).map((key, idx) => ({
        key,
        label: key,
        color: ['#8e44ad', '#16a085', '#c0392b', '#34495e', '#2c3e50'][idx % 5],
        visible: monitorState.visible.temps[key] !== false,
      }));
      renderLegend('legendTemp', legendItems, 'temps');
    }

    function startMonitor() {
      monitorState.enabled = true;
      monitorState.adc = {v1: [], v2: [], v3: []};
      monitorState.temps = {};
      const durInput = document.getElementById('monitorDuration');
      const val = Number(durInput?.value || 60);
      monitorState.durationMs = (Number.isFinite(val) && val > 0) ? val * 1000 : 0;
      const stopBtn = document.getElementById('monitorStopBtn');
      const startBtn = document.getElementById('monitorStartBtn');
      if (stopBtn) stopBtn.disabled = false;
      if (startBtn) startBtn.disabled = true;
      renderCharts();
    }

    function stopMonitor() {
      monitorState.enabled = false;
      const stopBtn = document.getElementById('monitorStopBtn');
      const startBtn = document.getElementById('monitorStartBtn');
      if (stopBtn) stopBtn.disabled = true;
      if (startBtn) startBtn.disabled = false;
      renderCharts();
    }

    function updateData(data) {
      document.getElementById('voltage1').textContent = data.voltage1.toFixed(6) + ' V';
      document.getElementById('voltage2').textContent = data.voltage2.toFixed(6) + ' V';
      document.getElementById('voltage3').textContent = data.voltage3.toFixed(6) + ' V';
      document.getElementById('inaVoltage').textContent = data.inaBusVoltage.toFixed(3) + ' V';
      document.getElementById('inaCurrent').textContent = data.inaCurrent.toFixed(3);
      document.getElementById('inaPower').textContent = data.inaPower.toFixed(3);
      const wifiRssiEl = document.getElementById('wifiRssi');
      const wifiQualEl = document.getElementById('wifiQuality');
      if (wifiRssiEl && data.wifiRssi !== undefined) wifiRssiEl.textContent = data.wifiRssi + ' dBm';
      if (wifiQualEl && data.wifiQuality !== undefined) wifiQualEl.textContent = data.wifiQuality + ' %';
      const wifiIpEl = document.getElementById('wifiIpLabel');
      if (wifiIpEl) wifiIpEl.textContent = data.wifiStaIp || data.wifiIp || '--';
      const ethLinkEl = document.getElementById('ethLink');
      if (ethLinkEl) ethLinkEl.textContent = data.ethLink ? 'Up' : 'Down';
      const ethIpEl = document.getElementById('ethIp');
      if (ethIpEl) ethIpEl.textContent = data.ethIp || '--';
      document.getElementById('fanPowerDisplay').textContent = data.fanPower.toFixed(0) + ' %';
      document.getElementById('fan1RpmDisplay').textContent = data.fan1Rpm;
      document.getElementById('fan2RpmDisplay').textContent = data.fan2Rpm;
      setValueIfIdle('heaterPower', data.heaterPower?.toFixed(1) ?? data.heaterPower ?? 0);
      const list = document.getElementById('tempList');
      const tempEntries = getTempEntries(data);
      if (tempEntries.length > 0) {
        let html = '';
        tempEntries.forEach((entry, idx) => {
          const name = entry.label || `t${idx + 1}`;
          const addr = entry.address || '';
          const title = addr ? ` title="1-Wire ${addr}"` : '';
          const labelHtml = `<span class="temp-label"${title}>${name}</span>`;
          const text = Number.isFinite(entry.value) ? `${entry.value.toFixed(2)} °C` : '--.- °C';
          const classAttr = idx === 0 ? ' class="voltage"' : '';
          html += `<div${classAttr}>${labelHtml}: ${text}</div>`;
        });
        list.innerHTML = html;
      } else {
        list.innerHTML = '<div class="voltage">--.- °C</div><div>No sensors</div>';
      }
      const logStatus = document.getElementById('logStatus');
      if (data.logging) {
        logStatus.textContent = 'Logging to ' + (data.logFilename || '');
      } else {
        logStatus.textContent = 'Idle';
      }
      if (!measurementsInitialized) {
        setValueIfIdle('logFilename', data.logFilename || '');
        const logUseMotorEl = document.getElementById('logUseMotor');
        if (logUseMotorEl && document.activeElement !== logUseMotorEl) {
          logUseMotorEl.checked = !!data.logUseMotor;
      }
      setValueIfIdle('logDuration', (data.logDuration ?? 1).toFixed(1));
      // Wi-Fi defaults
        const wifiModeEl = document.getElementById('wifiMode');
        if (wifiModeEl) wifiModeEl.value = data.wifiApMode ? 'ap' : 'sta';
        setValueIfIdle('wifiSsid', data.wifiSsid || '');
        setValueIfIdle('wifiPassword', data.wifiPassword || '');
        const netModeEl = document.getElementById('netMode');
        if (netModeEl) netModeEl.value = data.netMode || 'wifi';
        const netPriorityEl = document.getElementById('netPriority');
        if (netPriorityEl) netPriorityEl.value = data.netPriority || 'wifi';
        const gpsModeEl = document.getElementById('gpsMode');
        if (gpsModeEl) gpsModeEl.value = data.gpsMode || 'base_time_60';
        const gpsTypes = Array.isArray(data.gpsRtcmTypes) ? data.gpsRtcmTypes.join(',') : '1004,1006,1033';
        setValueIfIdle('gpsRtcmTypes', gpsTypes);
        setValueIfIdle('deviceId', data.deviceId || '');
        setValueIfIdle('minioEndpoint', data.minioEndpoint || '');
        setValueIfIdle('minioBucket', data.minioBucket || '');
        setValueIfIdle('minioAccessKey', data.minioAccessKey || '');
        setValueIfIdle('minioSecretKey', data.minioSecretKey || '');
        const minioEnabledEl = document.getElementById('minioEnabled');
        if (minioEnabledEl && document.activeElement !== minioEnabledEl) {
          minioEnabledEl.checked = !!data.minioEnabled;
        }
        setValueIfIdle('mqttUri', data.mqttUri || '');
        setValueIfIdle('mqttUser', data.mqttUser || '');
        setValueIfIdle('mqttPassword', data.mqttPassword || '');
        const mqttEnabledEl = document.getElementById('mqttEnabled');
        if (mqttEnabledEl && document.activeElement !== mqttEnabledEl) {
          mqttEnabledEl.checked = !!data.mqttEnabled;
        }
        const storageBackendEl = document.getElementById('storageBackend');
        if (storageBackendEl && document.activeElement !== storageBackendEl) {
          storageBackendEl.value = data.storageBackend || 'sd';
        }
        measurementsInitialized = true;
      }
      const storageBackendLabelEl = document.getElementById('storageBackendLabel');
      if (storageBackendLabelEl) {
        storageBackendLabelEl.textContent = data.storageBackend === 'internal_flash' ? 'Internal flash' : 'SD card';
      }
      const sdMountedEl = document.getElementById('sdMountedLabel');
      if (sdMountedEl) sdMountedEl.textContent = data.sdMounted ? 'Yes' : 'No';
      const internalMountedEl = document.getElementById('internalFlashMountedLabel');
      if (internalMountedEl) internalMountedEl.textContent = data.internalFlashMounted ? 'Yes' : 'No';
      const activeMountedEl = document.getElementById('activeStorageMountedLabel');
      if (activeMountedEl) activeMountedEl.textContent = data.activeStorageMounted ? 'Yes' : 'No';
      const storageBackendSelectEl = document.getElementById('storageBackend');
      if (storageBackendSelectEl && document.activeElement !== storageBackendSelectEl) {
        storageBackendSelectEl.value = data.storageBackend || 'sd';
      }
      document.getElementById('stepperStatus').textContent =
        (data.stepperEnabled ? 'Enabled' : 'Disabled') + (data.stepperHomeStatus ? ` / ${data.stepperHomeStatus}` : '');
      document.getElementById('stepperPosition').textContent = data.stepperPosition;
      document.getElementById('stepperTarget').textContent = data.stepperTarget;
      document.getElementById('stepperHomeOffsetDisplay').textContent = data.stepperHomeOffsetSteps ?? 0;
      document.getElementById('motorHallRaw').textContent = data.motorHallRawLevel ?? '?';
      document.getElementById('motorHallActive').textContent = data.motorHallActiveLevel ?? 0;
      document.getElementById('motorHallTriggered').textContent = data.motorHallTriggered ? 'Yes' : 'No';
      document.getElementById('stepperMoving').textContent = data.stepperMoving ? 'Yes' : 'No';
      setValueIfIdle('speed', data.stepperSpeedUs ?? '');
      setValueIfIdle('homeOffsetSteps', data.stepperHomeOffsetSteps ?? 0);
      setValueIfIdle('loggingMotorSteps', data.loggingMotorSteps ?? 100);
      const loggingReturnModeEl = document.getElementById('loggingReturnMode');
      if (loggingReturnModeEl && document.activeElement !== loggingReturnModeEl) {
        loggingReturnModeEl.value = data.loggingHomeEachCycle === false ? 'steps' : 'home';
      }
      const hallActiveLevelEl = document.getElementById('hallActiveLevel');
      if (hallActiveLevelEl && document.activeElement !== hallActiveLevelEl) {
        hallActiveLevelEl.value = String(data.motorHallActiveLevel ?? 0);
      }
      const lastUpdateEl = document.getElementById('lastUpdate');
      if (lastUpdateEl) {
        if (data.timestampIso) {
          const parsed = new Date(data.timestampIso);
          lastUpdateEl.textContent = isNaN(parsed.getTime()) ? data.timestampIso : parsed.toLocaleString();
        } else {
          lastUpdateEl.textContent = new Date().toLocaleTimeString();
        }
      }
      const modeLabel = data.usbMode === 'msc' ? 'Mass Storage (SD over USB)' : 'Serial (logs/flash)';
      document.getElementById('usbModeLabel').textContent = modeLabel;
      const gpsActualModeEl = document.getElementById('gpsActualMode');
      if (gpsActualModeEl) gpsActualModeEl.textContent = data.gpsActualMode || '--';
      const gpsAntShortEl = document.getElementById('gpsAntennaShort');
      if (gpsAntShortEl) gpsAntShortEl.textContent = typeof data.gpsAntennaShort === 'boolean' ? (data.gpsAntennaShort ? 'YES' : 'NO') : '--';
      const gpsAntShortRawEl = document.getElementById('gpsAntennaShortRaw');
      if (gpsAntShortRawEl) gpsAntShortRawEl.textContent = data.gpsAntennaShortRaw ?? '--';
      const gpsPositionStatusEl = document.getElementById('gpsPositionStatus');
      if (gpsPositionStatusEl) {
        if (data.gpsPositionValid) {
          const age = Number.isFinite(data.gpsPositionAgeMs) ? `, age ${Math.round(data.gpsPositionAgeMs / 1000)}s` : '';
          gpsPositionStatusEl.textContent = `OK lat=${Number(data.gpsLat).toFixed(6)} lon=${Number(data.gpsLon).toFixed(6)} sats=${data.gpsSatellites ?? '--'} fix=${data.gpsFixQuality ?? '--'}${age}`;
        } else {
          gpsPositionStatusEl.textContent = 'NO FIX';
        }
      }
      const gpsTimeStatusEl = document.getElementById('gpsTimeStatus');
      if (gpsTimeStatusEl) {
        if (data.gpsTimeValid) {
          const age = Number.isFinite(data.gpsTimeAgeMs) ? `, age ${Math.round(data.gpsTimeAgeMs / 1000)}s` : '';
          gpsTimeStatusEl.textContent = `${data.gpsTimeIso || '--'}${age}`;
        } else {
          gpsTimeStatusEl.textContent = 'NO TIME';
        }
      }
      const extPowerEl = document.getElementById('externalPowerState');
      if (extPowerEl) extPowerEl.textContent = data.externalPowerOn ? 'ON' : 'OFF';
      const setMetricText = (id, value) => {
        const el = document.getElementById(id);
        if (el) el.textContent = value ?? '--';
      };
      setMetricText('heapFreeBytes', data.heapFreeBytes);
      setMetricText('heapMinFreeBytes', data.heapMinFreeBytes);
      setMetricText('heapLargestFreeBlockBytes', data.heapLargestFreeBlockBytes);
      setMetricText('heapInternalFreeBytes', data.heapInternalFreeBytes);
      setMetricText('heapInternalLargestFreeBlockBytes', data.heapInternalLargestFreeBlockBytes);
      setMetricText('heapPsramFreeBytes', data.heapPsramFreeBytes);
      setMetricText('heapPsramLargestFreeBlockBytes', data.heapPsramLargestFreeBlockBytes);
      const minioAttemptsEl = document.getElementById('minioUploadAttempts');
      if (minioAttemptsEl) minioAttemptsEl.textContent = data.minioUploadAttempts ?? '--';
      const minioLastEl = document.getElementById('minioLastAttempt');
      if (minioLastEl) {
        minioLastEl.textContent = Number.isFinite(data.minioLastAttemptMs)
          ? `${Math.round(data.minioLastAttemptMs / 1000)}s since boot`
          : '--';
      }

      const mask = Number.isFinite(data.pidSensorMask) ? data.pidSensorMask : 0;
      if (!pidEditing) {
        if (mask > 0) {
          setPidSelectionFromMask(mask, tempEntries.length);
        } else if (Number.isFinite(data.pidSensorIndex)) {
          setPidSelectionFromMask(1 << Number(data.pidSensorIndex), tempEntries.length);
        } else {
          setPidSelectionFromMask(0, tempEntries.length);
        }
      }
      renderPidChips(tempEntries);
      // PID inputs оставляем как ввёл пользователь, не трогаем автоданными
      document.getElementById('pidStatus').textContent = data.pidEnabled ? `On (out ${data.pidOutput?.toFixed(1) ?? 0}%)` : 'Off';
      setValueIfIdle('pidSetpoint', (data.pidSetpoint ?? 0).toFixed(2));
      setValueIfIdle('pidKp', (data.pidKp ?? 0).toFixed(4));
      setValueIfIdle('pidKi', (data.pidKi ?? 0).toFixed(4));
      setValueIfIdle('pidKd', (data.pidKd ?? 0).toFixed(4));
      addMonitorSample(data);
    }
    
    function setupTabs() {
      const buttons = Array.from(document.querySelectorAll('.tab-button'));
      const contents = Array.from(document.querySelectorAll('.tab-content'));
      const activate = (id) => {
        contents.forEach(c => c.classList.toggle('active', c.id === id));
        buttons.forEach(b => b.classList.toggle('active', b.dataset.tab === id));
      };
      buttons.forEach(btn => {
        btn.addEventListener('click', () => activate(btn.dataset.tab));
      });
      const initial = buttons.find(b => b.classList.contains('active'));
      if (initial) activate(initial.dataset.tab);
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

    function setSystemStatus(text) {
      const el = document.getElementById('systemActionStatus');
      if (el) el.textContent = text;
    }

    function restartDevice() {
      if (!confirm('Restart ESP32 now?')) return;
      setSystemStatus('Restart command sent...');
      fetch('/restart', { method: 'POST' })
        .then(response => {
          if (!response.ok) throw new Error('Restart failed');
          setSystemStatus('ESP32 is restarting');
        })
        .catch(error => {
          console.error('Error:', error);
          setSystemStatus('Restart request failed');
        });
    }

    function setExternalPower(enabled) {
      setSystemStatus(enabled ? 'Switching external power ON...' : 'Switching external power OFF...');
      fetch('/external_power/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled })
      })
      .then(response => {
        if (!response.ok) throw new Error('External power set failed');
        return response.json();
      })
      .then(() => {
        setSystemStatus(enabled ? 'External power is ON' : 'External power is OFF');
        refreshData();
      })
      .catch(error => {
        console.error('Error:', error);
        setSystemStatus('External power request failed');
        refreshData();
      });
    }

    function cycleExternalPower() {
      const offMs = parseInt(document.getElementById('externalPowerOffMs').value, 10) || 1000;
      if (!confirm(`Power cycle external modules? OFF for ${offMs} ms.`)) return;
      setSystemStatus('Power cycle started...');
      fetch('/external_power/cycle', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ offMs })
      })
      .then(response => {
        if (!response.ok) throw new Error('External power cycle failed');
        return response.json();
      })
      .then(() => {
        setSystemStatus('Power cycle command accepted');
        refreshData();
      })
      .catch(error => {
        console.error('Error:', error);
        setSystemStatus('Power cycle request failed');
        refreshData();
      });
    }

    function syncConfigInternalFlash() {
      setSystemStatus('Syncing config to ESP internal flash...');
      fetch('/config/sync_internal_flash', { method: 'POST' })
        .then(response => {
          if (!response.ok) throw new Error('Config sync failed');
          return response.json();
        })
        .then(() => {
          setSystemStatus('Config synced to ESP internal flash');
          loadFlash();
        })
        .catch(error => {
          console.error('Error:', error);
          setSystemStatus('Config sync to ESP flash failed');
        });
    }

    function applyStorageBackend() {
      const backend = document.getElementById('storageBackend')?.value || 'sd';
      setSystemStatus('Applying storage backend...');
      fetch('/storage/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ backend })
      })
      .then(async response => {
        const text = await response.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!response.ok) {
          throw new Error(data?.error || data?.message || text || 'Storage apply failed');
        }
        return data;
      })
      .then(data => {
        const label = data.backend === 'internal_flash' ? 'Internal flash' : 'SD card';
        setSystemStatus(data.restartRequired ? `Storage backend: ${label}; restart required to mount it` : `Storage backend: ${label}`);
        refreshData();
        loadFlash();
      })
      .catch(error => {
        console.error('Error:', error);
        setSystemStatus(error.message || 'Storage apply failed');
        refreshData();
      });
    }

    function remountStorage() {
      setSystemStatus('Remounting active storage...');
      fetch('/storage/remount', { method: 'POST' })
      .then(async response => {
        const text = await response.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!response.ok) {
          throw new Error(data?.error || data?.message || text || 'Storage remount failed');
        }
        return data;
      })
      .then(data => {
        const label = data.backend === 'internal_flash' ? 'Internal flash' : 'SD card';
        setSystemStatus(`Storage remounted: ${label}`);
        refreshData();
        loadFlash();
      })
      .catch(error => {
        console.error('Error:', error);
        setSystemStatus(error.message || 'Storage remount failed');
        refreshData();
      });
    }

    function transferUploadQueue(target) {
      const toSd = target === 'sd';
      const label = toSd ? 'из flash в SD' : 'из SD во flash';
      if (!confirm(`Перенести файлы из to_upload ${label}? Логирование должно быть остановлено.`)) {
        return Promise.resolve();
      }
      setSystemStatus(`Перенос to_upload ${label}...`);
      return fetch(toSd ? '/storage/transfer_flash_to_sd' : '/storage/transfer_sd_to_flash', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ maxFiles: 10 }),
      }).then(async response => {
        const text = await response.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!response.ok) {
          throw new Error(data?.error || data?.message || text || 'Transfer failed');
        }
        return data;
      }).then(data => {
        setSystemStatus(`Перенос завершен: moved=${data.moved || 0}, skipped=${data.skipped || 0}, failed=${data.failed || 0}`);
        alert(`Перенос: moved=${data.moved || 0}, skipped=${data.skipped || 0}, failed=${data.failed || 0}`);
        loadFiles();
        loadFlash();
        refreshData();
      }).catch(error => {
        console.error('Error:', error);
        setSystemStatus(error.message || 'Transfer failed');
        alert(error.message || 'Transfer failed');
        refreshData();
      });
    }

    function transferFlashToSd() {
      return transferUploadQueue('sd');
    }

    function transferSdToFlash() {
      return transferUploadQueue('flash');
    }
    
    function enableStepper() {
      fetch('/stepper/enable', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          refreshData();
        });
    }
    
    function disableStepper() {
      fetch('/stepper/disable', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
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
        refreshData();
      })
      .catch(error => {
        console.error('Error:', error);
        refreshData();
      });
    }
    
    function stopStepper() {
      fetch('/stepper/stop', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          refreshData();
        });
    }
    
    function setZero() {
      fetch('/stepper/zero', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          refreshData();
        })
        .catch(error => {
          console.error('Error:', error);
          refreshData();
        });
    }

    function goToOffsetZero() {
      saveStepperSettings(false)
        .then(() => fetch('/stepper/find_zero', { method: 'POST' }))
        .then(() => refreshData())
        .catch(() => refreshData());
    }

    function saveStepperSettings(showAlert = true) {
      const speedUs = parseInt(document.getElementById('speed').value, 10) || 1;
      const offsetSteps = parseInt(document.getElementById('homeOffsetSteps').value, 10) || 0;
      const loggingMotorSteps = Math.max(1, parseInt(document.getElementById('loggingMotorSteps').value, 10) || 100);
      const loggingHomeEachCycle = document.getElementById('loggingReturnMode').value !== 'steps';
      const hallActiveLevel = parseInt(document.getElementById('hallActiveLevel').value, 10) ? 1 : 0;
      return fetch('/stepper/home_offset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ speedUs, offsetSteps, loggingMotorSteps, loggingHomeEachCycle, hallActiveLevel })
      })
      .then(response => {
        if (!response.ok) throw new Error('Failed to save motor settings');
        return response.json();
      })
      .then(() => refreshData())
      .catch(error => {
        if (showAlert) {
          alert(error.message || 'Motor settings save failed');
        }
        refreshData();
        throw error;
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

    function startLog() {
      const fname = document.getElementById('logFilename').value;
      const useMotor = document.getElementById('logUseMotor').checked;
      const duration = parseFloat(document.getElementById('logDuration').value);
      fetch('/log/start', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ filename: fname, useMotor: useMotor, durationSec: duration })
      }).then(() => refreshData()).catch(() => refreshData());
    }

    function stopLog() {
      fetch('/log/stop', { method: 'POST' })
      .then(() => refreshData())
      .catch(() => refreshData());
    }

    function applyWifi() {
      const mode = document.getElementById('wifiMode').value;
      const ssid = document.getElementById('wifiSsid').value;
      const password = document.getElementById('wifiPassword').value;
      fetch('/wifi/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode, ssid, password })
      }).then(res => {
        if (!res.ok) throw new Error('Failed to apply Wi-Fi');
        return res.json();
      }).then(() => {
        alert('Wi‑Fi settings applied. Device may reconnect or switch mode.');
      }).catch(err => {
        alert('Wi‑Fi apply failed: ' + err.message);
      });
    }

    function applyNetwork() {
      const mode = document.getElementById('netMode').value;
      const priority = document.getElementById('netPriority').value;
      fetch('/net/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode, priority })
      }).then(res => {
        if (!res.ok) throw new Error('Failed to apply network');
        return res.json();
      }).then(() => {
        alert('Network settings applied. Interface may switch.');
      }).catch(err => {
        alert('Network apply failed: ' + err.message);
      });
    }

    function applyCloudConfig() {
      const payload = {
        deviceId: document.getElementById('deviceId').value,
        minioEnabled: document.getElementById('minioEnabled').checked,
        minioEndpoint: document.getElementById('minioEndpoint').value,
        minioBucket: document.getElementById('minioBucket').value,
        minioAccessKey: document.getElementById('minioAccessKey').value,
        minioSecretKey: document.getElementById('minioSecretKey').value,
        mqttUri: document.getElementById('mqttUri').value,
        mqttEnabled: document.getElementById('mqttEnabled').checked,
        mqttUser: document.getElementById('mqttUser').value,
        mqttPassword: document.getElementById('mqttPassword').value,
      };
      fetch('/cloud/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
      }).then(res => {
        if (!res.ok) throw new Error('Не удалось сохранить настройки');
        return res.json();
      }).then(() => {
        alert('Облачные настройки сохранены');
      }).catch(err => alert(err.message || 'Ошибка сохранения'));
    }

    function parseGpsRtcmTypes(text) {
      return String(text || '')
        .split(/[,\s;]+/)
        .map(v => parseInt(v, 10))
        .filter((v, idx, arr) => Number.isFinite(v) && v > 0 && v <= 4095 && arr.indexOf(v) === idx);
    }

    function applyGpsConfig() {
      const status = document.getElementById('gpsConfigStatus');
      const rtcmTypes = parseGpsRtcmTypes(document.getElementById('gpsRtcmTypes').value);
      const mode = document.getElementById('gpsMode').value;
      if (!rtcmTypes.length) {
        alert('Укажи хотя бы один RTCM код');
        return;
      }
      if (status) status.textContent = 'Saving GPS settings...';
      fetch('/gps/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode, rtcmTypes }),
      }).then(res => {
        if (!res.ok) throw new Error('Не удалось сохранить GPS настройки');
        return res.json();
      }).then(() => {
        if (status) status.textContent = 'GPS settings saved. Reconfigure queued.';
        refreshData();
      }).catch(err => {
        if (status) status.textContent = err.message || 'GPS settings save failed';
        alert(err.message || 'Ошибка сохранения GPS настроек');
      });
    }

    function probeGpsMode() {
      fetch('/gps/probe', { method: 'POST' })
        .then(() => setTimeout(refreshData, 500))
        .catch(() => refreshData());
    }

    function applyPid() {
      const selected = Array.from(pidSelection).filter(v => Number.isFinite(v)).sort((a, b) => a - b);
      if (pidEntries.length > 0 && selected.length === 0) {
        selected.push(0);
        pidSelection.add(0);
      }
      const payload = {
        setpoint: parseFloat(document.getElementById('pidSetpoint').value),
        sensors: selected,
        kp: parseFloat(document.getElementById('pidKp').value),
        ki: parseFloat(document.getElementById('pidKi').value),
        kd: parseFloat(document.getElementById('pidKd').value),
      };
      pidEditing = false;
      fetch('/pid/apply', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
      }).then(() => refreshData()).catch(() => refreshData());
    }

    function enablePid() {
      fetch('/pid/enable', { method: 'POST' })
        .then(() => refreshData())
        .catch(() => refreshData());
    }

    function disablePid() {
      fetch('/pid/disable', { method: 'POST' })
        .then(() => refreshData())
        .catch(() => refreshData());
    }

    function getFileName(item) {
      return getBaseName(getFilePath(item));
    }

    function refreshSelectionState() {
      const available = new Set(cachedFiles.filter(item => (item?.type ?? 'file') === 'file').map(getFilePath).filter(Boolean));
      Array.from(selectedFiles).forEach(path => {
        if (!available.has(path)) {
          selectedFiles.delete(path);
        }
      });
    }

    function updateSelectionControls() {
      const selectAll = document.getElementById('selectAllFiles');
      const deleteBtn = document.getElementById('deleteSelectedBtn');
      const selectableCount = cachedFiles.filter(item => {
        const name = getFileName(item);
        return name && name !== 'config.txt' && (item?.type ?? 'file') === 'file';
      }).length;
      const selectedCount = selectedFiles.size;
      if (selectAll) {
        selectAll.checked = selectableCount > 0 && selectedCount === selectableCount;
        selectAll.indeterminate = selectedCount > 0 && selectedCount < selectableCount;
      }
      if (deleteBtn) {
        deleteBtn.disabled = selectedCount === 0;
      }
    }

    function toggleFileSelection(path, checked) {
      const base = getBaseName(path);
      if (!path || base === 'config.txt') return;
      if (checked) {
        selectedFiles.add(path);
      } else {
        selectedFiles.delete(path);
      }
      updateSelectionControls();
    }

    function toggleSelectAll(checked) {
      cachedFiles.forEach(item => {
        const path = getFilePath(item);
        const base = getBaseName(path);
        if (!path || base === 'config.txt' || (item?.type ?? 'file') === 'dir') return;
        if (checked) {
          selectedFiles.add(path);
        } else {
          selectedFiles.delete(path);
        }
      });
      renderFiles({ entries: cachedFiles, path: filesState.path, page: filesState.page, pageSize: filesState.pageSize, total: filesState.total, totalPages: filesState.totalPages });
    }

    function sendDeleteRequest(files) {
      const unique = Array.from(new Set(files.filter(name => {
        const base = getBaseName(name);
        return name && base !== 'config.txt';
      })));
      if (unique.length === 0) {
        alert('Нет выбранных файлов для удаления');
        return Promise.resolve();
      }
      if (!confirm(`Удалить ${unique.length} файл(ов)?`)) {
        return Promise.resolve();
      }
      return fetch('/fs/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ files: unique }),
      }).then(async res => {
        const text = await res.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!res.ok) {
          throw new Error(data?.error || data?.message || text || 'Не удалось удалить файлы');
        }
        return data;
      }).then(result => {
        selectedFiles.clear();
        loadFiles();
        const deleted = Array.isArray(result.deleted) ? result.deleted : [];
        const skipped = Array.isArray(result.skipped) ? result.skipped : [];
        const failed = Array.isArray(result.failed) ? result.failed : [];
        if (deleted.length === 0 && skipped.length === 0 && failed.length === 0) {
          return;
        }
        let msg = '';
        if (deleted.length) msg += `Удалены: ${deleted.join(', ')}. `;
        if (skipped.length) msg += `Пропущены: ${skipped.join(', ')}. `;
        if (failed.length) msg += `Ошибки: ${failed.join(', ')}.`;
        if (msg.trim()) {
          alert(msg.trim());
        }
      }).catch(err => {
        alert(err.message || 'Не удалось удалить файлы');
      });
    }

    function deleteSelectedFiles() {
      return sendDeleteRequest(Array.from(selectedFiles));
    }

    function deleteSingleFile(name) {
      return sendDeleteRequest([name]);
    }

    function clearUploadedFiles() {
      if (!confirm('Удалить все файлы из /uploaded? Файлы из /to_upload не будут тронуты.')) {
        return Promise.resolve();
      }
      return fetch('/fs/clear_uploaded', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ maxFiles: 1000 }),
      }).then(async res => {
        const text = await res.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!res.ok) {
          throw new Error(data?.error || data?.message || text || 'Не удалось очистить uploaded');
        }
        const status = data.status === 'already_running' ? 'очистка уже идет' : 'очистка запущена';
        alert(`uploaded: ${status}, лимит ${data.maxFiles || 1000} файлов`);
        setTimeout(loadFiles, 1500);
        refreshData();
      }).catch(err => {
        alert(err.message || 'Не удалось очистить uploaded');
      });
    }

    function refreshFlashSelectionState() {
      const available = new Set(cachedFlashFiles.filter(item => {
        const path = getFilePath(item);
        const name = getBaseName(path);
        return path && name !== 'config.txt' && (item?.type ?? 'file') === 'file';
      }).map(getFilePath));
      Array.from(selectedFlashFiles).forEach(path => {
        if (!available.has(path)) {
          selectedFlashFiles.delete(path);
        }
      });
    }

    function updateFlashSelectionControls() {
      const selectAll = document.getElementById('selectAllFlashFiles');
      const deleteBtn = document.getElementById('deleteSelectedFlashBtn');
      const selectableCount = cachedFlashFiles.filter(item => {
        const path = getFilePath(item);
        const name = getBaseName(path);
        return path && name !== 'config.txt' && (item?.type ?? 'file') === 'file';
      }).length;
      const selectedCount = selectedFlashFiles.size;
      if (selectAll) {
        selectAll.checked = selectableCount > 0 && selectedCount === selectableCount;
        selectAll.indeterminate = selectedCount > 0 && selectedCount < selectableCount;
      }
      if (deleteBtn) {
        deleteBtn.disabled = selectedCount === 0;
      }
    }

    function toggleFlashSelection(path, checked) {
      const base = getBaseName(path);
      if (!path || base === 'config.txt') return;
      if (checked) {
        selectedFlashFiles.add(path);
      } else {
        selectedFlashFiles.delete(path);
      }
      updateFlashSelectionControls();
    }

    function toggleSelectAllFlash(checked) {
      cachedFlashFiles.forEach(item => {
        const path = getFilePath(item);
        const base = getBaseName(path);
        if (!path || base === 'config.txt' || (item?.type ?? 'file') !== 'file') return;
        if (checked) {
          selectedFlashFiles.add(path);
        } else {
          selectedFlashFiles.delete(path);
        }
      });
      document.querySelectorAll('#flashList .file-checkbox:not(:disabled)').forEach(cb => { cb.checked = checked; });
      updateFlashSelectionControls();
    }

    function sendFlashDeleteRequest(files) {
      const unique = Array.from(new Set(files.filter(name => {
        const base = getBaseName(name);
        return name && base !== 'config.txt';
      })));
      if (unique.length === 0) {
        alert('Нет выбранных файлов flash для удаления');
        return Promise.resolve();
      }
      if (!confirm(`Удалить ${unique.length} файл(ов) из внутренней flash?`)) {
        return Promise.resolve();
      }
      return fetch('/flash/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ files: unique }),
      }).then(async res => {
        const text = await res.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!res.ok) {
          throw new Error(data?.error || data?.message || text || 'Не удалось удалить файлы flash');
        }
        return data;
      }).then(result => {
        selectedFlashFiles.clear();
        loadFlash(flashState.path, flashState.page);
        const deleted = Array.isArray(result.deleted) ? result.deleted : [];
        const skipped = Array.isArray(result.skipped) ? result.skipped : [];
        const failed = Array.isArray(result.failed) ? result.failed : [];
        let msg = '';
        if (deleted.length) msg += `Удалены: ${deleted.join(', ')}. `;
        if (skipped.length) msg += `Пропущены: ${skipped.join(', ')}. `;
        if (failed.length) msg += `Ошибки: ${failed.join(', ')}.`;
        if (msg.trim()) alert(msg.trim());
      }).catch(err => {
        alert(err.message || 'Не удалось удалить файлы flash');
      });
    }

    function deleteSelectedFlashFiles() {
      return sendFlashDeleteRequest(Array.from(selectedFlashFiles));
    }

    function deleteSingleFlashFile(path) {
      return sendFlashDeleteRequest([path]);
    }

    function clearFlashUploadedFiles() {
      if (!confirm('Удалить все файлы из /flashfs/uploaded? Файлы из /flashfs/to_upload не будут тронуты.')) {
        return Promise.resolve();
      }
      return fetch('/flash/clear_uploaded', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ maxFiles: 1000 }),
      }).then(async res => {
        const text = await res.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { message: text }; }
        if (!res.ok) {
          throw new Error(data?.error || data?.message || text || 'Не удалось очистить flash uploaded');
        }
        alert(`flash uploaded: удалено ${data.deleted || 0}, ошибок ${data.failed || 0}, просмотрено ${data.scanned || 0}`);
        setTimeout(() => loadFlash(flashState.path, flashState.page), 1500);
        refreshData();
      }).catch(err => {
        alert(err.message || 'Не удалось очистить flash uploaded');
      });
    }

    function formatBytes(bytes) {
      if (!Number.isFinite(bytes)) return '--';
      if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
      if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
      return `${bytes} B`;
    }

    function updateFlashNav() {
      const pathLabel = document.getElementById('flashPathLabel');
      const pageInfo = document.getElementById('flashPageInfo');
      const cleanPath = flashState.path || '/';
      if (pathLabel) pathLabel.textContent = cleanPath;
      if (pageInfo) pageInfo.textContent = `${flashState.page + 1}/${Math.max(flashState.totalPages || 1, 1)}`;
    }

    function renderFlash(data) {
      const listEl = document.getElementById('flashList');
      const totalEl = document.getElementById('flashTotalBytes');
      const partitionEl = document.getElementById('flashPartitionBytes');
      const internalFsEl = document.getElementById('flashInternalFsBytes');
      const nvsEl = document.getElementById('flashNvsEntries');
      if (totalEl) totalEl.textContent = formatBytes(data?.flashTotalBytes);
      if (partitionEl) partitionEl.textContent = formatBytes(data?.partitionBytes);
      if (internalFsEl) {
        if (data?.internalFsMounted && Number.isFinite(data?.internalFsTotalBytes)) {
          internalFsEl.textContent = `${formatBytes(data.internalFsUsedBytes || 0)} / ${formatBytes(data.internalFsTotalBytes)}`;
        } else {
          internalFsEl.textContent = 'not mounted';
        }
      }
      if (nvsEl) {
        const nvs = data?.nvs || {};
        if (Number.isFinite(nvs.usedEntries) && Number.isFinite(nvs.totalEntries)) {
          nvsEl.textContent = `${nvs.usedEntries}/${nvs.totalEntries} used, ${nvs.freeEntries ?? '--'} free`;
        } else {
          nvsEl.textContent = '--';
        }
      }
      const entries = Array.isArray(data?.entries) ? data.entries : [];
      flashState.path = data?.path || '';
      flashState.page = Number.isFinite(data?.page) ? data.page : 0;
      flashState.pageSize = Number.isFinite(data?.pageSize) ? data.pageSize : 10;
      flashState.total = Number.isFinite(data?.total) ? data.total : entries.length;
      flashState.totalPages = Number.isFinite(data?.totalPages) ? data.totalPages : 1;
      cachedFlashFiles = entries;
      refreshFlashSelectionState();
      updateFlashNav();
      if (!listEl) return;
      if (!entries.length) {
        listEl.innerHTML = '<div>Нет данных по flash</div>';
        updateFlashSelectionControls();
        return;
      }
      listEl.innerHTML = '';
      entries.forEach(item => {
        const path = getFilePath(item);
        const base = getBaseName(path);
        const isDir = item.type === 'dir';
        const isPartition = item.type === 'partition';
        const isDeletable = item.type === 'file' && base !== 'config.txt';
        const row = document.createElement('div');
        row.className = 'file-row';

        const info = document.createElement('div');
        info.className = 'file-info';

        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.className = 'file-checkbox';
        checkbox.disabled = !isDeletable;
        checkbox.checked = selectedFlashFiles.has(path);
        checkbox.onchange = () => toggleFlashSelection(path, checkbox.checked);
        info.appendChild(checkbox);

        const name = document.createElement('span');
        name.className = 'file-name';
        if (isPartition) {
          const offset = Number.isFinite(item.offset) ? ` @0x${Number(item.offset).toString(16)}` : '';
          name.textContent = `🧩 ${item.name} [${item.partType || '?'}:${item.subtype || '?'}]${offset} (${formatBytes(item.size)})`;
        } else if (isDir) {
          name.textContent = `📁 ${item.name} — ${item.area || 'internal FAT'}`;
        } else {
          name.textContent = `📄 ${item.name} (${formatBytes(item.size)}) — ${item.area || 'NVS'}`;
        }
        info.appendChild(name);

        const actions = document.createElement('div');
        if (isDir) {
          const openBtn = document.createElement('button');
          openBtn.className = 'btn btn-small';
          openBtn.textContent = 'Открыть';
          openBtn.onclick = () => loadFlash(path, 0);
          actions.appendChild(openBtn);
        } else if (item.downloadable) {
          const dlBtn = document.createElement('button');
          dlBtn.className = 'btn btn-small';
          dlBtn.textContent = 'Скачать';
          dlBtn.onclick = () => { window.open('/flash/download?path=' + encodeURIComponent(path), '_blank'); };
          actions.appendChild(dlBtn);
          const delBtn = document.createElement('button');
          delBtn.className = 'btn btn-small btn-stop';
          delBtn.textContent = 'Удалить';
          delBtn.disabled = !isDeletable;
          delBtn.onclick = () => deleteSingleFlashFile(path);
          actions.appendChild(delBtn);
        }

        row.appendChild(info);
        row.appendChild(actions);
        listEl.appendChild(row);
      });
      updateFlashSelectionControls();
    }

    function loadFlash(path = flashState.path, page = flashState.page) {
      const listEl = document.getElementById('flashList');
      if (listEl) listEl.innerHTML = 'Загружаю...';
      if (typeof path === 'string') flashState.path = path;
      if (Number.isFinite(page)) flashState.page = Math.max(0, page);
      const params = new URLSearchParams();
      if (flashState.path) params.append('path', flashState.path);
      params.append('page', flashState.page);
      params.append('pageSize', flashState.pageSize);
      fetch('/flash/list?' + params.toString())
        .then(res => res.json())
        .then(data => renderFlash(data))
        .catch(() => {
          cachedFlashFiles = [];
          selectedFlashFiles.clear();
          if (listEl) listEl.innerHTML = 'Ошибка загрузки flash';
          updateFlashSelectionControls();
        });
    }

    function flashGoUp() {
      const path = flashState.path || '';
      const idx = path.lastIndexOf('/');
      flashState.path = idx > 0 ? path.slice(0, idx) : '';
      flashState.page = 0;
      selectedFlashFiles.clear();
      loadFlash();
    }

    function flashPagePrev() {
      if (flashState.page > 0) {
        flashState.page -= 1;
        loadFlash();
      }
    }

    function flashPageNext() {
      if (flashState.page + 1 < flashState.totalPages) {
        flashState.page += 1;
        loadFlash();
      }
    }

    function updateFileNav() {
      const pathLabel = document.getElementById('filePathLabel');
      const pageInfo = document.getElementById('filePageInfo');
      const cleanPath = filesState.path || '/';
      if (pathLabel) pathLabel.textContent = cleanPath;
      if (pageInfo) pageInfo.textContent = `${filesState.page + 1}/${Math.max(filesState.totalPages || 1, 1)}`;
    }

    function renderFiles(data) {
      const listEl = document.getElementById('fileList');
      let entries = [];
      if (Array.isArray(data)) {
        entries = data;
        filesState.total = entries.length;
        filesState.totalPages = 1;
        filesState.page = 0;
      } else if (data && typeof data === 'object') {
        entries = Array.isArray(data.entries) ? data.entries : [];
        filesState.path = data.path || '';
        filesState.page = Number.isFinite(data.page) ? data.page : 0;
        filesState.pageSize = Number.isFinite(data.pageSize) ? data.pageSize : 10;
        filesState.total = Number.isFinite(data.total) ? data.total : entries.length;
        filesState.totalPages = Number.isFinite(data.totalPages) ? data.totalPages : 1;
      }
      cachedFiles = entries;
      refreshSelectionState();
      updateFileNav();
      if (!listEl) return;
      if (!Array.isArray(entries) || entries.length === 0) {
        listEl.innerHTML = '<div>Нет файлов</div>';
        updateSelectionControls();
        return;
      }
      listEl.innerHTML = '';
      entries.forEach(item => {
        const path = getFilePath(item);
        const name = getBaseName(path);
        const sizeBytes = (item && typeof item === 'object' && Number.isFinite(item.size)) ? item.size : null;
        const isDir = (item && item.type === 'dir');
        if (!name) return;
        const row = document.createElement('div');
        row.className = 'file-row';

        const info = document.createElement('div');
        info.className = 'file-info';

        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.className = 'file-checkbox';
        checkbox.disabled = name === 'config.txt' || isDir;
        checkbox.checked = selectedFiles.has(path);
        checkbox.onchange = () => {
          toggleFileSelection(path, checkbox.checked);
          updateSelectionControls();
        };

        const left = document.createElement('span');
        left.className = 'file-name';
        let sizeText = '';
        if (sizeBytes !== null) {
          sizeText = ` (${(sizeBytes / (1024 * 1024)).toFixed(2)} MB)`;
        }
        left.textContent = (isDir ? '📁 ' : '') + name + sizeText;

        info.appendChild(checkbox);
        info.appendChild(left);

        const actions = document.createElement('div');

        if (isDir) {
          const openBtn = document.createElement('button');
          openBtn.className = 'btn btn-small';
          openBtn.textContent = 'Открыть';
          openBtn.onclick = () => openFolder(path);
          actions.appendChild(openBtn);
        } else {
          const dlBtn = document.createElement('button');
          dlBtn.className = 'btn btn-small';
          dlBtn.textContent = 'Скачать';
          dlBtn.onclick = () => { window.open('/fs/download?path=' + encodeURIComponent(path), '_blank'); };

          const delBtn = document.createElement('button');
          delBtn.className = 'btn btn-small btn-stop';
          delBtn.textContent = 'Удалить';
          delBtn.disabled = name === 'config.txt';
          delBtn.onclick = () => deleteSingleFile(path);

          actions.appendChild(dlBtn);
          actions.appendChild(delBtn);
        }
        row.appendChild(info);
        row.appendChild(actions);
        listEl.appendChild(row);
      });
      updateSelectionControls();
    }

    function openFolder(path) {
      filesState.path = path || '';
      filesState.page = 0;
      loadFiles();
    }

    function goUp() {
      const path = filesState.path || '';
      const idx = path.lastIndexOf('/');
      filesState.path = idx > 0 ? path.slice(0, idx) : '';
      filesState.page = 0;
      loadFiles();
    }

    function pagePrev() {
      if (filesState.page > 0) {
        filesState.page -= 1;
        loadFiles();
      }
    }

    function pageNext() {
      if (filesState.page + 1 < filesState.totalPages) {
        filesState.page += 1;
        loadFiles();
      }
    }

    function loadFiles() {
      const listEl = document.getElementById('fileList');
      if (listEl) listEl.innerHTML = 'Загружаю...';
      const params = new URLSearchParams();
      if (filesState.path) params.append('path', filesState.path);
      params.append('page', filesState.page);
      params.append('pageSize', filesState.pageSize);
      const url = '/fs/list' + (params.toString() ? ('?' + params.toString()) : '');
      fetch(url)
        .then(res => res.json())
        .then(files => renderFiles(files))
        .catch(() => {
          cachedFiles = [];
          selectedFiles.clear();
          if (listEl) listEl.innerHTML = 'Ошибка загрузки списка';
          updateSelectionControls();
        });
    }

    const selectAllEl = document.getElementById('selectAllFiles');
    if (selectAllEl) {
      selectAllEl.addEventListener('change', (e) => toggleSelectAll(e.target.checked));
    }
    const deleteSelectedBtn = document.getElementById('deleteSelectedBtn');
    if (deleteSelectedBtn) {
      deleteSelectedBtn.addEventListener('click', deleteSelectedFiles);
    }
    const selectAllFlashEl = document.getElementById('selectAllFlashFiles');
    if (selectAllFlashEl) {
      selectAllFlashEl.addEventListener('change', (e) => toggleSelectAllFlash(e.target.checked));
    }
    const deleteSelectedFlashBtn = document.getElementById('deleteSelectedFlashBtn');
    if (deleteSelectedFlashBtn) {
      deleteSelectedFlashBtn.addEventListener('click', deleteSelectedFlashFiles);
    }
    const startMonitorBtn = document.getElementById('monitorStartBtn');
    const stopMonitorBtn = document.getElementById('monitorStopBtn');
    if (startMonitorBtn) startMonitorBtn.addEventListener('click', startMonitor);
    if (stopMonitorBtn) stopMonitorBtn.addEventListener('click', stopMonitor);
    setupTabs();

    // Auto-refresh every 2 seconds
    setInterval(refreshData, 2000);
    
    // Initial load
    refreshData();
    loadFiles();
    loadFlash();
  </script>
</body>
</html>
)rawliteral";
