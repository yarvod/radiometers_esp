<template>
  <div class="card calibration-panel">
    <div class="card-head">
      <h3>Калибровка радиометров</h3>
      <span class="badge">2 точки</span>
    </div>

    <div class="status-row">
      <span class="chip" :class="{ warn: logging, online: !logging }">
        {{ logging ? 'Идет измерение' : 'Измерение остановлено' }}
      </span>
      <span class="chip subtle">ADC1: {{ adcLabel('adc1', 'ADC1') }}</span>
      <span class="chip subtle">ADC2: {{ adcLabel('adc2', 'ADC2') }}</span>
      <span class="chip subtle">ADC3: {{ adcLabel('adc3', 'ADC3') }}</span>
    </div>

    <div class="actions">
      <button class="btn primary" :disabled="logging" @click="openCreate">Создать калибровку</button>
      <button class="btn ghost" @click="loadCalibrations" :disabled="loading">Обновить</button>
      <span class="muted" v-if="logging">Останови штатное измерение перед калибровкой.</span>
      <span class="muted" v-if="status">{{ status }}</span>
    </div>

    <div class="table-wrap">
      <table class="table">
        <thead>
          <tr>
            <th>Дата</th>
            <th>Нагрузки, K</th>
            <th>{{ adcLabel('adc1', 'ADC1') }}</th>
            <th>{{ adcLabel('adc2', 'ADC2') }}</th>
            <th>{{ adcLabel('adc3', 'ADC3') }}</th>
            <th>Темп. радиометров, C</th>
            <th>Комментарий</th>
            <th>Действия</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="!loading && calibrations.length === 0">
            <td colspan="8" class="muted">Калибровок пока нет</td>
          </tr>
          <tr v-for="item in calibrations" :key="item.id">
            <td>{{ formatDate(item.created_at) }}</td>
            <td>{{ fmt(item.t_black_body_1, 2) }} / {{ fmt(item.t_black_body_2, 2) }}</td>
            <td>
              <div>k={{ fmt(item.adc1_slope, 6) }}</div>
              <div>b={{ fmt(item.adc1_intercept, 3) }}</div>
              <div>Tш={{ fmt(item.adc1_noise_temp, 1) }} K</div>
            </td>
            <td>
              <div>k={{ fmt(item.adc2_slope, 6) }}</div>
              <div>b={{ fmt(item.adc2_intercept, 3) }}</div>
              <div>Tш={{ fmt(item.adc2_noise_temp, 1) }} K</div>
            </td>
            <td>
              <div>k={{ fmt(item.adc3_slope, 6) }}</div>
              <div>b={{ fmt(item.adc3_intercept, 3) }}</div>
              <div>Tш={{ fmt(item.adc3_noise_temp, 1) }} K</div>
            </td>
            <td>{{ fmt(item.t_adc1, 1) }} / {{ fmt(item.t_adc2, 1) }} / {{ fmt(item.t_adc3, 1) }}</td>
            <td>{{ item.comment || '—' }}</td>
            <td>
              <div class="actions compact-actions">
                <button class="btn ghost sm" @click="openEdit(item)">Править</button>
                <button class="btn danger sm" @click="deleteCalibration(item)" :disabled="deletingId === item.id">Удалить</button>
              </div>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <div class="actions">
      <button class="btn ghost sm" :disabled="page <= 1 || loading" @click="goPage(page - 1)">←</button>
      <span class="muted">Страница {{ page }} / {{ pageCount }}</span>
      <button class="btn ghost sm" :disabled="page >= pageCount || loading" @click="goPage(page + 1)">→</button>
    </div>

    <div v-if="modalOpen" class="modal-backdrop" @click.self="closeCreate">
      <div class="modal calibration-modal">
        <div class="modal-head">
          <h3>{{ editingId ? 'Редактировать калибровку' : 'Новая калибровка' }}</h3>
          <button class="btn ghost sm" @click="closeCreate" :disabled="isSampling">Закрыть</button>
        </div>

        <div class="inline fields">
          <label class="compact">Время замера, с
            <input type="number" min="1" max="120" step="1" v-model.number="form.durationSec" />
          </label>
          <label class="compact">T {{ adcLabel('adc1', 'ADC1') }}, C
            <input type="number" step="0.1" v-model.number="form.t_adc1" />
          </label>
          <label class="compact">T {{ adcLabel('adc2', 'ADC2') }}, C
            <input type="number" step="0.1" v-model.number="form.t_adc2" />
          </label>
          <label class="compact">T {{ adcLabel('adc3', 'ADC3') }}, C
            <input type="number" step="0.1" v-model.number="form.t_adc3" />
          </label>
          <button class="btn ghost sm" type="button" @click="applyBoundRadiometerTemps">
            Взять T радиометров
          </button>
        </div>
        <div class="status-row">
          <span class="chip subtle">{{ boundTempText('radiometer_adc1', adcLabel('adc1', 'ADC1')) }}</span>
          <span class="chip subtle">{{ boundTempText('radiometer_adc2', adcLabel('adc2', 'ADC2')) }}</span>
          <span class="chip subtle">{{ boundTempText('radiometer_adc3', adcLabel('adc3', 'ADC3')) }}</span>
          <span class="chip subtle">{{ boundLoadTempText(1) }}</span>
          <span class="chip subtle">{{ boundLoadTempText(2) }}</span>
        </div>

        <div class="calibration-steps">
          <div class="calibration-step">
            <div class="card-head compact-head">
              <h4>Нагрузка 1</h4>
              <span class="badge">{{ samples1.length }} samples</span>
            </div>
            <div class="form-group">
              <label>Температура черного тела, K</label>
              <input type="number" step="0.01" v-model.number="form.t_black_body_1" />
            </div>
            <div class="actions">
              <button class="btn ghost" type="button" @click="applyBoundLoadTemp(1)">Взять T нагрузки</button>
              <button class="btn primary" :disabled="logging || isSampling" @click="startSample(1)">Начать</button>
              <button class="btn warning ghost" :disabled="samplingPhase !== 1" @click="stopSample">Стоп</button>
            </div>
            <div class="sample-grid">
              <span>{{ adcLabel('adc1', 'ADC1') }}: {{ fmt(avg1.adc1, 6) }}</span>
              <span>{{ adcLabel('adc2', 'ADC2') }}: {{ fmt(avg1.adc2, 6) }}</span>
              <span>{{ adcLabel('adc3', 'ADC3') }}: {{ fmt(avg1.adc3, 6) }}</span>
            </div>
          </div>

          <div class="calibration-step">
            <div class="card-head compact-head">
              <h4>Нагрузка 2</h4>
              <span class="badge">{{ samples2.length }} samples</span>
            </div>
            <div class="form-group">
              <label>Температура черного тела, K</label>
              <input type="number" step="0.01" v-model.number="form.t_black_body_2" />
            </div>
            <div class="actions">
              <button class="btn ghost" type="button" @click="applyBoundLoadTemp(2)">Взять T нагрузки</button>
              <button class="btn primary" :disabled="logging || isSampling" @click="startSample(2)">Начать</button>
              <button class="btn warning ghost" :disabled="samplingPhase !== 2" @click="stopSample">Стоп</button>
            </div>
            <div class="sample-grid">
              <span>{{ adcLabel('adc1', 'ADC1') }}: {{ fmt(avg2.adc1, 6) }}</span>
              <span>{{ adcLabel('adc2', 'ADC2') }}: {{ fmt(avg2.adc2, 6) }}</span>
              <span>{{ adcLabel('adc3', 'ADC3') }}: {{ fmt(avg2.adc3, 6) }}</span>
            </div>
          </div>
        </div>

        <div class="preview">
          <span class="chip subtle">{{ adcLabel('adc1', 'ADC1') }} k={{ fmt(coefficients.adc1.slope, 6) }} b={{ fmt(coefficients.adc1.intercept, 3) }} Tш={{ fmt(coefficients.adc1.noiseTemp, 1) }} K</span>
          <span class="chip subtle">{{ adcLabel('adc2', 'ADC2') }} k={{ fmt(coefficients.adc2.slope, 6) }} b={{ fmt(coefficients.adc2.intercept, 3) }} Tш={{ fmt(coefficients.adc2.noiseTemp, 1) }} K</span>
          <span class="chip subtle">{{ adcLabel('adc3', 'ADC3') }} k={{ fmt(coefficients.adc3.slope, 6) }} b={{ fmt(coefficients.adc3.intercept, 3) }} Tш={{ fmt(coefficients.adc3.noiseTemp, 1) }} K</span>
        </div>

        <div class="form-group">
          <label>Комментарий</label>
          <textarea v-model="form.comment" rows="3"></textarea>
        </div>

        <div class="actions">
          <button class="btn success" :disabled="saving || isSampling" @click="saveCalibration">
            {{ editingId ? 'Сохранить изменения' : 'Сохранить калибровку' }}
          </button>
          <span class="muted" v-if="!modalStatus && saveBlockReason">{{ saveBlockReason }}</span>
          <span class="muted" v-if="modalStatus">{{ modalStatus }}</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'

