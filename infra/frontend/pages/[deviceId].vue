<template>
  <div class="page device-page" v-if="deviceId">
    <div class="header">
      <button class="btn ghost back" @click="navigateTo('/')">← Назад</button>
      <div>
        <div class="title">Устройство {{ deviceTitle }}</div>
        <div class="status-row">
          <span class="chip" :class="{ online: device?.online }">{{ device?.online ? 'Online' : 'Offline' }}</span>
          <span class="chip subtle" v-if="device?.lastSeen">Обновлено: {{ new Date(device.lastSeen).toLocaleTimeString() }}</span>
          <button class="btn primary sm" @click="refreshState">Обновить состояние</button>
        </div>
      </div>
    </div>

    <div class="tabs">
      <button class="tab-btn" :class="{ active: activeTab === 'data' }" @click="setActiveTab('data')">Данные</button>
      <button class="tab-btn" :class="{ active: activeTab === 'control' }" @click="setActiveTab('control')">Мониторинг и управление</button>
      <button class="tab-btn" :class="{ active: activeTab === 'gps' }" @click="setActiveTab('gps')">GPS</button>
      <button class="tab-btn" :class="{ active: activeTab === 'calibration' }" @click="setActiveTab('calibration')">Калибровка</button>
      <button class="tab-btn" :class="{ active: activeTab === 'settings' }" @click="setActiveTab('settings')">Настройки</button>
      <button class="tab-btn" :class="{ active: activeTab === 'errors' }" @click="setActiveTab('errors')">Ошибки</button>
    </div>

    <div class="card metrics" v-show="activeTab === 'control'">
      <div class="metrics-top">
        <h3>Показания</h3>
        <div class="readings-grid large">
          <div class="reading-card primary">
            <div class="reading-label">{{ adcLabel('adc1', 'U1') }}</div>
            <div class="reading-value big">{{ device?.state?.voltage1?.toFixed?.(6) ?? '—' }}</div>
            <div class="reading-sub">В</div>
          </div>
          <div class="reading-card primary">
            <div class="reading-label">{{ adcLabel('adc2', 'U2') }}</div>
            <div class="reading-value big">{{ device?.state?.voltage2?.toFixed?.(6) ?? '—' }}</div>
            <div class="reading-sub">В</div>
          </div>
          <div class="reading-card primary">
            <div class="reading-label">{{ adcLabel('adc3', 'U3') }}</div>
            <div class="reading-value big">{{ device?.state?.voltage3?.toFixed?.(6) ?? '—' }}</div>
            <div class="reading-sub">В</div>
          </div>
        </div>
      </div>
      <div class="temps">
        <div class="temp-card subtle">
          <div class="temp-title">Wi‑Fi RSSI</div>
          <div class="temp-value small">{{ device?.state?.wifiRssi ?? '--' }} dBm</div>
        </div>
        <div class="temp-card subtle">
          <div class="temp-title">Wi‑Fi качество</div>
          <div class="temp-value small">{{ device?.state?.wifiQuality ?? '--' }}%</div>
        </div>
        <div v-for="sensor in tempEntries" :key="sensor.key" class="temp-card">
          <div class="temp-title">{{ sensor.label }}</div>
          <div class="temp-value small">{{ sensor.value?.toFixed?.(2) ?? '--' }} °C</div>
        </div>
        <div class="temp-card">
          <div class="temp-title">I</div>
          <div class="temp-value small">{{ device?.state?.inaCurrent?.toFixed?.(3) ?? '—' }} A</div>
        </div>
        <div class="temp-card">
          <div class="temp-title">P</div>
          <div class="temp-value small">{{ device?.state?.inaPower?.toFixed?.(3) ?? '—' }} W</div>
        </div>
        <div class="temp-card warm">
          <div class="temp-title">Нагрев</div>
          <div class="temp-value small">{{ device?.state?.heaterPower?.toFixed?.(1) ?? '—' }} %</div>
        </div>
        <div class="temp-card cool">
          <div class="temp-title">Вентилятор</div>
          <div class="temp-value small">{{ device?.state?.fanPower?.toFixed?.(1) ?? '—' }} %</div>
        </div>
        <div class="temp-card subtle">
          <div class="temp-title">FAN1</div>
          <div class="temp-value small">{{ device?.state?.fan1Rpm ?? '—' }} rpm</div>
        </div>
        <div class="temp-card subtle">
          <div class="temp-title">FAN2</div>
          <div class="temp-value small">{{ device?.state?.fan2Rpm ?? '—' }} rpm</div>
        </div>
      </div>
    </div>

    <div class="grid" v-show="activeTab === 'control'">
      <div class="card">
        <div class="card-head">
          <h3>Логи</h3>
          <span class="badge">Файлы</span>
        </div>
        <div class="form-group">
          <label>Имя файла</label>
          <input v-model="log.filename" />
        </div>
        <div class="inline fields">
          <label class="checkbox"><input type="checkbox" v-model="log.useMotor" /> <span>Использовать мотор</span></label>
          <label class="compact">Длительность (с)
            <input type="number" v-model.number="log.durationSec" min="0.1" step="0.1" />
          </label>
        </div>
        <div class="actions">
          <button class="btn primary" @click="startLog">Старт</button>
          <button class="btn warning ghost" @click="stopLog">Стоп</button>
        </div>
        <p class="muted" v-if="logStatus">{{ logStatus }}</p>
        <p class="muted">Текущий файл: {{ device?.state?.logFilename || '—' }}</p>
        <div class="log-stats">
          <div class="log-stat">
            <div class="log-stat-label">SD занято</div>
            <div class="log-stat-value">{{ sdUsageLabel }}</div>
          </div>
          <div class="log-stat">
            <div class="log-stat-label">data_ в корне</div>
            <div class="log-stat-value">{{ sdRootFiles }}</div>
          </div>
          <div class="log-stat">
            <div class="log-stat-label">to_upload</div>
            <div class="log-stat-value">{{ sdToUploadFiles }}</div>
          </div>
          <div class="log-stat">
            <div class="log-stat-label">uploaded</div>
            <div class="log-stat-value">{{ sdUploadedFiles }}</div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Система</h3>
          <span class="badge">Управление</span>
        </div>
        <p class="muted">Полная перезагрузка устройства (использовать при зависаниях).</p>
        <div class="status-row">
          <span class="chip" :class="{ online: externalPowerOn, subtle: !externalPowerOn }">EXT_PWR_ON: {{ externalPowerOn ? 'ON' : 'OFF' }}</span>
          <span class="chip subtle">Heap: {{ heapFreeLabel }}</span>
          <span class="chip subtle">Largest: {{ heapLargestLabel }}</span>
          <span class="chip subtle">MinIO tries: {{ minioUploadAttempts }}</span>
          <span class="chip subtle">Last MinIO: {{ minioLastAttemptLabel }}</span>
        </div>
        <div class="form-group">
          <label>Пауза перед включением EXT_PWR_ON, мс</label>
          <input type="number" min="100" max="30000" step="100" v-model.number="externalPowerOffMs" />
        </div>
        <div class="actions">
          <button class="btn success" @click="externalPowerSet(true)" :disabled="externalPowerBusy">Питание модулей ON</button>
          <button class="btn warning ghost" @click="externalPowerSet(false)" :disabled="externalPowerBusy">Питание модулей OFF</button>
          <button class="btn primary" @click="externalPowerCycle" :disabled="externalPowerBusy">Передернуть питание</button>
          <button class="btn ghost" @click="syncConfigInternalFlash" :disabled="configSyncBusy">Синхр. config во flash ESP</button>
          <button class="btn danger" @click="restartDevice" :disabled="restarting">Перезагрузить</button>
        </div>
        <p class="muted" v-if="externalPowerStatus">{{ externalPowerStatus }}</p>
        <p class="muted" v-if="configSyncStatus">{{ configSyncStatus }}</p>
        <p class="muted" v-if="restartStatus">{{ restartStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Термоконтроль</h3>
          <span class="badge accent">Heater + PID</span>
        </div>
        <div class="status-row">
          <span class="chip" :class="{ online: pidEnabled, subtle: !pidEnabled }">PID {{ pidEnabled ? 'On' : 'Off' }}</span>
          <span class="chip subtle">Выход: {{ pidOutputDisplay }}</span>
          <span class="chip subtle">Цель: {{ pidSetpointDisplay }}</span>
          <span class="chip subtle">Датчик: {{ pidSensorLabel }}</span>
          <span class="chip subtle">T: {{ pidSensorTemp }}</span>
        </div>
        <div class="form-group">
          <label>Ручной нагрев (%)</label>
          <div class="range-row">
            <input type="range" min="0" max="100" step="0.5" v-model.number="heaterPower" @input="heaterEditing = true" @change="heaterEditing = true" />
            <input type="number" min="0" max="100" step="0.1" v-model.number="heaterPower" @input="heaterEditing = true" />
          </div>
        </div>
        <button class="btn danger" @click="setHeater">Установить</button>
        <div class="divider"></div>
        <div class="form-group">
          <label>PID цель (°C)</label>
          <input type="number" min="0" step="0.1" v-model.number="pidForm.setpoint" @input="pidDirty = true" />
        </div>
        <div class="form-group">
          <label>PID датчики</label>
          <div class="chip-select">
            <button
              v-for="(sensor, idx) in tempEntries"
              :key="sensor.key"
              class="chip-option"
              :class="{ selected: pidForm.sensorIndices.includes(idx) }"
              type="button"
              @click="togglePidSensor(idx)"
            >
              {{ sensor.label }}{{ sensor.address ? ` (${sensor.address})` : '' }}
            </button>
            <span v-if="tempEntries.length === 0" class="muted">Нет датчиков</span>
          </div>
        </div>
        <div class="inline fields">
          <label class="compact">Kp
            <input type="number" step="0.01" v-model.number="pidForm.kp" @input="pidDirty = true" />
          </label>
          <label class="compact">Ki
            <input type="number" step="0.01" v-model.number="pidForm.ki" @input="pidDirty = true" />
          </label>
          <label class="compact">Kd
            <input type="number" step="0.01" v-model.number="pidForm.kd" @input="pidDirty = true" />
          </label>
        </div>
        <div class="actions">
          <button class="btn primary" @click="applyPid">Сохранить PID</button>
          <button class="btn success" @click="enablePid">Включить PID</button>
          <button class="btn warning ghost" @click="disablePid">Выключить PID</button>
        </div>
        <p class="muted" v-if="pidApplyStatus">{{ pidApplyStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Шаговик</h3>
          <span class="badge success">Мотор</span>
        </div>
        <div class="status-row">
          <span class="chip" :class="{ online: stepperEnabled, subtle: !stepperEnabled }">{{ stepperEnabled ? 'Enabled' : 'Disabled' }}</span>
          <span class="chip" :class="{ warm: stepperMoving, subtle: !stepperMoving }">{{ stepperMoving ? 'Moving' : 'Idle' }}</span>
          <span class="chip" :class="{ warn: stepperHoming, subtle: !stepperHoming }">{{ stepperHoming ? 'Homing' : 'Ready' }}</span>
          <span class="chip subtle">Dir: {{ stepperDirText }}</span>
          <span class="chip" :class="stepperHomeChipClass">Home: {{ stepperHomeLabel }}</span>
        </div>
        <div class="inline">
          <button class="btn success" @click="stepperEnable">Enable</button>
          <button class="btn ghost" @click="stepperDisable">Disable</button>
          <button class="btn primary" @click="stepperFindZero">Найти 0 Hall+offset</button>
          <button class="btn ghost" @click="stepperZero">Поставить 0</button>
        </div>
        <div class="form-group">
          <label>Шаги</label>
          <input type="number" v-model.number="stepper.steps" />
        </div>
        <div class="form-group">
          <label>Скорость, us</label>
          <input type="number" v-model.number="stepper.speedUs" />
        </div>
        <label class="inline checkbox"><input type="checkbox" v-model="stepper.reverse" /> <span>Реверс</span></label>
        <button class="btn primary" @click="stepperMove">Движение</button>
        <p class="muted">Pos: {{ device?.state?.stepperPosition }} Target: {{ device?.state?.stepperTarget }}</p>
        <p class="muted" v-if="stepperStatus">{{ stepperStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Wi‑Fi</h3>
          <span class="badge">Сеть</span>
        </div>
        <div class="status-row">
          <span class="chip strong">IP: {{ wifiIpDisplay }}</span>
          <span class="chip subtle">STA: {{ wifiStaIpDisplay }}</span>
          <span class="chip subtle">AP: {{ wifiApIpDisplay }}</span>
          <span class="chip" :class="{ online: wifiModeDisplay === 'sta', cool: wifiModeDisplay === 'ap' }">Mode: {{ wifiModeDisplay.toUpperCase() }}</span>
        </div>
        <div class="status-row">
          <span class="chip" :class="{ online: ethLinkUp, subtle: !ethLinkUp }">ETH link: {{ ethLinkUp ? 'UP' : 'DOWN' }}</span>
          <span class="chip" :class="{ online: ethIpUp, subtle: !ethIpUp }">ETH IP: {{ ethIpDisplay }}</span>
          <span class="chip subtle">Net: {{ netModeDisplay.toUpperCase() }}</span>
          <span class="chip subtle">Priority: {{ netPriorityDisplay.toUpperCase() }}</span>
        </div>
        <div class="status-row">
          <span class="chip subtle">RSSI: {{ device?.state?.wifiRssi ?? '--' }} dBm</span>
          <span class="chip subtle">Качество: {{ device?.state?.wifiQuality ?? '--' }}%</span>
          <span class="chip subtle">SSID: {{ wifiSsidDisplay }}</span>
        </div>
        <div class="divider"></div>
        <div class="form-group">
          <label>Сетевой режим</label>
          <select v-model="netForm.mode" @change="netDirty = true">
            <option value="wifi">Только Wi‑Fi</option>
            <option value="eth">Только Ethernet</option>
            <option value="both">Wi‑Fi + Ethernet</option>
          </select>
        </div>
        <div class="form-group">
          <label>Приоритет выхода в сеть</label>
          <select v-model="netForm.priority" @change="netDirty = true">
            <option value="wifi">Wi‑Fi</option>
            <option value="eth">Ethernet</option>
          </select>
        </div>
        <button class="btn primary" @click="applyNetwork">Применить сеть</button>
        <p class="muted" v-if="netApplyStatus">{{ netApplyStatus }}</p>
        <div class="divider"></div>
        <div class="form-group">
          <label>Режим</label>
          <select v-model="wifiForm.mode" @change="wifiDirty = true">
            <option value="sta">STA (подключение)</option>
            <option value="ap">AP (точка доступа)</option>
          </select>
        </div>
        <div class="form-group">
          <label>SSID</label>
          <input type="text" v-model="wifiForm.ssid" @input="wifiDirty = true" />
        </div>
        <div class="form-group">
          <label>Пароль</label>
          <input type="password" v-model="wifiForm.password" @input="wifiDirty = true" />
        </div>
        <button class="btn primary" @click="applyWifi">Применить Wi‑Fi</button>
        <p class="muted" v-if="wifiApplyStatus">{{ wifiApplyStatus }}</p>
      </div>
    </div>

    <div class="card" v-show="activeTab === 'data'">
      <div class="card-head">
        <h3>История измерений</h3>
        <span class="badge">Графики</span>
      </div>
      <div class="inline fields">
        <label class="compact">С
          <input
            type="datetime-local"
            class="date-input"
            v-model="historyFilters.from"
            @focus="historyDateEditing.from = true"
            @blur="validateHistoryDate('from')"
          />
        </label>
        <label class="compact">По
          <input
            type="datetime-local"
            class="date-input"
            v-model="historyFilters.to"
            :disabled="historyAutoRefresh"
            @focus="historyDateEditing.to = true"
            @blur="validateHistoryDate('to')"
          />
        </label>
        <label class="compact">Лимит
          <input type="number" min="100" max="10000" step="100" v-model.number="historyFilters.limit" />
        </label>
      </div>
      <p class="muted" v-if="historyDateError">{{ historyDateError }}</p>
      <p class="muted" v-if="browserTzLabel">Таймзона браузера: {{ browserTzLabel }}</p>
      <div class="inline fields">
        <label class="compact">Усреднение
          <select v-model="historyFilters.bucketMode">
            <option value="auto">Авто</option>
            <option value="manual">Вручную</option>
          </select>
        </label>
        <label class="compact" v-if="historyFilters.bucketMode === 'manual'">Окно
          <div class="inline">
            <input type="number" min="1" step="1" v-model.number="historyFilters.bucketValue" />
            <select v-model="historyFilters.bucketUnit">
              <option value="s">сек</option>
              <option value="m">мин</option>
              <option value="h">ч</option>
            </select>
          </div>
        </label>
      </div>
      <p class="muted" v-if="historyRangeLabel">{{ historyRangeLabel }}</p>
      <div class="actions">
        <button class="btn primary" @click="loadHistory" :disabled="historyLoading">Загрузить</button>
        <span class="muted" v-if="historyStatus">{{ historyStatus }}</span>
        <span class="muted" v-if="atmosphereStatus">{{ atmosphereStatus }}</span>
      </div>
      <div class="inline fields">
        <label class="checkbox">
          <input type="checkbox" v-model="historyAutoRefresh" />
          <span>Автообновление</span>
        </label>
        <label class="compact">Интервал (сек)
          <input type="number" min="5" max="300" step="5" v-model.number="historyRefreshSec" />
        </label>
      </div>
      <div class="status-row" v-if="historyBucketLabel && historyBucketLabel !== 'raw'">
        <span class="chip subtle">Окно: {{ historyBucketLabel }}</span>
        <span class="chip subtle">Исходно: {{ historyRawCount }}</span>
      </div>
      <div class="form-group">
        <label>Температурные датчики</label>
        <div class="chip-select">
          <button
            v-for="sensor in historyTempOptions"
            :key="`history-temp-${sensor.idx}`"
            type="button"
            class="chip-option"
            :class="{ selected: historySelection.tempIndices.includes(sensor.idx) }"
            @click="toggleHistoryTemp(sensor.idx)"
          >
            {{ sensor.label }}
          </button>
          <span v-if="historyTempOptions.length === 0" class="muted">Нет датчиков</span>
        </div>
      </div>
      <div class="form-group">
        <label>Серии ADC</label>
        <div class="chip-select">
          <button
            v-for="series in adcSeriesOptions"
            :key="series.key"
            type="button"
            class="chip-option"
            :class="{ selected: historySelection.adcSeries.includes(series.key) }"
            @click="toggleAdcSeries(series.key)"
          >
            {{ series.label }}
          </button>
        </div>
      </div>
      <div class="chart-stack">
        <div class="chart-box">
          <h4>Температуры</h4>
          <div class="chart-body">
            <canvas ref="tempChartEl"></canvas>
          </div>
        </div>
        <div class="chart-box">
          <h4>ADC + Cal</h4>
          <div class="chart-body">
            <canvas ref="adcChartEl"></canvas>
          </div>
        </div>
        <div class="chart-box">
          <h4>Яркостная температура Tk</h4>
          <div class="chart-body">
            <canvas ref="brightnessChartEl"></canvas>
          </div>
        </div>
        <div class="chart-box">
          <h4>Контроль теплой нагрузки</h4>
          <div class="chart-body">
            <canvas ref="loadCheckChartEl"></canvas>
          </div>
        </div>
        <div class="chart-box">
          <h4>Эффективная температура по зондам</h4>
          <div class="chart-body">
            <canvas ref="teffChartEl"></canvas>
          </div>
        </div>
        <div class="chart-box">
          <div class="chart-head-row">
            <h4>Тау атмосферы</h4>
            <div class="inline fields compact-controls">
              <label class="checkbox">
                <input type="checkbox" v-model="atmosphereAverage" :disabled="atmosphereStationOptions.length === 0" />
                <span>Усреднить</span>
              </label>
              <label class="compact">Станция
                <select v-model="atmospherePrimaryStation" :disabled="atmosphereAverage || atmosphereStationOptions.length === 0">
                  <option v-for="station in atmosphereStationOptions" :key="station.station_id" :value="station.station_id">
                    {{ stationLabel(station) }}
                  </option>
                </select>
              </label>
            </div>
          </div>
          <div class="chart-body">
            <canvas ref="tauChartEl"></canvas>
          </div>
        </div>
        <div class="chart-box">
          <h4>PWV</h4>
          <div class="chart-body">
            <canvas ref="pwvAtmosphereChartEl"></canvas>
          </div>
        </div>
      </div>
    </div>

    <div class="card" v-show="activeTab === 'settings'">
      <div class="card-head">
        <h3>Настройки устройства</h3>
        <span class="badge">Конфиг</span>
      </div>
      <p class="muted" v-if="configStatus">{{ configStatus }}</p>
      <div class="form-group">
        <label>Название устройства</label>
        <input type="text" v-model="configForm.displayName" @input="configDirty = true" />
      </div>
      <div class="form-group">
        <label>Температурные датчики</label>
        <div class="config-table">
          <div class="config-row header">
            <span>Индекс</span>
            <span>Адрес</span>
            <span>Имя</span>
          </div>
          <div v-if="configForm.tempRows.length === 0" class="muted">Нет данных о датчиках</div>
          <div
            v-for="row in configForm.tempRows"
            :key="`${row.index ?? 'x'}-${row.address ?? 'na'}`"
            class="config-row"
          >
            <span class="chip subtle">{{ row.index !== null ? `t${row.index + 1}` : '—' }}</span>
            <span class="muted small">{{ row.address || '—' }}</span>
            <input type="text" v-model="row.label" @input="configDirty = true" />
          </div>
        </div>
      </div>
      <div class="form-group">
        <label>ADC / Cal</label>
        <div class="config-grid">
          <label class="compact" v-for="key in adcLabelKeys" :key="key">
            {{ adcLabelDefaults[key] }}
            <input type="text" v-model="configForm.adcLabels[key]" @input="configDirty = true" />
          </label>
        </div>
      </div>
      <div class="form-group">
        <label>Привязка температур</label>
        <div class="config-binding-list">
          <label class="config-binding-row" v-for="binding in tempBindingRoles" :key="binding.key">
            <span>{{ tempBindingLabel(binding.key) }}</span>
            <select v-model="configForm.tempBindings[binding.key]" @change="configDirty = true">
              <option value="">Не задано</option>
              <option
                v-for="sensor in tempSensorOptions"
                :key="`${binding.key}-${sensor.address}`"
                :value="sensor.address"
              >
                {{ sensor.label }} ({{ sensor.address }})
              </option>
            </select>
          </label>
        </div>
      </div>
      <div class="form-group">
        <label>Атмосферные профили для расчета Tэфф / tau / PWV</label>
        <div class="config-grid">
          <label class="compact">Высота прибора, м
            <input type="number" min="0" step="1" v-model.number="configForm.atmosphere.altitudeM" @input="configDirty = true" />
          </label>
          <label class="compact">h0, м
            <input type="number" min="1" step="1" v-model.number="configForm.atmosphere.h0M" @input="configDirty = true" />
          </label>
        </div>
        <div class="chip-select">
          <button
            v-for="station in stationOptions"
            :key="`atm-station-${station.station_id}`"
            type="button"
            class="chip-option"
            :class="{ selected: configForm.atmosphere.stationIds.includes(station.station_id) }"
            @click="toggleAtmosphereStation(station.station_id)"
          >
            {{ stationLabel(station) }}
          </button>
          <span v-if="stationOptions.length === 0" class="muted">Станции не загружены</span>
        </div>
        <p class="muted small">Профили с количеством точек меньше 50 в расчет не попадают.</p>
      </div>
      <div class="actions">
        <button class="btn primary" @click="saveConfig" :disabled="configSaving">Сохранить</button>
        <button class="btn ghost" @click="resetConfig" :disabled="configSaving">Сбросить</button>
      </div>
    </div>

    <DeviceGpsPanel
      v-show="activeTab === 'gps'"
      :has-gps="hasGps"
      :actual-mode="gpsActualMode"
      :antenna-short="gpsAntennaShort"
      :antenna-short-raw="gpsAntennaShortRaw"
      :position-valid="gpsPositionValid"
      :latitude="gpsLatitude"
      :longitude="gpsLongitude"
      :altitude="gpsAltitude"
      :fix-quality="gpsFixQuality"
      :satellites="gpsSatellites"
      :position-age-ms="gpsPositionAgeMs"
      :time-valid="gpsTimeValid"
      :time-iso="gpsTimeIso"
      :time-age-ms="gpsTimeAgeMs"
      :form="gpsForm"
      :saving="gpsSaving"
      :status="gpsStatus"
      @save="saveGpsConfig"
      @probe="probeGpsMode"
      @update:mode="updateGpsMode"
      @update:rtcm-text="updateGpsRtcmText"
    />

    <DeviceCalibrationPanel
      v-show="activeTab === 'calibration'"
      :device-id="deviceId"
      :logging="!!device?.state?.logging"
      :adc-labels="adcLabelMap"
      :temp-bindings="deviceConfig?.temp_bindings || {}"
      :temp-sensors="tempEntries"
    />

    <div class="card" v-show="activeTab === 'errors'">
      <div class="card-head">
        <h3>Ошибки устройства</h3>
        <span class="badge">{{ errorTotal }}</span>
      </div>
      <div class="inline fields">
        <label class="compact">С
          <input type="datetime-local" class="date-input" v-model="errorFilters.from" />
        </label>
        <label class="compact">По
          <input type="datetime-local" class="date-input" v-model="errorFilters.to" />
        </label>
        <label class="compact">Статус
          <select v-model="errorFilters.status">
            <option value="all">Все</option>
            <option value="active">Active</option>
            <option value="cleared">Cleared</option>
          </select>
        </label>
      </div>
      <div class="inline fields">
        <label class="compact">Название
          <input type="text" v-model="errorFilters.name" placeholder="code" />
        </label>
        <label class="compact">Лимит
          <input type="number" min="20" max="1000" step="20" v-model.number="errorFilters.limit" />
        </label>
        <label class="compact">Страница
          <input type="number" min="1" :max="errorPageCount" step="1" v-model.number="errorFilters.page" />
        </label>
      </div>
      <div class="actions">
        <button class="btn primary sm" @click="loadErrors" :disabled="errorLoading">Применить</button>
        <button class="btn ghost sm" @click="resetErrorFilters" :disabled="errorLoading">Сбросить</button>
        <button class="btn ghost sm" @click="goToErrorPage(errorFilters.page - 1)" :disabled="errorLoading || errorFilters.page <= 1">←</button>
        <button class="btn ghost sm" @click="goToErrorPage(errorFilters.page + 1)" :disabled="errorLoading || errorFilters.page >= errorPageCount">→</button>
        <span class="muted" v-if="errorRangeLabel">{{ errorRangeLabel }}</span>
        <span class="muted" v-if="errorStatus">{{ errorStatus }}</span>
      </div>
      <p class="muted" v-if="errorLoading">Загрузка...</p>
      <div v-else-if="errorEvents.length === 0" class="muted">Ошибок не найдено</div>
      <div v-else class="error-list">
        <div v-for="event in errorEvents" :key="event.id" class="error-item">
          <div class="error-head">
            <div class="error-title">{{ event.code }}</div>
            <div class="error-tags">
              <span class="chip" :class="severityClass(event.severity)">{{ event.severity }}</span>
              <span class="chip" :class="{ online: event.active, subtle: !event.active }">
                {{ event.active ? 'Active' : 'Cleared' }}
              </span>
            </div>
          </div>
          <div class="muted small">{{ formatErrorTime(event.timestamp, event.created_at) }}</div>
          <div class="error-message">{{ event.message || '—' }}</div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'

definePageMeta({ layout: 'admin' })

type MeasurementPoint = {
  timestamp: string
  timestamp_ms: number | null
  adc1: number
  adc2: number
  adc3: number
  temps: number[]
  bus_v: number
  bus_i: number
  bus_p: number
  adc1_cal: number | null
  adc2_cal: number | null
  adc3_cal: number | null
  gps_lat: number | null
  gps_lon: number | null
  gps_alt: number | null
  gps_fix_quality: number | null
  gps_satellites: number | null
  gps_fix_age_ms: number | null
  brightness_temp1: number | null
  brightness_temp2: number | null
  brightness_temp3: number | null
  cal_brightness_temp1: number | null
  cal_brightness_temp2: number | null
  cal_brightness_temp3: number | null
}

type MeasurementsResponse = {
  points: MeasurementPoint[]
  raw_count: number
  limit: number
  bucket_seconds: number
  bucket_label: string
  aggregated: boolean
  temp_labels: string[]
  temp_label_map: Record<string, string>
  adc_labels: Record<string, string>
  temp_addresses: string[]
  temp_bindings: Record<string, string>
  brightness_temp_labels: Record<string, string>
}

type ErrorEvent = {
  id: string
  device_id: string
  timestamp: string
  timestamp_ms: number | null
  code: string
  severity: string
  message: string
  active: boolean
  created_at: string
}

type ErrorEventsResponse = {
  items: ErrorEvent[]
  total: number
  limit: number
  offset: number
}

type DeviceConfig = {
  id: string
  display_name: string | null
  created_at: string
  last_seen_at: string | null
  temp_labels: string[]
  temp_addresses: string[]
  temp_label_map: Record<string, string>
  temp_bindings: Record<string, string>
  atmosphere_config: AtmosphereConfig
  adc_labels: Record<string, string>
}

type AtmosphereConfig = {
  station_ids?: string[]
  altitude_m?: number
  h0_m?: number
  tau_station_id?: string
  tau_average?: boolean
}

type StationOption = {
  station_id: string
  name: string | null
}

type AtmosphereTeffPoint = {
  station_id: string
  station_name: string | null
  sounding_time: string
  t_eff: number | null
  pwv_profile: number | null
  row_count: number
}

type AtmosphereMeasurementPoint = {
  timestamp: string
  timestamp_ms: number | null
  t_eff: number | null
  t_eff_station_id: string | null
  t_eff_age_hours: number | null
  brightness_temp1: number | null
  brightness_temp2: number | null
  brightness_temp3: number | null
  tau1: number | null
  tau2: number | null
  tau3: number | null
  pwv1: number | null
  pwv2: number | null
  pwv3: number | null
}

type AtmosphereResponse = {
  config: AtmosphereConfig
  station_labels: Record<string, string>
  adc_labels: Record<string, string>
  t_eff_points: AtmosphereTeffPoint[]
  measurement_points: AtmosphereMeasurementPoint[]
  raw_count: number
  bucket_seconds: number
  bucket_label: string
  aggregated: boolean
}

type DeviceGpsConfig = {
  device_id: string
  has_gps: boolean
  rtcm_types: number[]
  mode: string
  actual_mode: string | null
  updated_at: string | null
  created_at: string | null
}

type TempConfigRow = {
  index: number | null
  address: string | null
  label: string
}

type DeviceTab = 'data' | 'control' | 'gps' | 'calibration' | 'settings' | 'errors'

const { apiFetch } = useApi()
const route = useRoute()
const deviceId = computed(() => route.params.deviceId as string)
const store = useDevicesStore()
const nuxtApp = useNuxtApp()
store.init(nuxtApp.$mqtt)

const device = computed(() => (deviceId.value ? store.devices.get(deviceId.value) : undefined))
const deviceConfig = ref<DeviceConfig | null>(null)
const validTabs = new Set<DeviceTab>(['data', 'control', 'gps', 'calibration', 'settings', 'errors'])
const tabFromQuery = (): DeviceTab => {
  const raw = Array.isArray(route.query.tab) ? route.query.tab[0] : route.query.tab
  return raw && validTabs.has(raw as DeviceTab) ? (raw as DeviceTab) : 'control'
}
const activeTab = ref<DeviceTab>(tabFromQuery())
const configDirty = ref(false)
const configStatus = ref('')
const configSaving = ref(false)
const configForm = reactive({
  displayName: '',
  tempRows: [] as TempConfigRow[],
  adcLabels: {
    adc1: '',
    adc2: '',
    adc3: '',
    adc1_cal: '',
    adc2_cal: '',
    adc3_cal: '',
  },
  tempBindings: {
    radiometer_adc1: '',
    radiometer_adc2: '',
    radiometer_adc3: '',
    calibration_load: '',
  } as Record<string, string>,
  atmosphere: {
    stationIds: [] as string[],
    altitudeM: 0,
    h0M: 5300,
  },
})

const buildTempLabelMap = (
  config?: Pick<DeviceConfig, 'temp_labels' | 'temp_addresses' | 'temp_label_map'> | null,
) => {
  const labelByAddress = new Map<string, string>()
  Object.entries(config?.temp_label_map || {}).forEach(([address, label]) => {
    if (address && label) labelByAddress.set(address, label)
  })
  const labels = config?.temp_labels || []
  const addresses = config?.temp_addresses || []
  addresses.forEach((address, idx) => {
    if (address && !labelByAddress.has(address)) {
      labelByAddress.set(address, labels[idx] || `t${idx + 1}`)
    }
  })
  return labelByAddress
}
const gpsConfig = ref<DeviceGpsConfig | null>(null)
const gpsDirty = ref(false)
const gpsSaving = ref(false)
const gpsStatus = ref('')
const gpsForm = reactive({
  mode: 'base_time_60',
  rtcmText: '1004,1006,1033',
})
const tempEntries = computed(() => {
  const sensors = device.value?.state?.tempSensors
  const byIndex = deviceConfig.value?.temp_labels || []
  const labelByAddress = buildTempLabelMap(deviceConfig.value)
  if (Array.isArray(sensors)) {
    return sensors.map((value, idx) => ({
      key: `t${idx + 1}`,
      label: byIndex[idx] || `t${idx + 1}`,
      value,
      address: '',
    }))
  }
  if (!sensors || typeof sensors !== 'object') return []
  const entries = Object.entries(sensors as Record<string, any>)
  entries.sort(([a], [b]) => {
    const ai = a.startsWith('t') ? Number(a.slice(1)) : Number.NaN
    const bi = b.startsWith('t') ? Number(b.slice(1)) : Number.NaN
    if (Number.isFinite(ai) && Number.isFinite(bi)) return ai - bi
    if (Number.isFinite(ai)) return -1
    if (Number.isFinite(bi)) return 1
    return a.localeCompare(b)
  })
  return entries.map(([label, entry], idx) => {
    if (entry && typeof entry === 'object') {
      const address = entry.address || ''
      const renamed = (address && labelByAddress.get(address)) || byIndex[idx] || label
      return { key: label, label: renamed, value: entry.value, address }
    }
    const renamed = byIndex[idx] || label
    return { key: label, label: renamed, value: entry, address: '' }
  })
})
const historyTempOptions = computed(() => {
  const labels = historyTempLabels.value.length
    ? historyTempLabels.value
    : deviceConfig.value?.temp_labels?.length
    ? deviceConfig.value.temp_labels
    : tempEntries.value.map((entry, idx) => entry.label || `t${idx + 1}`)
  return labels.map((label, idx) => ({ label, idx }))
})
const deviceTitle = computed(() => deviceConfig.value?.display_name || deviceId.value || '—')
useHead(() => ({
  title: deviceTitle.value ? `Устройство ${deviceTitle.value}` : 'Устройство',
}))
const adcLabelMap = computed(() => deviceConfig.value?.adc_labels || {})
const gpsLiveTypes = computed(() => {
  const raw = device.value?.state?.gpsRtcmTypes
  if (!Array.isArray(raw)) return []
  return raw.map((value: any) => Number(value)).filter((value: number) => Number.isFinite(value) && value > 0)
})
const gpsActualMode = computed(() => String(device.value?.state?.gpsActualMode || gpsConfig.value?.actual_mode || ''))
const gpsAntennaShort = computed(() => {
  const value = device.value?.state?.gpsAntennaShort
  return typeof value === 'boolean' ? value : null
})
const gpsAntennaShortRaw = computed(() => {
  const value = Number(device.value?.state?.gpsAntennaShortRaw)
  return Number.isFinite(value) ? value : null
})
const optionalNumberState = (key: string) => {
  const value = Number(device.value?.state?.[key])
  return Number.isFinite(value) ? value : null
}
const gpsPositionValid = computed(() => device.value?.state?.gpsPositionValid === true)
const gpsLatitude = computed(() => optionalNumberState('gpsLat'))
const gpsLongitude = computed(() => optionalNumberState('gpsLon'))
const gpsAltitude = computed(() => optionalNumberState('gpsAlt'))
const gpsFixQuality = computed(() => optionalNumberState('gpsFixQuality'))
const gpsSatellites = computed(() => optionalNumberState('gpsSatellites'))
const gpsPositionAgeMs = computed(() => optionalNumberState('gpsPositionAgeMs') ?? optionalNumberState('gpsFixAgeMs'))
const gpsTimeValid = computed(() => device.value?.state?.gpsTimeValid === true)
const gpsTimeIso = computed(() => String(device.value?.state?.gpsTimeIso || ''))
const gpsTimeAgeMs = computed(() => optionalNumberState('gpsTimeAgeMs'))
const hasGps = computed(() => {
  const state = device.value?.state || {}
  return !!gpsConfig.value?.has_gps ||
    gpsLiveTypes.value.length > 0 ||
    !!state.gpsMode ||
    !!state.gpsActualMode ||
    state.gpsPositionValid === true ||
    state.gpsTimeValid === true
})
const adcLabelDefaults: Record<string, string> = {
  adc1: 'ADC1',
  adc2: 'ADC2',
  adc3: 'ADC3',
  adc1_cal: 'ADC1 Cal',
  adc2_cal: 'ADC2 Cal',
  adc3_cal: 'ADC3 Cal',
}
const adcLabelKeys = Object.keys(adcLabelDefaults)
const adcLabel = (key: string, fallback: string) => adcLabelMap.value[key] || fallback
const tempBindingRoles = [
  { key: 'radiometer_adc1' },
  { key: 'radiometer_adc2' },
  { key: 'radiometer_adc3' },
  { key: 'calibration_load' },
]
const tempBindingLabel = (key: string) => {
  if (key === 'radiometer_adc1') return `Температура ${adcLabel('adc1', 'ADC1')}`
  if (key === 'radiometer_adc2') return `Температура ${adcLabel('adc2', 'ADC2')}`
  if (key === 'radiometer_adc3') return `Температура ${adcLabel('adc3', 'ADC3')}`
  return 'Теплая калибровочная нагрузка'
}
const tempSensorOptions = computed(() => {
  const seen = new Set<string>()
  return configForm.tempRows
    .map((row) => {
      const address = (row.address || '').trim()
      const label = row.label.trim() || (row.index !== null ? `t${row.index + 1}` : address)
      return { address, label }
    })
    .filter((row) => {
      if (!row.address || seen.has(row.address)) return false
      seen.add(row.address)
      return true
    })
})
const atmosphereStationOptions = computed(() => {
  const ids = deviceConfig.value?.atmosphere_config?.station_ids || []
  const byId = new Map(stationOptions.value.map((station) => [station.station_id, station]))
  return ids.map((id) => byId.get(id) || { station_id: id, name: atmosphereData.value?.station_labels?.[id] || null })
})

const log = reactive({ filename: 'data', useMotor: false, durationSec: 1 })
const stepper = reactive({ steps: 400, speedUs: 1500, reverse: false })
const logStatus = ref('')
const stepperStatus = ref('')
const heaterPower = ref(0)
const heaterEditing = ref(false)
const pidDirty = ref(false)
const wifiDirty = ref(false)
const netDirty = ref(false)
const pidApplyStatus = ref('')
const wifiApplyStatus = ref('')
const netApplyStatus = ref('')
const restartStatus = ref('')
const restarting = ref(false)
const externalPowerStatus = ref('')
const externalPowerBusy = ref(false)
const externalPowerOffMs = ref(1000)
const configSyncStatus = ref('')
const configSyncBusy = ref(false)
const pidForm = reactive({ setpoint: 25, sensorIndices: [] as number[], kp: 1, ki: 0, kd: 0 })
const wifiForm = reactive({ mode: 'sta', ssid: '', password: '' })
const netForm = reactive({ mode: 'wifi', priority: 'wifi' })
const historyFilters = reactive({
  from: '',
  to: '',
  limit: 2000,
  bucketMode: 'auto',
  bucketValue: 10,
  bucketUnit: 's',
})
const historySelection = reactive({
  tempIndices: [] as number[],
  adcSeries: ['adc1', 'adc2', 'adc3'] as string[],
})
const historyData = ref<MeasurementPoint[]>([])
const historyTempLabels = ref<string[]>([])
const historyTempAddresses = ref<string[]>([])
const historyTempBindings = ref<Record<string, string>>({})
const historyBrightnessLabels = ref<Record<string, string>>({})
const atmosphereData = ref<AtmosphereResponse | null>(null)
const atmosphereStatus = ref('')
const atmosphereAverage = ref(false)
const atmospherePrimaryStation = ref('')
const stationOptions = ref<StationOption[]>([])
const historyLoading = ref(false)
const historyStatus = ref('')
const historyBucketLabel = ref('')
const historyRawCount = ref(0)
const historyAutoRefresh = ref(false)
const historyRefreshSec = ref(15)
let historyTimer: ReturnType<typeof setInterval> | null = null
const errorEvents = ref<ErrorEvent[]>([])
const errorLoading = ref(false)
const errorStatus = ref('')
const errorTotal = ref(0)
const errorFilters = reactive({
  from: '',
  to: '',
  status: 'all',
  name: '',
  limit: 200,
  page: 1,
})
const browserTzLabel = ref('')
const errorPageCount = computed(() => Math.max(1, Math.ceil(errorTotal.value / errorFilters.limit)))
const errorOffset = computed(() => Math.max(0, (errorFilters.page - 1) * errorFilters.limit))
const errorRangeLabel = computed(() => {
  if (errorTotal.value === 0) return ''
  const start = errorOffset.value + 1
  const end = errorOffset.value + errorEvents.value.length
  return `${start}–${end} из ${errorTotal.value}`
})
const tempChartEl = ref<HTMLCanvasElement | null>(null)
const adcChartEl = ref<HTMLCanvasElement | null>(null)
const brightnessChartEl = ref<HTMLCanvasElement | null>(null)
const loadCheckChartEl = ref<HTMLCanvasElement | null>(null)
const teffChartEl = ref<HTMLCanvasElement | null>(null)
const tauChartEl = ref<HTMLCanvasElement | null>(null)
const pwvAtmosphereChartEl = ref<HTMLCanvasElement | null>(null)
let tempChart: any = null
let adcChart: any = null
let brightnessChart: any = null
let loadCheckChart: any = null
let teffChart: any = null
let tauChart: any = null
let pwvAtmosphereChart: any = null
let ChartCtor: any = null

const maskToIndices = (mask: number, count: number) => {
  const out: number[] = []
  for (let i = 0; i < count; i++) {
    if (mask & (1 << i)) out.push(i)
  }
  return out
}

const normalizeIndices = (indices: number[], count: number) => {
  const set = new Set(indices.filter((idx) => idx >= 0 && idx < count))
  return Array.from(set).sort((a, b) => a - b)
}

const pidStateSensorIndices = computed(() => {
  const count = tempEntries.value.length
  const state = device.value?.state || {}
  const mask = Number(state.pidSensorMask)
  if (Number.isFinite(mask) && mask > 0) {
    const indices = maskToIndices(mask, count)
    return indices.length ? indices : (count ? [0] : [])
  }
  const idx = Number(state.pidSensorIndex)
  if (Number.isFinite(idx) && count > 0) {
    return [Math.max(0, Math.min(count - 1, idx))]
  }
  return []
})

const pidEnabled = computed(() => !!device.value?.state?.pidEnabled)
const pidOutputDisplay = computed(() => {
  const val = device.value?.state?.pidOutput
  return Number.isFinite(val) ? `${Number(val).toFixed(1)} %` : '--'
})
const pidSetpointDisplay = computed(() => {
  const val = device.value?.state?.pidSetpoint
  return Number.isFinite(val) ? `${Number(val).toFixed(1)} °C` : '--'
})
const pidSensorLabel = computed(() => {
  if (pidStateSensorIndices.value.length === 0) return '--'
  return pidStateSensorIndices.value
    .map((idx) => tempEntries.value[idx]?.label || `T${idx + 1}`)
    .join(', ')
})
const pidSensorTemp = computed(() => {
  const indices = pidStateSensorIndices.value
  if (!indices.length) return '--'
  let sum = 0
  let count = 0
  indices.forEach((idx) => {
    const entry = tempEntries.value[idx]
    if (!entry || !Number.isFinite(entry.value)) return
    sum += Number(entry.value)
    count += 1
  })
  if (!count) return '--'
  return `${(sum / count).toFixed(2)} °C`
})

const stepperEnabled = computed(() => !!device.value?.state?.stepperEnabled)
const stepperMoving = computed(() => !!device.value?.state?.stepperMoving)
const stepperHoming = computed(() => !!device.value?.state?.stepperHoming)
const stepperHomeStatus = computed(() => (device.value?.state?.stepperHomeStatus as string) || 'idle')
const stepperHomeLabel = computed(() => {
  switch (stepperHomeStatus.value) {
    case 'ok':
    case 'hall_zero':
      return 'Найден'
    case 'offset_zero':
      return 'Offset 0'
    case 'manual_zero':
      return 'Ручной 0'
    case 'not_found':
      return 'Не найден'
    case 'aborted':
      return 'Отменен'
    case 'running':
    case 'seeking_hall':
    case 'applying_offset':
      return 'Поиск...'
    default:
      return 'Нет'
  }
})
const stepperHomeChipClass = computed(() => {
  if (['ok', 'hall_zero', 'offset_zero', 'manual_zero'].includes(stepperHomeStatus.value)) return 'online'
  if (['running', 'seeking_hall', 'applying_offset'].includes(stepperHomeStatus.value)) return 'cool'
  if (stepperHomeStatus.value === 'not_found' || stepperHomeStatus.value === 'aborted') return 'warn'
  return 'subtle'
})
const stepperDirText = computed(() => {
  const dir = device.value?.state?.stepperDirForward
  if (dir === undefined || dir === null) return '--'
  return dir ? 'FWD' : 'REV'
})
const externalPowerOn = computed(() => device.value?.state?.externalPowerOn !== false)

const wifiModeDisplay = computed(() => {
  const mode = device.value?.state?.wifiMode
  if (mode === 'ap' || mode === 'sta') return mode
  return device.value?.state?.wifiApMode ? 'ap' : 'sta'
})
const wifiIpDisplay = computed(() => device.value?.state?.wifiIp || '--')
const wifiStaIpDisplay = computed(() => device.value?.state?.wifiStaIp || '--')
const wifiApIpDisplay = computed(() => device.value?.state?.wifiApIp || '--')
const wifiSsidDisplay = computed(() => device.value?.state?.wifiSsid || '--')
const ethIpDisplay = computed(() => device.value?.state?.ethIp || '--')
const ethLinkUp = computed(() => !!device.value?.state?.ethLink)
const ethIpUp = computed(() => !!device.value?.state?.ethIpUp)
const netModeDisplay = computed(() => {
  const mode = device.value?.state?.netMode
  return mode === 'eth' || mode === 'both' || mode === 'wifi' ? mode : 'wifi'
})
const netPriorityDisplay = computed(() => (device.value?.state?.netPriority === 'eth' ? 'eth' : 'wifi'))

const formatBytes = (value: number) => {
  if (!Number.isFinite(value) || value <= 0) return '--'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let idx = 0
  let v = value
  while (v >= 1024 && idx < units.length - 1) {
    v /= 1024
    idx++
  }
  const digits = idx === 0 ? 0 : idx <= 2 ? 1 : 2
  return `${v.toFixed(digits)} ${units[idx]}`
}

const sdTotalBytes = computed(() => Number(device.value?.state?.sdTotalBytes ?? 0))
const sdUsedBytes = computed(() => Number(device.value?.state?.sdUsedBytes ?? 0))
const sdUsageLabel = computed(() => {
  if (!sdTotalBytes.value || sdTotalBytes.value <= 0) return '--'
  const percent = Math.round((sdUsedBytes.value * 100) / sdTotalBytes.value)
  return `${formatBytes(sdUsedBytes.value)} / ${formatBytes(sdTotalBytes.value)} (${percent}%)`
})
const sdRootFiles = computed(() => device.value?.state?.sdRootDataFiles ?? 0)
const sdToUploadFiles = computed(() => device.value?.state?.sdToUploadFiles ?? 0)
const sdUploadedFiles = computed(() => device.value?.state?.sdUploadedFiles ?? 0)
const heapFreeLabel = computed(() => formatBytes(Number(device.value?.state?.heapFreeBytes ?? 0)))
const heapLargestLabel = computed(() => formatBytes(Number(device.value?.state?.heapLargestFreeBlockBytes ?? 0)))
const minioUploadAttempts = computed(() => device.value?.state?.minioUploadAttempts ?? 0)
const minioLastAttemptLabel = computed(() => {
  const ms = Number(device.value?.state?.minioLastAttemptMs ?? 0)
  if (!Number.isFinite(ms) || ms <= 0) return '--'
  return `${Math.round(ms / 1000)}s after boot`
})

const parseLocalInput = (value: string) => {
  if (!value) return null
  const match = value.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})$/)
  if (!match) return null
  const year = Number(match[1])
  const month = Number(match[2])
  const day = Number(match[3])
  const hour = Number(match[4])
  const minute = Number(match[5])
  if (![year, month, day, hour, minute].every((n) => Number.isFinite(n))) return null
  const dt = new Date(year, month - 1, day, hour, minute, 0, 0)
  if (Number.isNaN(dt.getTime())) return null
  if (dt.getFullYear() !== year || dt.getMonth() !== month - 1 || dt.getDate() !== day) return null
  return dt
}

const parseDateAny = (value: string) => {
  if (!value) return null
  const local = parseLocalInput(value)
  if (local) return local
  const dt = new Date(value)
  if (Number.isNaN(dt.getTime())) return null
  return dt
}

const localInputToIso = (value: string) => {
  const dt = parseLocalInput(value)
  return dt ? dt.toISOString() : ''
}

const formatTimestamp = (value: string) => {
  const dt = parseDateAny(value)
  if (!dt || Number.isNaN(dt.getTime())) return value
  return dt.toLocaleString('ru-RU', {
    day: '2-digit',
    month: '2-digit',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  })
}

const stationLabel = (station: StationOption) => {
  return station.name ? `${station.station_id} · ${station.name}` : station.station_id
}

const toLocalInputValue = (date: Date) => {
  const offset = date.getTimezoneOffset() * 60000
  return new Date(date.getTime() - offset).toISOString().slice(0, 16)
}

const formatRangeDate = (value: string) => {
  const dt = parseDateAny(value)
  if (!dt || Number.isNaN(dt.getTime())) return value
  return dt.toLocaleString('ru-RU', {
    day: '2-digit',
    month: '2-digit',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  })
}

const historyRangeLabel = computed(() => {
  if (!historyFilters.from) return ''
  const fromLabel = formatRangeDate(historyFilters.from)
  if (historyAutoRefresh.value) return `Диапазон: ${fromLabel} — сейчас`
  if (!historyFilters.to) return ''
  return `Диапазон: ${fromLabel} — ${formatRangeDate(historyFilters.to)}`
})

const historyDateEditing = reactive({ from: false, to: false })
const historyDateError = ref('')

const validateHistoryDate = (field: 'from' | 'to') => {
  historyDateEditing[field] = false
  const value = field === 'from' ? historyFilters.from : historyFilters.to
  if (!value) {
    historyDateError.value = ''
    return true
  }
  const dt = parseDateAny(value)
  if (!dt || Number.isNaN(dt.getTime())) {
    historyDateError.value = 'Введите корректную дату и время'
    return false
  }
  historyDateError.value = ''
  return true
}

const getHistoryWindowEnd = (timestamp: string) => {
  const dt = parseDateAny(timestamp)
  return dt ?? null
}

const setHistoryWindow = (end: Date) => {
  historyDateEditing.from = false
  historyDateEditing.to = false
  historyFilters.to = toLocalInputValue(end)
  historyFilters.from = toLocalInputValue(new Date(end.getTime() - 24 * 60 * 60 * 1000))
}

const palette = [
  '#1f77b4',
  '#ff7f0e',
  '#2ca02c',
  '#d62728',
  '#9467bd',
  '#8c564b',
  '#e377c2',
  '#7f7f7f',
]

type AdcSeriesOption = {
  key: string
  label: string
  color: string
  extract: (row: MeasurementPoint) => number | null
}

const adcSeriesOptions = computed<AdcSeriesOption[]>(() => [
  { key: 'adc1', label: adcLabelMap.value.adc1 || 'ADC1', color: '#1f77b4', extract: (row) => row.adc1 ?? null },
  { key: 'adc2', label: adcLabelMap.value.adc2 || 'ADC2', color: '#2ca02c', extract: (row) => row.adc2 ?? null },
  { key: 'adc3', label: adcLabelMap.value.adc3 || 'ADC3', color: '#d62728', extract: (row) => row.adc3 ?? null },
  { key: 'adc1_cal', label: adcLabelMap.value.adc1_cal || 'ADC1 Cal', color: '#9ecae1', extract: (row) => row.adc1_cal ?? null },
  { key: 'adc2_cal', label: adcLabelMap.value.adc2_cal || 'ADC2 Cal', color: '#98df8a', extract: (row) => row.adc2_cal ?? null },
  { key: 'adc3_cal', label: adcLabelMap.value.adc3_cal || 'ADC3 Cal', color: '#ff9896', extract: (row) => row.adc3_cal ?? null },
])

const normalizeHistorySelection = (indices: number[], count: number) => {
  const normalized = normalizeIndices(indices, count)
  return normalized.length ? normalized : (count ? [0] : [])
}

const historyLabels = computed(() => historyData.value.map((row) => formatTimestamp(row.timestamp)))

const buildDataset = (label: string, data: (number | null)[], color: string) => ({
  label,
  data,
  borderColor: color,
  backgroundColor: color,
  borderWidth: 2,
  tension: 0.25,
  pointRadius: 0,
})

const buildTempDatasets = () => {
  const labels = historyTempOptions.value.map((entry) => entry.label)
  const selected = normalizeHistorySelection(historySelection.tempIndices, labels.length)
  return selected.map((idx, seriesIdx) => {
    const color = palette[seriesIdx % palette.length]
    const data = historyData.value.map((row) => {
      const value = row.temps?.[idx]
      return Number.isFinite(value) ? Number(value) : null
    })
    const label = labels[idx] || `t${idx + 1}`
    return buildDataset(label, data, color)
  })
}

const buildAdcDatasets = () => {
  return adcSeriesOptions.value
    .filter((series) => historySelection.adcSeries.includes(series.key))
    .map((series) => buildDataset(series.label, historyData.value.map((row) => series.extract(row)), series.color))
}

const buildBrightnessDatasets = () => {
  const options: AdcSeriesOption[] = [
    {
      key: 'brightness_temp1',
      label: historyBrightnessLabels.value.brightness_temp1 || `${adcLabelMap.value.adc1 || 'ADC1'} Tk`,
      color: '#16a085',
      extract: (row) => row.brightness_temp1 ?? null,
    },
    {
      key: 'brightness_temp2',
      label: historyBrightnessLabels.value.brightness_temp2 || `${adcLabelMap.value.adc2 || 'ADC2'} Tk`,
      color: '#c0392b',
      extract: (row) => row.brightness_temp2 ?? null,
    },
    {
      key: 'brightness_temp3',
      label: historyBrightnessLabels.value.brightness_temp3 || `${adcLabelMap.value.adc3 || 'ADC3'} Tk`,
      color: '#8e44ad',
      extract: (row) => row.brightness_temp3 ?? null,
    },
  ]
  return options
    .map((series) => buildDataset(series.label, historyData.value.map((row) => series.extract(row)), series.color))
    .filter((dataset) => dataset.data.some((value) => Number.isFinite(value)))
}

const loadTempAddress = computed(() => historyTempBindings.value.calibration_load || deviceConfig.value?.temp_bindings?.calibration_load || '')
const loadTempIndex = computed(() => {
  const address = loadTempAddress.value
  if (!address) return -1
  const addresses = historyTempAddresses.value.length ? historyTempAddresses.value : (deviceConfig.value?.temp_addresses || [])
  return addresses.findIndex((item) => item === address)
})
const loadTempLabel = computed(() => {
  const idx = loadTempIndex.value
  if (idx < 0) return 'T нагрузки, K'
  const label = historyTempLabels.value[idx] || deviceConfig.value?.temp_labels?.[idx] || `t${idx + 1}`
  return `${label}, K`
})

const buildLoadCheckDatasets = () => {
  const calOptions: AdcSeriesOption[] = [
    {
      key: 'cal_brightness_temp1',
      label: `${adcLabelMap.value.adc1 || 'ADC1'} Tk по Cal`,
      color: '#16a085',
      extract: (row) => row.cal_brightness_temp1 ?? null,
    },
    {
      key: 'cal_brightness_temp2',
      label: `${adcLabelMap.value.adc2 || 'ADC2'} Tk по Cal`,
      color: '#c0392b',
      extract: (row) => row.cal_brightness_temp2 ?? null,
    },
    {
      key: 'cal_brightness_temp3',
      label: `${adcLabelMap.value.adc3 || 'ADC3'} Tk по Cal`,
      color: '#8e44ad',
      extract: (row) => row.cal_brightness_temp3 ?? null,
    },
  ]
  const datasets = calOptions
    .map((series) => buildDataset(series.label, historyData.value.map((row) => series.extract(row)), series.color))
    .filter((dataset) => dataset.data.some((value) => Number.isFinite(value)))
  const idx = loadTempIndex.value
  if (idx >= 0) {
    const data = historyData.value.map((row) => {
      const value = row.temps?.[idx]
      return Number.isFinite(value) ? Number(value) + 273.15 : null
    })
    const loadDataset = buildDataset(loadTempLabel.value, data, '#111827')
    loadDataset.borderWidth = 3
    loadDataset.borderDash = [6, 4]
    datasets.unshift(loadDataset)
  }
  return datasets
}

const timestampMs = (value: string) => {
  const dt = new Date(value)
  return Number.isNaN(dt.getTime()) ? null : dt.getTime()
}

const buildXYDataset = (label: string, data: { x: number; y: number }[], color: string) => ({
  label,
  data,
  borderColor: color,
  backgroundColor: color,
  borderWidth: 2,
  tension: 0.2,
  pointRadius: 1.5,
})

const buildTeffDatasets = () => {
  const response = atmosphereData.value
  if (!response) return []
  const byStation = new Map<string, { x: number; y: number }[]>()
  response.t_eff_points.forEach((point) => {
    if (!Number.isFinite(point.t_eff)) return
    const x = timestampMs(point.sounding_time)
    if (x === null) return
    if (!byStation.has(point.station_id)) byStation.set(point.station_id, [])
    byStation.get(point.station_id)!.push({ x, y: Number(point.t_eff) })
  })
  return Array.from(byStation.entries()).map(([stationId, data], idx) =>
    buildXYDataset(response.station_labels[stationId] || stationId, data, palette[idx % palette.length])
  )
}

const atmosphereAdcSeries = computed(() => [
  { key: '1', label: adcLabelMap.value.adc1 || 'ADC1', color: '#16a085' },
  { key: '2', label: adcLabelMap.value.adc2 || 'ADC2', color: '#c0392b' },
  { key: '3', label: adcLabelMap.value.adc3 || 'ADC3', color: '#8e44ad' },
])

const buildTauDatasets = () => {
  const points = atmosphereData.value?.measurement_points || []
  return atmosphereAdcSeries.value
    .map((series) => {
      const key = `tau${series.key}` as keyof AtmosphereMeasurementPoint
      const data = points
        .map((point) => {
          const x = timestampMs(point.timestamp)
          const y = point[key]
          return x !== null && Number.isFinite(y) ? { x, y: Number(y) } : null
        })
        .filter(Boolean) as { x: number; y: number }[]
      return buildXYDataset(`${series.label} tau`, data, series.color)
    })
    .filter((dataset) => dataset.data.length > 0)
}

const buildPwvAtmosphereDatasets = () => {
  const response = atmosphereData.value
  if (!response) return []
  const radiometerDatasets = atmosphereAdcSeries.value
    .map((series) => {
      const key = `pwv${series.key}` as keyof AtmosphereMeasurementPoint
      const data = response.measurement_points
        .map((point) => {
          const x = timestampMs(point.timestamp)
          const y = point[key]
          return x !== null && Number.isFinite(y) ? { x, y: Number(y) } : null
        })
        .filter(Boolean) as { x: number; y: number }[]
      return buildXYDataset(`${series.label} PWV`, data, series.color)
    })
    .filter((dataset) => dataset.data.length > 0)
  const byStation = new Map<string, { x: number; y: number }[]>()
  response.t_eff_points.forEach((point) => {
    if (!Number.isFinite(point.pwv_profile)) return
    const x = timestampMs(point.sounding_time)
    if (x === null) return
    if (!byStation.has(point.station_id)) byStation.set(point.station_id, [])
    byStation.get(point.station_id)!.push({ x, y: Number(point.pwv_profile) })
  })
  const profileDatasets = Array.from(byStation.entries()).map(([stationId, data], idx) => {
    const dataset = buildXYDataset(`Профиль ${response.station_labels[stationId] || stationId}`, data, palette[(idx + 3) % palette.length])
    dataset.borderDash = [6, 4]
    dataset.pointRadius = 2
    return dataset
  })
  return [...radiometerDatasets, ...profileDatasets]
}

const atmosphereChartOptions = () => ({
  responsive: true,
  maintainAspectRatio: false,
  parsing: false,
  interaction: { mode: 'nearest', intersect: false },
  plugins: {
    legend: {
      position: 'top',
      align: 'start',
      labels: { boxWidth: 10, boxHeight: 10, padding: 12, usePointStyle: true },
    },
  },
  layout: { padding: { top: 4, right: 8, bottom: 4, left: 4 } },
  scales: {
    x: {
      type: 'linear',
      ticks: {
        maxTicksLimit: 6,
        callback: (value: any) => formatTimestamp(new Date(Number(value)).toISOString()),
      },
      grid: { display: false },
    },
    y: { ticks: { maxTicksLimit: 6 } },
  },
})

const renderCharts = () => {
  if (!ChartCtor) return
  if (
    !tempChartEl.value ||
    !adcChartEl.value ||
    !brightnessChartEl.value ||
    !loadCheckChartEl.value ||
    !teffChartEl.value ||
    !tauChartEl.value ||
    !pwvAtmosphereChartEl.value
  ) return

  const labels = historyLabels.value
  const tempDatasets = buildTempDatasets()
  const adcDatasets = buildAdcDatasets()
  const brightnessDatasets = buildBrightnessDatasets()
  const loadCheckDatasets = buildLoadCheckDatasets()
  const teffDatasets = buildTeffDatasets()
  const tauDatasets = buildTauDatasets()
  const pwvDatasets = buildPwvAtmosphereDatasets()
  const tempHidden = new Map<string, boolean>()
  const adcHidden = new Map<string, boolean>()
  const brightnessHidden = new Map<string, boolean>()
  const loadCheckHidden = new Map<string, boolean>()
  if (tempChart?.data?.datasets) {
    tempChart.data.datasets.forEach((dataset: any) => {
      if (dataset?.label) tempHidden.set(dataset.label, !!dataset.hidden)
    })
  }
  if (adcChart?.data?.datasets) {
    adcChart.data.datasets.forEach((dataset: any) => {
      if (dataset?.label) adcHidden.set(dataset.label, !!dataset.hidden)
    })
  }
  if (brightnessChart?.data?.datasets) {
    brightnessChart.data.datasets.forEach((dataset: any) => {
      if (dataset?.label) brightnessHidden.set(dataset.label, !!dataset.hidden)
    })
  }
  if (loadCheckChart?.data?.datasets) {
    loadCheckChart.data.datasets.forEach((dataset: any) => {
      if (dataset?.label) loadCheckHidden.set(dataset.label, !!dataset.hidden)
    })
  }
  tempDatasets.forEach((dataset) => {
    if (tempHidden.has(dataset.label)) dataset.hidden = tempHidden.get(dataset.label)
  })
  adcDatasets.forEach((dataset) => {
    if (adcHidden.has(dataset.label)) dataset.hidden = adcHidden.get(dataset.label)
  })
  brightnessDatasets.forEach((dataset) => {
    if (brightnessHidden.has(dataset.label)) dataset.hidden = brightnessHidden.get(dataset.label)
  })
  loadCheckDatasets.forEach((dataset) => {
    if (loadCheckHidden.has(dataset.label)) dataset.hidden = loadCheckHidden.get(dataset.label)
  })

  if (!tempChart) {
    tempChart = new ChartCtor(tempChartEl.value, {
      type: 'line',
      data: { labels, datasets: tempDatasets },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: {
            position: 'top',
            align: 'start',
            labels: { boxWidth: 10, boxHeight: 10, padding: 12, usePointStyle: true },
          },
        },
        layout: { padding: { top: 4, right: 8, bottom: 4, left: 4 } },
        scales: {
          x: { ticks: { maxTicksLimit: 6, autoSkip: true, maxRotation: 0, minRotation: 0 }, grid: { display: false } },
          y: { ticks: { maxTicksLimit: 6 } },
        },
      },
    })
  } else {
    tempChart.data.labels = labels
    tempChart.data.datasets = tempDatasets
    tempChart.update('none')
  }

  if (!adcChart) {
    adcChart = new ChartCtor(adcChartEl.value, {
      type: 'line',
      data: { labels, datasets: adcDatasets },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: {
            position: 'top',
            align: 'start',
            labels: { boxWidth: 10, boxHeight: 10, padding: 12, usePointStyle: true },
          },
        },
        layout: { padding: { top: 4, right: 8, bottom: 4, left: 4 } },
        scales: {
          x: { ticks: { maxTicksLimit: 6, autoSkip: true, maxRotation: 0, minRotation: 0 }, grid: { display: false } },
          y: { ticks: { maxTicksLimit: 6 } },
        },
      },
    })
  } else {
    adcChart.data.labels = labels
    adcChart.data.datasets = adcDatasets
    adcChart.update('none')
  }

  if (!brightnessChart) {
    brightnessChart = new ChartCtor(brightnessChartEl.value, {
      type: 'line',
      data: { labels, datasets: brightnessDatasets },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: {
            position: 'top',
            align: 'start',
            labels: { boxWidth: 10, boxHeight: 10, padding: 12, usePointStyle: true },
          },
        },
        layout: { padding: { top: 4, right: 8, bottom: 4, left: 4 } },
        scales: {
          x: { ticks: { maxTicksLimit: 6, autoSkip: true, maxRotation: 0, minRotation: 0 }, grid: { display: false } },
          y: { ticks: { maxTicksLimit: 6 } },
        },
      },
    })
  } else {
    brightnessChart.data.labels = labels
    brightnessChart.data.datasets = brightnessDatasets
    brightnessChart.update('none')
  }

  if (!loadCheckChart) {
    loadCheckChart = new ChartCtor(loadCheckChartEl.value, {
      type: 'line',
      data: { labels, datasets: loadCheckDatasets },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: {
            position: 'top',
            align: 'start',
            labels: { boxWidth: 10, boxHeight: 10, padding: 12, usePointStyle: true },
          },
        },
        layout: { padding: { top: 4, right: 8, bottom: 4, left: 4 } },
        scales: {
          x: { ticks: { maxTicksLimit: 6, autoSkip: true, maxRotation: 0, minRotation: 0 }, grid: { display: false } },
          y: { ticks: { maxTicksLimit: 6 } },
        },
      },
    })
  } else {
    loadCheckChart.data.labels = labels
    loadCheckChart.data.datasets = loadCheckDatasets
    loadCheckChart.update('none')
  }

  if (!teffChart) {
    teffChart = new ChartCtor(teffChartEl.value, {
      type: 'line',
      data: { datasets: teffDatasets },
      options: atmosphereChartOptions(),
    })
  } else {
    teffChart.data.datasets = teffDatasets
    teffChart.update('none')
  }

  if (!tauChart) {
    tauChart = new ChartCtor(tauChartEl.value, {
      type: 'line',
      data: { datasets: tauDatasets },
      options: atmosphereChartOptions(),
    })
  } else {
    tauChart.data.datasets = tauDatasets
    tauChart.update('none')
  }

  if (!pwvAtmosphereChart) {
    pwvAtmosphereChart = new ChartCtor(pwvAtmosphereChartEl.value, {
      type: 'line',
      data: { datasets: pwvDatasets },
      options: atmosphereChartOptions(),
    })
  } else {
    pwvAtmosphereChart.data.datasets = pwvDatasets
    pwvAtmosphereChart.update('none')
  }
}

