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
        <button class="tab-button" data-tab="tab-files">Files</button>
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
              <label for="pidSensor">Sensor</label>
              <select id="pidSensor">
                <option value="0">Sensor 1</option>
                <option value="1">Sensor 2</option>
                <option value="2">Sensor 3</option>
              </select>
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
              <input type="number" id="speed" value="1000" step="1">
              <div class="speed-info">Lower value = faster speed. Значение берётся из веб-инпута без ограничений и сохраняется в config.txt.</div>
            </div>
            
            <button class="btn btn-stepper" onclick="enableStepper()">Enable Motor</button>
            <button class="btn btn-stop" onclick="disableStepper()">Disable Motor</button>
            <button class="btn" onclick="moveStepper()">Move</button>
            <button class="btn btn-stop" onclick="stopStepper()">Stop</button>
            <button class="btn" onclick="setZero()">Set Position to Zero</button>
            <button class="btn" onclick="findZero()">Find Zero (Hall)</button>
          </div>
        </div>
      </div>

      <div id="tab-wifi" class="tab-content">
        <div class="controls">
          <div class="control-panel">
            <h3>Wi‑Fi</h3>
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
        </div>
      </div>

      <div id="tab-files" class="tab-content">
        <div class="files-panel">
          <h3>Файлы на SD</h3>
          <div class="form-group file-actions">
            <label class="checkbox-label"><input type="checkbox" id="selectAllFiles"> Выбрать все</label>
            <div class="file-buttons">
              <button class="btn" onclick="loadFiles()">Обновить список</button>
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
            <input type="text" id="deviceId" placeholder="dev1">
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
    const selectedFiles = new Set();
    let cachedFiles = [];
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

      const temps = Array.isArray(data.tempSensors) ? data.tempSensors : [];
      temps.forEach((val, idx) => {
        if (!monitorState.temps[idx]) monitorState.temps[idx] = [];
        if (monitorState.visible.temps[idx] === undefined) monitorState.visible.temps[idx] = true;
        monitorState.temps[idx].push({t: now, v: val});
        pruneSeries(monitorState.temps[idx], now);
      });
      // Prune old sensor slots if window expired
      Object.keys(monitorState.temps).forEach(key => {
        const arr = monitorState.temps[key];
        pruneSeries(arr, now);
        if (arr.length === 0 && temps.length < Number(key)) {
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
          series.push({key, label: `T${Number(key) + 1}`, color: ['#8e44ad', '#16a085', '#c0392b', '#34495e', '#2c3e50'][idx % 5], data: arr});
        }
      });
      drawChart('chartTemp', series);
      const legendItems = Object.keys(monitorState.temps).map((key, idx) => ({
        key,
        label: `T${Number(key) + 1}`,
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
      document.getElementById('fanPowerDisplay').textContent = data.fanPower.toFixed(0) + ' %';
      document.getElementById('fan1RpmDisplay').textContent = data.fan1Rpm;
      document.getElementById('fan2RpmDisplay').textContent = data.fan2Rpm;
      setValueIfIdle('heaterPower', data.heaterPower?.toFixed(1) ?? data.heaterPower ?? 0);
    const list = document.getElementById('tempList');
    const labels = Array.isArray(data.tempLabels) ? data.tempLabels : [];
    const addresses = Array.isArray(data.tempAddresses) ? data.tempAddresses : [];
      if (Array.isArray(data.tempSensors) && data.tempSensors.length > 0) {
        let html = '';
        data.tempSensors.forEach((t, idx) => {
          const name = labels[idx] || `t${idx + 1}`;
          const addr = addresses[idx] || '';
          const title = addr ? ` title="1-Wire ${addr}"` : '';
          const labelHtml = `<span class="temp-label"${title}>${name}</span>`;
          const text = Number.isFinite(t) ? `${t.toFixed(2)} °C` : '--.- °C';
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
        measurementsInitialized = true;
      }
      document.getElementById('stepperStatus').textContent = data.stepperEnabled ? 'Enabled' : 'Disabled';
      document.getElementById('stepperPosition').textContent = data.stepperPosition;
      document.getElementById('stepperTarget').textContent = data.stepperTarget;
      document.getElementById('stepperMoving').textContent = data.stepperMoving ? 'Yes' : 'No';
      setValueIfIdle('speed', data.stepperSpeedUs ?? '');
      document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
      const modeLabel = data.usbMode === 'msc' ? 'Mass Storage (SD over USB)' : 'Serial (logs/flash)';
      document.getElementById('usbModeLabel').textContent = modeLabel;

      const sensorSelect = document.getElementById('pidSensor');
      const currentSensor = data.pidSensorIndex ?? 0;
      const count = data.tempSensorCount || (data.tempSensors ? data.tempSensors.length : 0);
      sensorSelect.innerHTML = '';
      for (let i = 0; i < count; i++) {
        const opt = document.createElement('option');
        opt.value = i;
        const label = labels[i] || `t${i + 1}`;
        const addr = addresses[i] || '';
        opt.textContent = addr ? `${label} (${addr})` : label;
        if (addr) opt.title = `1-Wire ${addr}`;
        if (i === currentSensor) opt.selected = true;
        sensorSelect.appendChild(opt);
      }
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

    function findZero() {
      fetch('/stepper/find_zero', { method: 'POST' })
        .then(() => refreshData())
        .catch(() => refreshData());
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

    function applyPid() {
      const payload = {
        setpoint: parseFloat(document.getElementById('pidSetpoint').value),
        sensor: parseInt(document.getElementById('pidSensor').value),
        kp: parseFloat(document.getElementById('pidKp').value),
        ki: parseFloat(document.getElementById('pidKi').value),
        kd: parseFloat(document.getElementById('pidKd').value),
      };
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
  </script>
</body>
</html>
)rawliteral";