type Calibration = {
  id: string
  device_id: string
  created_at: string
  t_black_body_1: number
  t_black_body_2: number
  adc1_1: number | null
  adc2_1: number | null
  adc3_1: number | null
  adc1_2: number | null
  adc2_2: number | null
  adc3_2: number | null
  t_adc1: number
  t_adc2: number
  t_adc3: number
  adc1_slope: number | null
  adc2_slope: number | null
  adc3_slope: number | null
  adc1_intercept: number | null
  adc2_intercept: number | null
  adc3_intercept: number | null
  adc1_noise_temp: number | null
  adc2_noise_temp: number | null
  adc3_noise_temp: number | null
  comment: string | null
}

type CalibrationsResponse = {
  items: Calibration[]
  total: number
  limit: number
  offset: number
}

type AdcSample = {
  adc1: number | null
  adc2: number | null
  adc3: number | null
}

type TempSensorEntry = {
  key: string
  label: string
  value: number | string | null
  address: string
}

const props = defineProps<{
  deviceId: string
  logging: boolean
  adcLabels: Record<string, string>
  tempBindings?: Record<string, string>
  tempSensors?: TempSensorEntry[]
}>()

const { apiFetch } = useApi()
const store = useDevicesStore()
const nuxtApp = useNuxtApp()