const setActiveTab = (tab: DeviceTab) => {
  activeTab.value = tab
}

const updateGpsMode = (value: string) => {
  gpsForm.mode = value
  gpsDirty.value = true
}

const updateGpsRtcmText = (value: string) => {
  gpsForm.rtcmText = value
  gpsDirty.value = true
}

const seedGpsForm = () => {
  const liveTypes = gpsLiveTypes.value
  const types = liveTypes.length ? liveTypes : gpsConfig.value?.rtcm_types?.length ? gpsConfig.value.rtcm_types : [1004, 1006, 1033]
  gpsForm.rtcmText = types.join(',')
  gpsForm.mode = String(device.value?.state?.gpsMode || gpsConfig.value?.mode || 'base_time_60')
  gpsDirty.value = false
}

const parseGpsRtcmTypes = (text: string) => {
  const unique = new Set<number>()
  text
    .split(/[,\s;]+/)
    .map((part) => Number(part.trim()))
    .filter((value) => Number.isInteger(value) && value > 0 && value <= 4095)
    .forEach((value) => unique.add(value))
  return Array.from(unique).sort((a, b) => a - b)
}

const loadDeviceGpsConfig = async () => {
  if (!deviceId.value) return
  try {
    const res = await apiFetch<DeviceGpsConfig>(`/api/devices/${deviceId.value}/gps`)
    gpsConfig.value = res
    gpsStatus.value = ''
    if (!gpsDirty.value) {
      seedGpsForm()
    }
  } catch (e: any) {
    gpsStatus.value = e?.message || 'Не удалось загрузить GPS config'
  }
}

