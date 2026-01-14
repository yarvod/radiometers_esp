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
      <button class="tab-btn" :class="{ active: activeTab === 'data' }" @click="activeTab = 'data'">Данные</button>
      <button class="tab-btn" :class="{ active: activeTab === 'control' }" @click="activeTab = 'control'">Мониторинг и управление</button>
      <button class="tab-btn" :class="{ active: activeTab === 'settings' }" @click="activeTab = 'settings'">Настройки</button>
      <button class="tab-btn" :class="{ active: activeTab === 'errors' }" @click="activeTab = 'errors'">Ошибки</button>
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
        <div class="actions">
          <button class="btn danger" @click="restartDevice" :disabled="restarting">Перезагрузить</button>
        </div>
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
          <button class="btn primary" @click="stepperFindZero">Найти дом (Hall)</button>
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
          <span class="chip subtle">RSSI: {{ device?.state?.wifiRssi ?? '--' }} dBm</span>
          <span class="chip subtle">Качество: {{ device?.state?.wifiQuality ?? '--' }}%</span>
          <span class="chip subtle">SSID: {{ wifiSsidDisplay }}</span>
        </div>
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
      <div class="actions">
        <button class="btn primary" @click="saveConfig" :disabled="configSaving">Сохранить</button>
        <button class="btn ghost" @click="resetConfig" :disabled="configSaving">Сбросить</button>
      </div>
    </div>

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
}

type MeasurementsResponse = {
  points: MeasurementPoint[]
  raw_count: number
  limit: number
  bucket_seconds: number
  bucket_label: string
  aggregated: boolean
  temp_labels: string[]
  adc_labels: Record<string, string>
  temp_addresses: string[]
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
  adc_labels: Record<string, string>
}

type TempConfigRow = {
  index: number | null
  address: string | null
  label: string
}

const { apiFetch } = useApi()
const route = useRoute()
const deviceId = computed(() => route.params.deviceId as string)
const store = useDevicesStore()
const nuxtApp = useNuxtApp()
store.init(nuxtApp.$mqtt)

const device = computed(() => (deviceId.value ? store.devices.get(deviceId.value) : undefined))
const deviceConfig = ref<DeviceConfig | null>(null)
const activeTab = ref<'data' | 'control' | 'settings' | 'errors'>('control')
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
})
const tempEntries = computed(() => {
  const sensors = device.value?.state?.tempSensors
  if (Array.isArray(sensors)) {
    const byIndex = deviceConfig.value?.temp_labels || []
    return sensors.map((value, idx) => ({
      key: `t${idx + 1}`,
      label: byIndex[idx] || `t${idx + 1}`,
      value,
      address: '',
    }))
  }
  if (!sensors || typeof sensors !== 'object') return []
  const byIndex = deviceConfig.value?.temp_labels || []
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
      const renamed = byIndex[idx] || label
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

const log = reactive({ filename: 'data', useMotor: false, durationSec: 1 })
const stepper = reactive({ steps: 400, speedUs: 1500, reverse: false })
const stepperStatus = ref('')
const heaterPower = ref(0)
const heaterEditing = ref(false)
const pidDirty = ref(false)
const wifiDirty = ref(false)
const pidApplyStatus = ref('')
const wifiApplyStatus = ref('')
const restartStatus = ref('')
const restarting = ref(false)
const pidForm = reactive({ setpoint: 25, sensorIndices: [] as number[], kp: 1, ki: 0, kd: 0 })
const wifiForm = reactive({ mode: 'sta', ssid: '', password: '' })
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
let tempChart: any = null
let adcChart: any = null
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
      return 'Найден'
    case 'manual_zero':
      return 'Ручной 0'
    case 'not_found':
      return 'Не найден'
    case 'aborted':
      return 'Отменен'
    case 'running':
      return 'Поиск...'
    default:
      return 'Нет'
  }
})
const stepperHomeChipClass = computed(() => {
  if (stepperHomeStatus.value === 'ok' || stepperHomeStatus.value === 'manual_zero') return 'online'
  if (stepperHomeStatus.value === 'running') return 'cool'
  if (stepperHomeStatus.value === 'not_found' || stepperHomeStatus.value === 'aborted') return 'warn'
  return 'subtle'
})
const stepperDirText = computed(() => {
  const dir = device.value?.state?.stepperDirForward
  if (dir === undefined || dir === null) return '--'
  return dir ? 'FWD' : 'REV'
})

const wifiModeDisplay = computed(() => {
  const mode = device.value?.state?.wifiMode
  if (mode === 'ap' || mode === 'sta') return mode
  return device.value?.state?.wifiApMode ? 'ap' : 'sta'
})
const wifiIpDisplay = computed(() => device.value?.state?.wifiIp || '--')
const wifiStaIpDisplay = computed(() => device.value?.state?.wifiStaIp || '--')
const wifiApIpDisplay = computed(() => device.value?.state?.wifiApIp || '--')
const wifiSsidDisplay = computed(() => device.value?.state?.wifiSsid || '--')

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