const calibrations = ref<Calibration[]>([])
const loading = ref(false)
const saving = ref(false)
const deletingId = ref('')
const status = ref('')
const modalStatus = ref('')
const modalOpen = ref(false)
const editingId = ref('')
const page = ref(1)
const limit = 10
const total = ref(0)
const samplingPhase = ref<1 | 2 | null>(null)
const samples1 = ref<AdcSample[]>([])
const samples2 = ref<AdcSample[]>([])

const form = reactive({
  durationSec: 10,
  t_black_body_1: 77,
  t_black_body_2: 300,
  t_adc1: 25,
  t_adc2: 25,
  t_adc3: 25,
  comment: '',
})

const resetForm = () => {
  form.durationSec = 10
  form.t_black_body_1 = 77
  form.t_black_body_2 = 300
  form.t_adc1 = 25
  form.t_adc2 = 25
  form.t_adc3 = 25
  form.comment = ''
  samples1.value = []
  samples2.value = []
  applyBoundRadiometerTemps()
}

const isSampling = computed(() => samplingPhase.value !== null)
const pageCount = computed(() => Math.max(1, Math.ceil(total.value / limit)))

const adcLabel = (key: string, fallback: string) => props.adcLabels?.[key] || fallback
const tempByAddress = computed(() => {
  const result = new Map<string, TempSensorEntry>()
  ;(props.tempSensors || []).forEach((sensor) => {
    if (sensor.address) result.set(sensor.address, sensor)
  })
  return result
})
const boundTempC = (role: string) => {
  const address = props.tempBindings?.[role] || ''
  const sensor = address ? tempByAddress.value.get(address) : null
  const value = sensor ? Number(sensor.value) : Number.NaN
  return Number.isFinite(value) ? value : null
}
const boundLoadRole = (phase: 1 | 2) => {
  const specific = props.tempBindings?.[`calibration_load_${phase}`]
  return specific ? `calibration_load_${phase}` : 'calibration_load'
}
const boundTempText = (role: string, label: string) => {
  const address = props.tempBindings?.[role] || ''
  if (!address) return `${label}: датчик не задан`
  const sensor = tempByAddress.value.get(address)
  const value = boundTempC(role)
  if (value === null) return `${label}: ${sensor?.label || address} нет данных`
  return `${label}: ${sensor?.label || address} ${value.toFixed(1)} C`
}
const boundLoadTempText = (phase: 1 | 2) => boundTempText(boundLoadRole(phase), `Нагрузка ${phase}`)
const applyBoundRadiometerTemps = () => {
  const adc1 = boundTempC('radiometer_adc1')
  const adc2 = boundTempC('radiometer_adc2')
  const adc3 = boundTempC('radiometer_adc3')
  if (adc1 !== null) form.t_adc1 = Number(adc1.toFixed(1))
  if (adc2 !== null) form.t_adc2 = Number(adc2.toFixed(1))
  if (adc3 !== null) form.t_adc3 = Number(adc3.toFixed(1))
}
const applyBoundLoadTemp = (phase: 1 | 2) => {
  const loadC = boundTempC(boundLoadRole(phase))
  if (loadC === null) {
    modalStatus.value = `Датчик нагрузки ${phase} не задан или нет данных`
    return
  }
  const loadK = Number((loadC + 273.15).toFixed(2))
  if (phase === 1) form.t_black_body_1 = loadK
  if (phase === 2) form.t_black_body_2 = loadK
}