const saveGpsConfig = async () => {
  if (!deviceId.value) return
  const rtcmTypes = parseGpsRtcmTypes(gpsForm.rtcmText)
  if (rtcmTypes.length === 0) {
    gpsStatus.value = 'Укажи хотя бы один RTCM код'
    return
  }
  gpsSaving.value = true
  gpsStatus.value = 'Сохраняю GPS config...'
  try {
    const saved = await apiFetch<DeviceGpsConfig>(`/api/devices/${deviceId.value}/gps`, {
      method: 'PATCH',
      body: {
        has_gps: true,
        rtcm_types: rtcmTypes,
        mode: gpsForm.mode,
      },
    })
    gpsConfig.value = saved
    await store.gpsApply(nuxtApp.$mqtt, deviceId.value, {
      mode: gpsForm.mode,
      rtcmTypes,
    })
    gpsDirty.value = false
    gpsStatus.value = 'GPS config сохранен, команда отправлена'
  } catch (e: any) {
    gpsStatus.value = e?.message || 'Не удалось применить GPS config'
  } finally {
    gpsSaving.value = false
  }
}

const probeGpsMode = async () => {
  if (!deviceId.value) return
  gpsSaving.value = true
  gpsStatus.value = 'Запрашиваю MODE...'
  try {
    await store.gpsProbe(nuxtApp.$mqtt, deviceId.value)
    gpsStatus.value = 'Команда MODE отправлена'
  } catch (e: any) {
    gpsStatus.value = e?.message || 'Не удалось запросить MODE'
  } finally {
    gpsSaving.value = false
  }
}