const renderCharts = () => {
  if (!ChartCtor) return
  if (!tempChartEl.value || !adcChartEl.value) return

  const labels = historyLabels.value
  const tempDatasets = buildTempDatasets()
  const adcDatasets = buildAdcDatasets()
  const tempHidden = new Map<string, boolean>()
  const adcHidden = new Map<string, boolean>()
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
  tempDatasets.forEach((dataset) => {
    if (tempHidden.has(dataset.label)) dataset.hidden = tempHidden.get(dataset.label)
  })
  adcDatasets.forEach((dataset) => {
    if (adcHidden.has(dataset.label)) dataset.hidden = adcHidden.get(dataset.label)
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
    if (!heaterEditing.value && Number.isFinite(state.heaterPower)) {
      heaterPower.value = Number(state.heaterPower)
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
    adcSeriesOptions.value.map((item) => item.label),
  ],
  () => {
    renderCharts()
  },
  { deep: true }
)

async function refreshState() {
  if (!deviceId.value) return
  try {
    await store.getState(nuxtApp.$mqtt, deviceId.value)
  } catch (e) {
    console.error(e)
  }
}

function startLog() {
  if (!deviceId.value) return
  store.logStart(nuxtApp.$mqtt, deviceId.value, { ...log })
}
function stopLog() {
  if (!deviceId.value) return
  store.logStop(nuxtApp.$mqtt, deviceId.value)
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
function stepperEnable() {
  if (!deviceId.value) return
  store.stepperEnable(nuxtApp.$mqtt, deviceId.value)
}
function stepperDisable() {
  if (!deviceId.value) return
  store.stepperDisable(nuxtApp.$mqtt, deviceId.value)
}
async function stepperFindZero() {
  if (!deviceId.value) return
  stepperStatus.value = 'Ищу дом по Hall...'
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
function stepperMove() {
  if (!deviceId.value) return
  store.stepperMove(nuxtApp.$mqtt, deviceId.value, { ...stepper })
}
function setHeater() {
  if (!deviceId.value) return
  store.heaterSet(nuxtApp.$mqtt, deviceId.value, heaterPower.value)
  heaterEditing.value = false
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
  if (historySelection.tempIndices.includes(idx)) {
    if (historySelection.tempIndices.length <= 1) {
      return
    }
    historySelection.tempIndices = historySelection.tempIndices.filter((value) => value !== idx)
  } else {
    historySelection.tempIndices = normalizeIndices([...historySelection.tempIndices, idx], tempEntries.value.length)
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
    if (response.temp_addresses?.length && deviceConfig.value && deviceConfig.value.temp_addresses.length === 0) {
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
  const length = Math.max(live.length, tempLabels.length, tempAddresses.length)
  for (let idx = 0; idx < length; idx += 1) {
    const liveEntry = live[idx]
    const address = tempAddresses[idx] || liveEntry?.address || ''
    const label = tempLabels[idx] || liveEntry?.label || `t${idx + 1}`
    rows.push({ index: idx, address: address || null, label })
  }
  configForm.tempRows = rows
  Object.assign(configForm.adcLabels, adcLabelDefaults, config.adc_labels || {})
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
    configForm.tempRows.forEach((row) => {
      const label = row.label.trim()
      if (row.index !== null) {
        tempLabels[row.index] = label || `t${row.index + 1}`
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
    const displayName = configForm.displayName.trim()
    const res = await apiFetch<DeviceConfig>(`/api/devices/${deviceId.value}`, {
      method: 'PATCH',
      body: {
        display_name: displayName ? displayName : null,
        temp_labels: tempLabels,
        adc_labels: adcLabels,
      },
    })
    deviceConfig.value = res
    configDirty.value = false
    configStatus.value = 'Настройки сохранены'
    if (historyTempLabels.value.length) {
      const nextLabels = [...tempLabels]
      if (nextLabels.length < historyTempLabels.value.length) {
        for (let i = nextLabels.length; i < historyTempLabels.value.length; i += 1) {
          nextLabels.push(`t${i + 1}`)
        }
      }
      historyTempLabels.value = nextLabels
    }
    seedConfigForm()
  } catch (e: any) {
    configStatus.value = e?.message || 'Не удалось сохранить настройки'
  } finally {
    configSaving.value = false
  }
}

onMounted(() => {
  refreshState()
  loadDeviceConfig()
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
})

watch(
  () => activeTab.value,
  (value) => {
    if (value === 'errors') {
      loadErrors()
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
    } else if (value === 'ok') {
      stepperStatus.value = 'Дом найден'
    }
  }
)

watch(
  () => deviceId.value,
  () => {
    errorEvents.value = []
    errorStatus.value = ''
    if (activeTab.value === 'errors') {
      loadErrors()
    }
  }
)

watch(
  () => tempEntries.value.map((entry) => `${entry.address || ''}:${entry.label}`),
  () => {
    if (!configDirty.value && deviceConfig.value) {
      seedConfigForm()
    }
  }
)
</script>