const fmt = (value: number | null | undefined, digits = 3) => {
  if (!Number.isFinite(value)) return '--'
  return Number(value).toFixed(digits)
}

const formatDate = (value: string) => {
  const dt = new Date(value)
  if (Number.isNaN(dt.getTime())) return value
  return dt.toLocaleString('ru-RU')
}

const average = (samples: AdcSample[]) => {
  if (!samples.length) return { adc1: null, adc2: null, adc3: null }
  const avgChannel = (key: keyof AdcSample) => {
    const values = samples
      .map((sample) => sample[key])
      .filter((value): value is number => Number.isFinite(value))
    if (!values.length) return null
    return values.reduce((sum, value) => sum + value, 0) / values.length
  }
  return {
    adc1: avgChannel('adc1'),
    adc2: avgChannel('adc2'),
    adc3: avgChannel('adc3'),
  }
}

const coefficient = (t1: number, t2: number, adcA: number | null, adcB: number | null) => {
  if (!Number.isFinite(t1) || !Number.isFinite(t2) || !Number.isFinite(adcA) || !Number.isFinite(adcB)) {
    return { slope: null, intercept: null, noiseTemp: null }
  }
  const denominator = Number(adcB) - Number(adcA)
  if (Math.abs(denominator) < 1e-12) return { slope: null, intercept: null, noiseTemp: null }
  const slope = (t2 - t1) / denominator
  const intercept = t1 - slope * Number(adcA)
  const hot = t1 >= t2
    ? { temp: t1, adc: Number(adcA), coldTemp: t2, coldAdc: Number(adcB) }
    : { temp: t2, adc: Number(adcB), coldTemp: t1, coldAdc: Number(adcA) }
  const yFactor = Math.abs(hot.coldAdc) >= 1e-12 ? hot.adc / hot.coldAdc : Number.NaN
  const noiseTemp = Number.isFinite(yFactor) && Math.abs(yFactor - 1) >= 1e-12
    ? (hot.temp - hot.coldTemp * yFactor) / (yFactor - 1)
    : null
  return { slope, intercept, noiseTemp }
}

const avg1 = computed(() => average(samples1.value))
const avg2 = computed(() => average(samples2.value))
const coefficients = computed(() => ({
  adc1: coefficient(form.t_black_body_1, form.t_black_body_2, avg1.value.adc1, avg2.value.adc1),
  adc2: coefficient(form.t_black_body_1, form.t_black_body_2, avg1.value.adc2, avg2.value.adc2),
  adc3: coefficient(form.t_black_body_1, form.t_black_body_2, avg1.value.adc3, avg2.value.adc3),
}))

const canSave = computed(() => {
  return !saveBlockReason.value
})

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

const apiErrorMessage = (e: any, fallback: string) => {
  const detail = e?.data?.detail ?? e?.response?._data?.detail
  if (Array.isArray(detail)) {
    return detail.map((item) => item?.msg || JSON.stringify(item)).join('; ')
  }
  if (typeof detail === 'string' && detail) return detail
  return e?.statusMessage || e?.message || fallback
}