watch(
  () => device.value?.state,
  (state) => {
    if (!state) return
    if (!pidDirty.value) {
      if (Number.isFinite(state.pidSetpoint)) pidForm.setpoint = Number(state.pidSetpoint)
      const mask = Number(state.pidSensorMask)
      if (Number.isFinite(mask) && mask > 0) {
        pidForm.sensorIndices = normalizeIndices(maskToIndices(mask, tempEntries.value.length), tempEntries.value.length)
      } else if (Number.isFinite(state.pidSensorIndex) && tempEntries.value.length > 0) {
        pidForm.sensorIndices = normalizeIndices([Number(state.pidSensorIndex)], tempEntries.value.length)
      }
      if (Number.isFinite(state.pidKp)) pidForm.kp = Number(state.pidKp)
      if (Number.isFinite(state.pidKi)) pidForm.ki = Number(state.pidKi)
      if (Number.isFinite(state.pidKd)) pidForm.kd = Number(state.pidKd)
    }
    if (!wifiDirty.value) {
      wifiForm.mode = state.wifiMode || (state.wifiApMode ? 'ap' : 'sta')
      wifiForm.ssid = state.wifiSsid || ''
    }
    if (!netDirty.value) {
      netForm.mode = state.netMode === 'eth' || state.netMode === 'both' || state.netMode === 'wifi' ? state.netMode : 'wifi'
      netForm.priority = state.netPriority === 'eth' ? 'eth' : 'wifi'
    }
    if (!heaterEditing.value && Number.isFinite(state.heaterPower)) {
      heaterPower.value = Number(state.heaterPower)
    }
    if (!gpsDirty.value) {
      seedGpsForm()
    }
  },
  { immediate: true }
)