const saveBlockReason = computed(() => {
  if (samples1.value.length === 0) return 'Сначала сними нагрузку 1'
  if (samples2.value.length === 0) return 'Сначала сними нагрузку 2'
  if (![form.t_black_body_1, form.t_black_body_2, form.t_adc1, form.t_adc2, form.t_adc3].every(Number.isFinite)) {
    return 'Проверь температуры: все поля должны быть числами'
  }
  if (![coefficients.value.adc1.slope, coefficients.value.adc2.slope, coefficients.value.adc3.slope].some(Number.isFinite)) {
    return 'Нет ни одного ADC-канала с двумя разными валидными точками'
  }
  return ''
})

const skippedChannels = computed(() => {
  const skipped: string[] = []
  if (!Number.isFinite(coefficients.value.adc1.slope)) skipped.push(adcLabel('adc1', 'ADC1'))
  if (!Number.isFinite(coefficients.value.adc2.slope)) skipped.push(adcLabel('adc2', 'ADC2'))
  if (!Number.isFinite(coefficients.value.adc3.slope)) skipped.push(adcLabel('adc3', 'ADC3'))
  return skipped
})

const nullableNumber = (value: unknown) => {
  if (value === null || value === undefined || value === '') return null
  const num = Number(value)
  return Number.isFinite(num) ? num : null
}

const currentSample = (snapshot?: any) => {
  const state = snapshot || store.devices.get(props.deviceId)?.state || {}
  const adc1 = nullableNumber(state.voltage1 ?? state.adc1)
  const adc2 = nullableNumber(state.voltage2 ?? state.adc2)
  const adc3 = nullableNumber(state.voltage3 ?? state.adc3)
  if (![adc1, adc2, adc3].some(Number.isFinite)) return null
  return { adc1, adc2, adc3 }
}

const loadCalibrations = async () => {
  if (!props.deviceId) return
  loading.value = true
  status.value = ''
  try {
    const offset = (page.value - 1) * limit
    const res = await apiFetch<CalibrationsResponse>(`/api/devices/${props.deviceId}/calibrations?limit=${limit}&offset=${offset}`)
    calibrations.value = res.items
    total.value = res.total
  } catch (e: any) {
    status.value = apiErrorMessage(e, 'Не удалось загрузить калибровки')
  } finally {
    loading.value = false
  }
}

const goPage = (nextPage: number) => {
  page.value = Math.min(Math.max(1, nextPage), pageCount.value)
  loadCalibrations()
}

const openCreate = () => {
  if (props.logging) {
    status.value = 'Останови измерение перед калибровкой'
    return
  }
  editingId.value = ''
  resetForm()
  modalOpen.value = true
  modalStatus.value = ''
}

const openEdit = (item: Calibration) => {
  editingId.value = item.id
  form.durationSec = 10
  form.t_black_body_1 = item.t_black_body_1
  form.t_black_body_2 = item.t_black_body_2
  form.t_adc1 = item.t_adc1
  form.t_adc2 = item.t_adc2
  form.t_adc3 = item.t_adc3
  form.comment = item.comment || ''
  samples1.value = [{ adc1: item.adc1_1, adc2: item.adc2_1, adc3: item.adc3_1 }]
  samples2.value = [{ adc1: item.adc1_2, adc2: item.adc2_2, adc3: item.adc3_2 }]
  modalOpen.value = true
  modalStatus.value = ''
}

const closeCreate = () => {
  if (isSampling.value) return
  modalOpen.value = false
}

const startSample = async (phase: 1 | 2) => {
  if (props.logging) {
    modalStatus.value = 'Останови измерение перед калибровкой'
    return
  }
  if (isSampling.value) return
  if (phase === 1) samples1.value = []
  if (phase === 2) samples2.value = []
  applyBoundRadiometerTemps()
  applyBoundLoadTemp(phase)
  const target = phase === 1 ? samples1 : samples2
  const durationMs = Math.max(1, Number(form.durationSec) || 1) * 1000
  const intervalMs = 500
  const startedAt = Date.now()
  samplingPhase.value = phase
  modalStatus.value = `Замер нагрузки ${phase}...`

  while (samplingPhase.value === phase && Date.now() - startedAt < durationMs) {
    try {
      const snapshot = await store.getState(nuxtApp.$mqtt, props.deviceId)
      if (!snapshot) {
        // Older firmware answers get_state without resp.data and publishes the
        // snapshot on the separate state topic. Give that message a short time
        // to arrive, then read the latest Pinia state.
        await sleep(250)
      }
      const sample = currentSample(snapshot)
      if (sample) target.value.push(sample)
    } catch (e: any) {
      modalStatus.value = e?.message || 'Ошибка получения state'
    }
    await sleep(intervalMs)
  }

  if (samplingPhase.value === phase) {
    samplingPhase.value = null
  }
  modalStatus.value = `Замер нагрузки ${phase} завершен: ${target.value.length} samples`
}