watch(
  () => historyTempOptions.value.length,
  (count) => {
    historySelection.tempIndices = normalizeHistorySelection(historySelection.tempIndices, count)
  }
)

watch(
  () => [historyAutoRefresh.value, historyRefreshSec.value],
  () => {
    if (historyAutoRefresh.value) {
      startHistoryTimer()
      loadHistory()
    } else {
      stopHistoryTimer()
    }
  }
)

watch(
  () => [
    historyData.value,
    historySelection.tempIndices,
    historySelection.adcSeries,
    historyTempOptions.value.map((item) => item.label),
    historyTempAddresses.value,
    historyTempBindings.value,
    adcSeriesOptions.value.map((item) => item.label),
    historyBrightnessLabels.value,
    loadTempIndex.value,
    atmosphereData.value,
  ],
  () => {
    renderCharts()
  },
  { deep: true }
)

watch(
  () => [atmospherePrimaryStation.value, atmosphereAverage.value],
  () => {
    if (!deviceConfig.value?.atmosphere_config?.station_ids?.length) return
    if (!historyFilters.from) return
    loadAtmosphere()
  }
)

async function refreshState() {
  if (!deviceId.value) return
  try {
    await store.getState(nuxtApp.$mqtt, deviceId.value)
  } catch (e) {
    console.error(e)
  }
}

async function startLog() {
  if (!deviceId.value) return
  logStatus.value = 'Запускаю лог...'
  try {
    await store.logStart(nuxtApp.$mqtt, deviceId.value, { ...log })
    logStatus.value = 'Лог запущен'
  } catch (e: any) {
    logStatus.value = e?.message || 'Не удалось запустить лог'
  }
}
async function stopLog() {
  if (!deviceId.value) return
  logStatus.value = 'Останавливаю лог...'
  try {
    await store.logStop(nuxtApp.$mqtt, deviceId.value)
    logStatus.value = 'Лог остановлен'
  } catch (e: any) {
    logStatus.value = e?.message || 'Не удалось остановить лог'
  }
}

async function restartDevice() {
  if (!deviceId.value) return
  const confirmed = window.confirm('Перезагрузить устройство? Текущая запись может прерваться.')
  if (!confirmed) return
  restartStatus.value = ''
  restarting.value = true
  try {
    await store.restartDevice(nuxtApp.$mqtt, deviceId.value)
    restartStatus.value = 'Команда на перезагрузку отправлена'
  } catch (e: any) {
    restartStatus.value = e?.message || 'Не удалось отправить команду'
  } finally {
    restarting.value = false
  }
}
async function externalPowerSet(enabled: boolean) {
  if (!deviceId.value) return
  const confirmed = enabled || window.confirm('Выключить питание внешних модулей?')
  if (!confirmed) return
  externalPowerBusy.value = true
  externalPowerStatus.value = enabled ? 'Включаю EXT_PWR_ON...' : 'Выключаю EXT_PWR_ON...'
  try {
    await store.externalPowerSet(nuxtApp.$mqtt, deviceId.value, enabled)
    externalPowerStatus.value = enabled ? 'Внешнее питание включено' : 'Внешнее питание выключено'
  } catch (e: any) {
    externalPowerStatus.value = e?.message || 'Не удалось изменить питание внешних модулей'
  } finally {
    externalPowerBusy.value = false
  }
}
async function externalPowerCycle() {
  if (!deviceId.value) return
  const offMs = Number(externalPowerOffMs.value) || 1000
  const confirmed = window.confirm(`Передернуть питание внешних модулей? OFF на ${offMs} мс.`)
  if (!confirmed) return
  externalPowerBusy.value = true
  externalPowerStatus.value = 'Передергиваю EXT_PWR_ON...'
  try {
    await store.externalPowerCycle(nuxtApp.$mqtt, deviceId.value, offMs)
    externalPowerStatus.value = 'Команда передергивания питания отправлена'
  } catch (e: any) {
    externalPowerStatus.value = e?.message || 'Не удалось передернуть питание'
  } finally {
    externalPowerBusy.value = false
  }
}
async function syncConfigInternalFlash() {
  if (!deviceId.value) return
  configSyncBusy.value = true
  configSyncStatus.value = 'Синхронизирую config.txt во внутреннюю flash ESP...'
  try {
    await store.configSyncInternalFlash(nuxtApp.$mqtt, deviceId.value)
    configSyncStatus.value = 'Конфиг сохранен во внутреннюю flash ESP'
  } catch (e: any) {
    configSyncStatus.value = e?.message || 'Не удалось синхронизировать конфиг во flash ESP'
  } finally {
    configSyncBusy.value = false
  }
}
async function stepperEnable() {
  if (!deviceId.value) return
  stepperStatus.value = 'Включаю мотор...'
  try {
    await store.stepperEnable(nuxtApp.$mqtt, deviceId.value)
    stepperStatus.value = 'Мотор включен'
  } catch (e: any) {
    stepperStatus.value = e?.message || 'Не удалось включить мотор'
  }
}
async function stepperDisable() {
  if (!deviceId.value) return
  stepperStatus.value = 'Выключаю мотор...'
  try {
    await store.stepperDisable(nuxtApp.$mqtt, deviceId.value)
    stepperStatus.value = 'Мотор выключен'
  } catch (e: any) {
    stepperStatus.value = e?.message || 'Не удалось выключить мотор'
  }
}
async function stepperFindZero() {
  if (!deviceId.value) return
  stepperStatus.value = 'Ищу Hall и применяю offset...'
  try {
    await store.stepperFindZero(nuxtApp.$mqtt, deviceId.value)
    stepperStatus.value = 'Поиск запущен'
  } catch (e: any) {
    stepperStatus.value = e?.message || 'Не удалось запустить поиск'
  }
}
async function stepperZero() {
  if (!deviceId.value) return
  stepperStatus.value = 'Устанавливаю 0...'
  try {
    await store.stepperZero(nuxtApp.$mqtt, deviceId.value)
    stepperStatus.value = 'Ноль установлен'
  } catch (e: any) {
    stepperStatus.value = e?.message || 'Не удалось установить 0'
  }
}
async function stepperMove() {
  if (!deviceId.value) return
  stepperStatus.value = 'Отправляю движение...'
  try {
    await store.stepperMove(nuxtApp.$mqtt, deviceId.value, { ...stepper })
    stepperStatus.value = 'Движение запущено'
  } catch (e: any) {
    stepperStatus.value = e?.message || 'Не удалось запустить движение'
  }
}
async function setHeater() {
  if (!deviceId.value) return
  pidApplyStatus.value = 'Устанавливаю нагрев...'
  try {
    await store.heaterSet(nuxtApp.$mqtt, deviceId.value, heaterPower.value)
    heaterEditing.value = false
    pidApplyStatus.value = 'Нагрев установлен'
  } catch (e: any) {
    pidApplyStatus.value = e?.message || 'Не удалось установить нагрев'
  }
}
function togglePidSensor(idx: number) {
  pidDirty.value = true
  if (pidForm.sensorIndices.includes(idx)) {
    pidForm.sensorIndices = pidForm.sensorIndices.filter((value) => value !== idx)
    return
  }
  pidForm.sensorIndices = normalizeIndices([...pidForm.sensorIndices, idx], tempEntries.value.length)
}
async function applyPid() {
  if (!deviceId.value) return
  const count = tempEntries.value.length
  const selected = normalizeIndices(pidForm.sensorIndices, count)
  pidForm.sensorIndices = selected
  if (count === 0) {
    pidApplyStatus.value = 'Нет доступных датчиков'
    return
  }
  if (selected.length === 0) {
    pidApplyStatus.value = 'Выберите хотя бы один датчик'
    return
  }
  pidApplyStatus.value = 'Сохраняю PID...'
  try {
    await store.pidApply(nuxtApp.$mqtt, deviceId.value, {
      setpoint: pidForm.setpoint,
      sensors: selected,
      kp: pidForm.kp,
      ki: pidForm.ki,
      kd: pidForm.kd,
    })
    pidDirty.value = false
    pidApplyStatus.value = 'PID сохранен'
  } catch (e: any) {
    pidApplyStatus.value = e?.message || 'Ошибка сохранения PID'
  }
}
async function enablePid() {
  if (!deviceId.value) return
  pidApplyStatus.value = 'Включаю PID...'
  try {
    await store.pidEnable(nuxtApp.$mqtt, deviceId.value)
    pidApplyStatus.value = 'PID включен'
  } catch (e: any) {
    pidApplyStatus.value = e?.message || 'Ошибка включения PID'
  }
}
async function disablePid() {
  if (!deviceId.value) return
  pidApplyStatus.value = 'Выключаю PID...'
  try {
    await store.pidDisable(nuxtApp.$mqtt, deviceId.value)
    pidApplyStatus.value = 'PID выключен'
  } catch (e: any) {
    pidApplyStatus.value = e?.message || 'Ошибка выключения PID'
  }
}
async function applyWifi() {
  if (!deviceId.value) return
  wifiApplyStatus.value = 'Применяю Wi‑Fi...'
  try {
    await store.wifiApply(nuxtApp.$mqtt, deviceId.value, {
      mode: wifiForm.mode,
      ssid: wifiForm.ssid,
      password: wifiForm.password,
    })
    wifiDirty.value = false
    wifiApplyStatus.value = 'Wi‑Fi обновлен (устройство может переподключиться)'
  } catch (e: any) {
    wifiApplyStatus.value = e?.message || 'Ошибка применения Wi‑Fi'
  }
}
async function applyNetwork() {
  if (!deviceId.value) return
  netApplyStatus.value = 'Применяю сетевой режим...'
  try {
    await store.netApply(nuxtApp.$mqtt, deviceId.value, {
      mode: netForm.mode,
      priority: netForm.priority,
    })
    netDirty.value = false
    netApplyStatus.value = 'Сетевой режим обновлен (соединение может переподключиться)'
  } catch (e: any) {
    netApplyStatus.value = e?.message || 'Ошибка применения сетевого режима'
  }
}