const stopSample = () => {
  samplingPhase.value = null
}

const saveCalibration = async () => {
  if (!canSave.value) {
    modalStatus.value = saveBlockReason.value || 'Нужно снять обе нагрузки и проверить коэффициенты'
    return
  }
  saving.value = true
  modalStatus.value = editingId.value ? 'Сохраняю изменения...' : 'Сохраняю калибровку...'
  try {
    const url = editingId.value
      ? `/api/devices/${props.deviceId}/calibrations/${editingId.value}`
      : `/api/devices/${props.deviceId}/calibrations`
    const body = {
      t_black_body_1: form.t_black_body_1,
      t_black_body_2: form.t_black_body_2,
      adc1_1: avg1.value.adc1,
      adc2_1: avg1.value.adc2,
      adc3_1: avg1.value.adc3,
      adc1_2: avg2.value.adc1,
      adc2_2: avg2.value.adc2,
      adc3_2: avg2.value.adc3,
      t_adc1: form.t_adc1,
      t_adc2: form.t_adc2,
      t_adc3: form.t_adc3,
      comment: form.comment,
    }
    console.info('Saving calibration', { url, body, skippedChannels: skippedChannels.value })
    await apiFetch<Calibration>(url, {
      method: editingId.value ? 'PATCH' : 'POST',
      body,
    })
    modalOpen.value = false
    const skipped = skippedChannels.value.length ? ` Пропущено: ${skippedChannels.value.join(', ')}` : ''
    status.value = (editingId.value ? 'Калибровка обновлена.' : 'Калибровка сохранена.') + skipped
    page.value = 1
    await loadCalibrations()
  } catch (e: any) {
    console.error('Calibration save failed', e)
    modalStatus.value = apiErrorMessage(e, 'Не удалось сохранить калибровку')
  } finally {
    saving.value = false
  }
}

const deleteCalibration = async (item: Calibration) => {
  const confirmed = window.confirm(`Удалить калибровку от ${formatDate(item.created_at)}?`)
  if (!confirmed) return
  deletingId.value = item.id
  status.value = 'Удаляю калибровку...'
  try {
    await apiFetch(`/api/devices/${props.deviceId}/calibrations/${item.id}`, {
      method: 'DELETE',
    })
    status.value = 'Калибровка удалена'
    if (calibrations.value.length === 1 && page.value > 1) {
      page.value -= 1
    }
    await loadCalibrations()
  } catch (e: any) {
    status.value = apiErrorMessage(e, 'Не удалось удалить калибровку')
  } finally {
    deletingId.value = ''
  }
}

watch(
  () => props.deviceId,
  () => {
    page.value = 1
    loadCalibrations()
  },
  { immediate: true }
)
</script>

<style scoped>
.calibration-panel {
  overflow: hidden;
}

.table-wrap {
  overflow-x: auto;
}

.table {
  width: 100%;
  border-collapse: collapse;
  min-width: 920px;
}

.table th,
.table td {
  text-align: left;
  padding: 10px 12px;
  border-bottom: 1px solid var(--border);
  font-size: 13px;
  vertical-align: top;
}

.table th {
  font-size: 12px;
  color: var(--muted);
  text-transform: uppercase;
}

.calibration-modal {
  width: min(1040px, 96vw);
}

.calibration-steps {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 14px;
}

.calibration-step {
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 14px;
}

.compact-head {
  margin-bottom: 8px;
}

.sample-grid,
.preview {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.sample-grid span {
  padding: 8px 10px;
  border-radius: 10px;
  background: #f5f7fb;
  font-size: 13px;
}

.compact-actions {
  gap: 6px;
  flex-wrap: nowrap;
}

textarea {
  width: 100%;
  resize: vertical;
}

@media (max-width: 760px) {
  .calibration-steps {
    grid-template-columns: 1fr;
  }
}
</style>