const severityClass = (severity: string) => {
  const level = severity.toLowerCase()
  if (level === 'critical' || level === 'error') return 'danger'
  if (level === 'warning') return 'warn'
  return 'cool'
}

const formatErrorTime = (timestamp: string, createdAt: string) => {
  const raw = timestamp || createdAt
  const parsed = new Date(raw)
  if (Number.isNaN(parsed.getTime())) return raw
  return parsed.toLocaleString('ru-RU')
}

async function loadErrors() {
  if (!deviceId.value) return
  errorLoading.value = true
  errorStatus.value = ''
  try {
    const params = new URLSearchParams({
      limit: String(errorFilters.limit),
      offset: String(errorOffset.value),
    })
    if (errorFilters.from) {
      const isoFrom = localInputToIso(errorFilters.from)
      if (isoFrom) params.set('from', isoFrom)
    }
    if (errorFilters.to) {
      const isoTo = localInputToIso(errorFilters.to)
      if (isoTo) params.set('to', isoTo)
    }
    if (errorFilters.status && errorFilters.status !== 'all') {
      params.set('status', errorFilters.status)
    }
    if (errorFilters.name.trim()) {
      params.set('name', errorFilters.name.trim())
    }
    const response = await apiFetch<ErrorEventsResponse>(`/api/devices/${deviceId.value}/errors?${params.toString()}`)
    errorEvents.value = response.items
    errorTotal.value = response.total
    const maxPage = Math.max(1, Math.ceil(response.total / errorFilters.limit))
    if (errorFilters.page > maxPage) {
      errorFilters.page = maxPage
    }
  } catch (e: any) {
    errorStatus.value = e?.message || 'Не удалось загрузить ошибки'
  } finally {
    errorLoading.value = false
  }
}

const resetErrorFilters = () => {
  errorFilters.from = ''
  errorFilters.to = ''
  errorFilters.status = 'all'
  errorFilters.name = ''
  errorFilters.limit = 200
  errorFilters.page = 1
  loadErrors()
}

const goToErrorPage = (nextPage: number) => {
  const page = Math.min(Math.max(nextPage, 1), errorPageCount.value)
  if (page === errorFilters.page) return
  errorFilters.page = page
  loadErrors()
}

function toggleHistoryTemp(idx: number) {
  const count = historyTempOptions.value.length
  if (historySelection.tempIndices.includes(idx)) {
    if (historySelection.tempIndices.length <= 1) {
      return
    }
    historySelection.tempIndices = historySelection.tempIndices.filter((value) => value !== idx)
  } else {
    historySelection.tempIndices = normalizeIndices([...historySelection.tempIndices, idx], count)
  }
}

function toggleAdcSeries(key: string) {
  const selected = new Set(historySelection.adcSeries)
  if (selected.has(key)) {
    selected.delete(key)
  } else {
    selected.add(key)
  }
  historySelection.adcSeries = adcSeriesOptions.value.filter((option) => selected.has(option.key)).map((option) => option.key)
}

function toggleAtmosphereStation(stationId: string) {
  const selected = new Set(configForm.atmosphere.stationIds)
  if (selected.has(stationId)) {
    selected.delete(stationId)
  } else {
    selected.add(stationId)
  }
  configForm.atmosphere.stationIds = stationOptions.value
    .map((station) => station.station_id)
    .filter((id) => selected.has(id))
  configDirty.value = true
}

async function loadStationOptions() {
  try {
    const collected: StationOption[] = []
    let offset = 0
    let total = 0
    do {
      const res = await apiFetch<{ items: StationOption[]; total: number }>(`/api/stations?limit=500&offset=${offset}`)
      const items = res.items || []
      collected.push(...items.map((item) => ({ station_id: item.station_id, name: item.name || null })))
      total = res.total || collected.length
      offset += items.length
      if (items.length === 0) break
    } while (collected.length < total)
    stationOptions.value = collected
  } catch (e) {
    stationOptions.value = []
  }
}

async function loadAtmosphere() {
  if (!deviceId.value) return
  if (!historyFilters.from) return
  const selected = deviceConfig.value?.atmosphere_config?.station_ids || []
  if (!selected.length) {
    atmosphereData.value = null
    atmosphereStatus.value = 'Выберите станции профилей в настройках устройства'
    renderCharts()
    return
  }
  try {
    const params = new URLSearchParams({
      limit: String(historyFilters.limit),
      average: atmosphereAverage.value ? 'true' : 'false',
    })
    const primary = atmospherePrimaryStation.value || selected[0]
    if (primary) params.set('tau_station_id', primary)
    if (historyFilters.bucketMode === 'manual') {
      const multiplier = historyFilters.bucketUnit === 'h' ? 3600 : historyFilters.bucketUnit === 'm' ? 60 : 1
      const seconds = Math.max(1, Math.floor(historyFilters.bucketValue * multiplier))
      params.set('bucket_seconds', String(seconds))
    }
    if (historyFilters.from) {
      const isoFrom = localInputToIso(historyFilters.from)
      if (isoFrom) params.set('from', isoFrom)
    }
    if (historyAutoRefresh.value) {
      params.set('to', new Date().toISOString())
    } else if (historyFilters.to) {
      const isoTo = localInputToIso(historyFilters.to)
      if (isoTo) params.set('to', isoTo)
    }
    const response = await apiFetch<AtmosphereResponse>(`/api/devices/${deviceId.value}/atmosphere?${params.toString()}`)
    atmosphereData.value = response
    atmosphereStatus.value = ''
  } catch (e: any) {
    atmosphereStatus.value = e?.data?.detail || e?.message || 'Не удалось рассчитать атмосферные параметры'
    atmosphereData.value = null
  }
}

async function loadHistory() {
  if (!deviceId.value) return
  historyLoading.value = true
  historyStatus.value = ''
  try {
    if (!validateHistoryDate('from')) {
      historyLoading.value = false
      return
    }
    if (!historyAutoRefresh.value) {
      if (!validateHistoryDate('to')) {
        historyLoading.value = false
        return
      }
    }
    const params = new URLSearchParams({
      device_id: deviceId.value,
      limit: String(historyFilters.limit),
    })
    if (historyFilters.bucketMode === 'manual') {
      const multiplier = historyFilters.bucketUnit === 'h' ? 3600 : historyFilters.bucketUnit === 'm' ? 60 : 1
      const seconds = Math.max(1, Math.floor(historyFilters.bucketValue * multiplier))
      params.set('bucket_seconds', String(seconds))
    }
    if (historyFilters.from) {
      const isoFrom = localInputToIso(historyFilters.from)
      if (isoFrom) params.set('from', isoFrom)
    }
    if (historyAutoRefresh.value) {
      params.set('to', new Date().toISOString())
    } else if (historyFilters.to) {
      const isoTo = localInputToIso(historyFilters.to)
      if (isoTo) params.set('to', isoTo)
    }
    const response = await apiFetch<MeasurementsResponse>(`/api/measurements?${params.toString()}`)
    historyData.value = response.points
    historyTempLabels.value = response.temp_labels || []
    historyTempAddresses.value = response.temp_addresses || []
    historyTempBindings.value = response.temp_bindings || {}
    historyBrightnessLabels.value = response.brightness_temp_labels || {}
    const hasKnownTempAddresses = !!deviceConfig.value?.temp_addresses?.some(Boolean)
    if (deviceConfig.value && response.temp_label_map) {
      deviceConfig.value = {
        ...deviceConfig.value,
        temp_label_map: { ...(deviceConfig.value.temp_label_map || {}), ...response.temp_label_map },
      }
    }
    if (response.temp_addresses?.some(Boolean) && deviceConfig.value && !hasKnownTempAddresses) {
      deviceConfig.value = { ...deviceConfig.value, temp_addresses: response.temp_addresses }
      if (!configDirty.value) {
        seedConfigForm()
      }
    }
    historyBucketLabel.value = response.bucket_label
    historyRawCount.value = response.raw_count
    if (response.aggregated) {
      historyStatus.value = `Получено ${response.points.length} из ${response.raw_count} (окно ${response.bucket_label})`
    } else {
      historyStatus.value = `Получено ${response.points.length} точек`
    }
    await loadAtmosphere()
  } catch (e: any) {
    historyStatus.value = e?.message || 'Не удалось загрузить историю'
  } finally {
    historyLoading.value = false
  }
}

async function loadLatestWindow() {
  if (!deviceId.value) return
  try {
    const res = await apiFetch<{ timestamp: string | null }>(`/api/measurements/last?device_id=${deviceId.value}`)
    if (res.timestamp) {
      const end = getHistoryWindowEnd(res.timestamp)
      if (end) {
        setHistoryWindow(end)
        return
      }
    }
    setHistoryWindow(new Date())
    historyStatus.value = 'Нет данных, показываю последние сутки'
  } catch (e) {
    setHistoryWindow(new Date())
  }
}

const startHistoryTimer = () => {
  if (historyTimer) clearInterval(historyTimer)
  historyTimer = setInterval(() => {
    if (historyLoading.value) return
    if (historyDateEditing.from || historyDateEditing.to || historyDateError.value) return
    loadHistory()
  }, Math.max(5, historyRefreshSec.value) * 1000)
}

const stopHistoryTimer = () => {
  if (historyTimer) {
    clearInterval(historyTimer)
    historyTimer = null
  }
}

const seedConfigForm = () => {
  const config = deviceConfig.value
  if (!config) return
  configForm.displayName = config.display_name || ''
  const tempLabels = config.temp_labels || []
  const tempAddresses = config.temp_addresses || []
  const rows: TempConfigRow[] = []
  const live = tempEntries.value
  const labelByAddress = buildTempLabelMap(config)
  const length = Math.max(live.length, tempLabels.length, tempAddresses.length)
  for (let idx = 0; idx < length; idx += 1) {
    const liveEntry = live[idx]
    const address = tempAddresses[idx] || liveEntry?.address || ''
    const label = (address && labelByAddress.get(address)) || tempLabels[idx] || liveEntry?.label || `t${idx + 1}`
    rows.push({ index: idx, address: address || null, label })
  }
  configForm.tempRows = rows
  Object.assign(configForm.adcLabels, adcLabelDefaults, config.adc_labels || {})
  tempBindingRoles.forEach(({ key }) => {
    configForm.tempBindings[key] = (config.temp_bindings || {})[key] || ''
  })
  const atmosphere = config.atmosphere_config || {}
  configForm.atmosphere.stationIds = [...(atmosphere.station_ids || [])]
  configForm.atmosphere.altitudeM = Number.isFinite(Number(atmosphere.altitude_m)) ? Number(atmosphere.altitude_m) : 0
  configForm.atmosphere.h0M = Number.isFinite(Number(atmosphere.h0_m)) ? Number(atmosphere.h0_m) : 5300
  atmospherePrimaryStation.value = String(atmosphere.tau_station_id || configForm.atmosphere.stationIds[0] || '')
  atmosphereAverage.value = !!atmosphere.tau_average
}

const resetConfig = () => {
  configDirty.value = false
  configStatus.value = ''
  seedConfigForm()
}

const loadDeviceConfig = async () => {
  if (!deviceId.value) return
  try {
    const res = await apiFetch<DeviceConfig>(`/api/devices/${deviceId.value}`)
    deviceConfig.value = res
    configStatus.value = ''
    if (!configDirty.value) {
      seedConfigForm()
    }
  } catch (e: any) {
    configStatus.value = e?.message || 'Не удалось загрузить конфигурацию'
  }
}

const saveConfig = async () => {
  if (!deviceId.value) return
  configSaving.value = true
  configStatus.value = 'Сохраняю настройки...'
  try {
    const tempLabels: string[] = []
    const tempAddresses: string[] = []
    const tempLabelMap: Record<string, string> = {}
    configForm.tempRows.forEach((row) => {
      const label = row.label.trim()
      const address = (row.address || '').trim()
      if (row.index !== null) {
        tempLabels[row.index] = label || `t${row.index + 1}`
        tempAddresses[row.index] = address
        if (address) {
          tempLabelMap[address] = tempLabels[row.index]
        }
      }
    })
    for (let i = 0; i < tempLabels.length; i += 1) {
      if (!tempLabels[i]) {
        tempLabels[i] = `t${i + 1}`
      }
    }
    const adcLabels: Record<string, string> = {}
    adcLabelKeys.forEach((key) => {
      const raw = (configForm.adcLabels as Record<string, string>)[key] || ''
      adcLabels[key] = raw.trim() || adcLabelDefaults[key]
    })
    const tempBindings: Record<string, string> = {}
    tempBindingRoles.forEach(({ key }) => {
      const address = (configForm.tempBindings[key] || '').trim()
      if (address) tempBindings[key] = address
    })
    const stationIds = configForm.atmosphere.stationIds.map((id) => id.trim()).filter(Boolean)
    const atmosphereConfig = {
      station_ids: stationIds,
      altitude_m: Math.max(0, Number(configForm.atmosphere.altitudeM) || 0),
      h0_m: Math.max(1, Number(configForm.atmosphere.h0M) || 5300),
      tau_station_id: stationIds.includes(atmospherePrimaryStation.value)
        ? atmospherePrimaryStation.value
        : (stationIds[0] || ''),
      tau_average: atmosphereAverage.value,
    }
    const displayName = configForm.displayName.trim()
    const res = await apiFetch<DeviceConfig>(`/api/devices/${deviceId.value}`, {
      method: 'PATCH',
      body: {
        display_name: displayName ? displayName : null,
        temp_labels: tempLabels,
        temp_addresses: tempAddresses,
        temp_label_map: tempLabelMap,
        temp_bindings: tempBindings,
        atmosphere_config: atmosphereConfig,
        adc_labels: adcLabels,
      },
    })
    deviceConfig.value = res
    configDirty.value = false
    configStatus.value = 'Настройки сохранены'
    if (historyTempLabels.value.length) {
      const nextLabels = [...(res.temp_labels || tempLabels)]
      if (nextLabels.length < historyTempLabels.value.length) {
        for (let i = nextLabels.length; i < historyTempLabels.value.length; i += 1) {
          nextLabels.push(`t${i + 1}`)
        }
      }
      historyTempLabels.value = nextLabels
    }
    seedConfigForm()
    await loadAtmosphere()
  } catch (e: any) {
    configStatus.value = e?.message || 'Не удалось сохранить настройки'
  } finally {
    configSaving.value = false
  }
}

onMounted(() => {
  refreshState()
  loadStationOptions()
  loadDeviceConfig()
  loadDeviceGpsConfig()
  loadLatestWindow().then(loadHistory)
  if (process.client) {
    const tz = Intl.DateTimeFormat().resolvedOptions().timeZone || 'local'
    const offsetMin = new Date().getTimezoneOffset()
    const sign = offsetMin <= 0 ? '+' : '-'
    const abs = Math.abs(offsetMin)
    const hh = String(Math.floor(abs / 60)).padStart(2, '0')
    const mm = String(abs % 60).padStart(2, '0')
    browserTzLabel.value = `${tz} (UTC${sign}${hh}:${mm})`
  }
  if (!ChartCtor) {
    import('chart.js/auto').then((mod: any) => {
      ChartCtor = mod?.Chart || mod?.default || mod
      renderCharts()
    })
  }
})

onBeforeUnmount(() => {
  stopHistoryTimer()
  if (tempChart) {
    tempChart.destroy()
    tempChart = null
  }
  if (adcChart) {
    adcChart.destroy()
    adcChart = null
  }
  if (brightnessChart) {
    brightnessChart.destroy()
    brightnessChart = null
  }
  if (loadCheckChart) {
    loadCheckChart.destroy()
    loadCheckChart = null
  }
  if (teffChart) {
    teffChart.destroy()
    teffChart = null
  }
  if (tauChart) {
    tauChart.destroy()
    tauChart = null
  }
  if (pwvAtmosphereChart) {
    pwvAtmosphereChart.destroy()
    pwvAtmosphereChart = null
  }
})

watch(
  () => activeTab.value,
  (value) => {
    const current = Array.isArray(route.query.tab) ? route.query.tab[0] : route.query.tab
    if (current !== value) {
      navigateTo({ path: route.path, query: { ...route.query, tab: value } }, { replace: true })
    }
    if (value === 'errors') {
      loadErrors()
    }
  }
)

watch(
  () => route.query.tab,
  () => {
    const next = tabFromQuery()
    if (next !== activeTab.value) {
      activeTab.value = next
    }
  }
)

watch(
  () => stepperHomeStatus.value,
  (value) => {
    if (value === 'not_found') {
      stepperStatus.value = 'Дом не найден'
    } else if (value === 'aborted') {
      stepperStatus.value = 'Поиск остановлен'
    } else if (value === 'ok' || value === 'hall_zero') {
      stepperStatus.value = 'Дом найден'
    } else if (value === 'offset_zero') {
      stepperStatus.value = 'Ноль с offset найден'
    } else if (value === 'seeking_hall') {
      stepperStatus.value = 'Ищу Hall...'
    } else if (value === 'applying_offset') {
      stepperStatus.value = 'Применяю offset...'
    }
  }
)

watch(
  () => deviceId.value,
  () => {
    errorEvents.value = []
    errorStatus.value = ''
    gpsConfig.value = null
    gpsStatus.value = ''
    gpsDirty.value = false
    loadDeviceConfig()
    loadDeviceGpsConfig()
    if (activeTab.value === 'errors') {
      loadErrors()
    }
  }
)

watch(
  () => tempEntries.value.map((entry) => entry.address || ''),
  () => {
    const known = new Set((deviceConfig.value?.temp_addresses || []).filter(Boolean))
    const hasNewLiveAddress = tempEntries.value.some((entry) => entry.address && !known.has(entry.address))
    if (!configDirty.value && deviceConfig.value && hasNewLiveAddress) {
      seedConfigForm()
    }
  }
)
</script>
