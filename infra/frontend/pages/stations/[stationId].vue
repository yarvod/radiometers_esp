<template>
  <div class="page station-page" v-if="stationId">
    <div class="header">
      <button class="btn ghost back" @click="navigateTo('/stations')">← Назад</button>
      <div>
        <h2>Станция {{ station?.station_id || stationId }}</h2>
        <p class="muted">Детальная карточка станции зондирования.</p>
      </div>
    </div>

    <p class="muted" v-if="statusMessage">{{ statusMessage }}</p>
    <p class="muted" v-if="loading">Загружаем станцию…</p>
    <p class="muted" v-if="refreshing">Идет обновление данных…</p>

    <div class="station-grid">
      <div class="card">
        <div class="card-head">
          <h3>Редактирование</h3>
          <span class="badge success">Локально</span>
        </div>
        <div class="form-group">
          <label>Название</label>
          <input type="text" v-model="editForm.name" />
        </div>
        <div class="inline fields">
          <label class="compact">Широта
            <input type="number" step="0.0001" v-model.number="editForm.lat" />
          </label>
          <label class="compact">Долгота
            <input type="number" step="0.0001" v-model.number="editForm.lon" />
          </label>
        </div>
        <div class="form-group">
          <label>Источник</label>
          <input type="text" v-model="editForm.src" />
        </div>
        <div class="actions">
          <button class="btn primary" @click="saveStation">Сохранить</button>
        </div>
        <p class="muted" v-if="editStatus">{{ editStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Загрузка профилей</h3>
          <span class="badge accent">ARQ</span>
        </div>
        <div class="inline fields">
          <label class="compact">Старт (UTC)
            <input type="datetime-local" v-model="loadForm.startAt" step="10800" />
          </label>
          <label class="compact">Конец (UTC)
            <input type="datetime-local" v-model="loadForm.endAt" step="10800" />
          </label>
          <label class="compact">Шаг, часы
            <input type="number" min="1" max="24" v-model.number="loadForm.stepHours" />
          </label>
        </div>
        <div class="actions">
          <button class="btn primary" @click="startSoundingJob" :disabled="jobRunning">Запустить загрузку</button>
        </div>
        <div v-if="job" class="job-status">
          <div class="muted">Статус: {{ job.status }} ({{ job.done }}/{{ job.total || 0 }})</div>
          <div class="progress-bar">
            <span class="progress-fill" :style="{ width: jobPercent + '%' }"></span>
          </div>
          <div class="muted" v-if="job.error">Ошибка: {{ job.error }}</div>
        </div>
        <p class="muted" v-if="jobStatus">{{ jobStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Профили станции</h3>
          <span class="badge">{{ soundingsTotal }}</span>
        </div>
        <div class="inline fields">
          <label class="compact">Фильтр от (UTC)
            <input type="datetime-local" v-model="filterForm.startAt" step="10800" />
          </label>
          <label class="compact">Фильтр до (UTC)
            <input type="datetime-local" v-model="filterForm.endAt" step="10800" />
          </label>
          <button class="btn ghost sm" @click="loadSoundings">Фильтр</button>
          <button class="btn ghost sm" @click="resetSoundingsFilter">Сброс</button>
        </div>
        <div class="bulk-actions">
          <div class="muted">Выбрано: {{ selectedCount }}</div>
          <div class="actions">
            <select v-model="bulkAction">
              <option value="csv">Скачать CSV</option>
              <option value="pwv">Построить PWV</option>
            </select>
            <button class="btn primary sm" @click="runBulkAction" :disabled="selectedCount === 0 || bulkRunning">
              Выполнить
            </button>
          </div>
        </div>
        <div v-if="soundingsLoading" class="loading-row">
          <span class="muted">Загружаем профили…</span>
          <span class="loading-bar"></span>
        </div>
        <div v-if="exportJob" class="job-status">
          <div class="muted">Экспорт: {{ exportJob.status }} ({{ exportJob.done }}/{{ exportJob.total || 0 }})</div>
          <div class="progress-bar">
            <span class="progress-fill" :style="{ width: exportPercent + '%' }"></span>
          </div>
          <div class="actions" v-if="exportJob.status === 'done' && exportJob.file_name">
            <button class="btn success sm" @click="downloadExport">Скачать {{ exportJob.file_name }}</button>
          </div>
          <div class="muted" v-if="exportJob.error">Ошибка: {{ exportJob.error }}</div>
        </div>
        <p class="muted" v-if="exportStatus">{{ exportStatus }}</p>
        <div v-else-if="soundings.length === 0" class="muted empty">Нет данных</div>
        <div v-else class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th class="check-cell">
                  <input type="checkbox" :checked="allSelected" @change="toggleSelectAll" />
                </th>
                <th>Время (UTC)</th>
                <th>Строк</th>
                <th>Имя станции</th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="item in soundings" :key="item.id" class="table-row">
                <td class="check-cell">
                  <input type="checkbox" v-model="selectedIds" :value="item.id" />
                </td>
                <td>{{ formatUtc(item.sounding_time) }}</td>
                <td>{{ item.row_count }}</td>
                <td>{{ item.station_name || '—' }}</td>
                <td><button class="btn ghost sm" @click="openSounding(item.id)">Открыть</button></td>
              </tr>
            </tbody>
          </table>
        </div>
        <div class="pagination" v-if="soundingsTotal > 0">
          <div class="muted">Показано {{ rangeStart }}–{{ rangeEnd }} из {{ soundingsTotal }}</div>
          <div class="pager">
            <button class="btn ghost sm" :disabled="soundingsPage <= 1" @click="goSoundingsPage(soundingsPage - 1)">←</button>
            <span class="chip subtle">{{ soundingsPage }} / {{ soundingsPageCount }}</span>
            <button class="btn ghost sm" :disabled="soundingsPage >= soundingsPageCount" @click="goSoundingsPage(soundingsPage + 1)">→</button>
          </div>
        </div>
      </div>

      <div class="card" v-if="selectedSounding">
        <div class="card-head">
          <h3>Данные профиля</h3>
          <span class="badge success">{{ formatUtc(selectedSounding.sounding_time) }}</span>
        </div>
        <div class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th v-for="col in selectedSounding.columns" :key="col">{{ col }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="(row, idx) in visibleRows" :key="idx">
                <td v-for="(cell, cidx) in row" :key="`${idx}-${cidx}`">{{ formatCell(cell) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <p class="muted" v-if="selectedSounding.rows.length > maxRows">
          Показано {{ maxRows }} строк из {{ selectedSounding.rows.length }}
        </p>
      </div>
    </div>

    <div class="modal-backdrop" v-if="pwvModalOpen">
      <div class="modal">
        <div class="modal-head">
          <h3>PWV для выбранных профилей</h3>
          <button class="btn ghost sm" @click="closePwvModal">Закрыть</button>
        </div>
        <div class="inline fields">
          <label class="compact">Мин. высота, м
            <input type="number" min="0" step="50" v-model.number="pwvMinHeight" />
          </label>
          <label class="compact">Смещение, часы
            <input type="number" min="-12" max="12" step="1" v-model.number="pwvOffsetHours" />
          </label>
        </div>
        <div class="actions">
          <button class="btn primary sm" @click="loadPwv" :disabled="pwvLoading">Построить</button>
        </div>
        <div v-if="pwvLoading" class="loading-row">
          <span class="muted">Считаем PWV…</span>
          <span class="loading-bar"></span>
        </div>
        <div v-else-if="pwvItems.length === 0" class="muted empty">Нет данных для графика</div>
        <div v-else class="pwv-chart">
          <svg :viewBox="`0 0 ${chartWidth} ${chartHeight}`" role="img">
            <polyline :points="pwvPolyline" fill="none" stroke="#2ecc71" stroke-width="2" />
          </svg>
          <div class="pwv-legend">
            <div class="muted">Мин: {{ pwvMin }} мм</div>
            <div class="muted">Макс: {{ pwvMax }} мм</div>
          </div>
          <div class="pwv-table">
            <div class="pwv-row" v-for="item in pwvItems" :key="item.id">
              <div class="pwv-time">{{ formatOffset(item.sounding_time) }}</div>
              <div class="pwv-value">{{ formatPwv(item.pwv) }}</div>
            </div>
          </div>
        </div>
        <p class="muted" v-if="pwvStatus">{{ pwvStatus }}</p>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
definePageMeta({ layout: 'admin' })
useHead({ title: 'Станция' })

const route = useRoute()
const { apiFetch } = useApi()
const config = useRuntimeConfig()

const stationId = computed(() => {
  const raw = route.params.stationId
  return Array.isArray(raw) ? raw[0] : raw
})

const station = ref<any | null>(null)
const statusMessage = ref('')
const editStatus = ref('')
const refreshing = ref(false)
const loading = ref(false)
const editForm = reactive({
  name: '',
  lat: null as number | null,
  lon: null as number | null,
  src: '',
})
const soundings = ref<any[]>([])
const soundingsTotal = ref(0)
const soundingsOffset = ref(0)
const soundingsLimit = ref(25)
const soundingsLoading = ref(false)
const selectedSounding = ref<any | null>(null)
const selectedIds = ref<string[]>([])
const bulkAction = ref('csv')
const maxRows = 200
const job = ref<any | null>(null)
const jobStatus = ref('')
const jobTimer = ref<ReturnType<typeof setInterval> | null>(null)
const exportJob = ref<any | null>(null)
const exportStatus = ref('')
const exportTimer = ref<ReturnType<typeof setInterval> | null>(null)
const pwvModalOpen = ref(false)
const pwvMinHeight = ref(0)
const pwvOffsetHours = ref(0)
const pwvItems = ref<any[]>([])
const pwvLoading = ref(false)
const pwvStatus = ref('')
const chartWidth = 600
const chartHeight = 200
const loadForm = reactive({
  startAt: '',
  endAt: '',
  stepHours: 3,
})
const filterForm = reactive({
  startAt: '',
  endAt: '',
})

function syncForm() {
  editForm.name = station.value?.name || ''
  editForm.lat = station.value?.lat ?? null
  editForm.lon = station.value?.lon ?? null
  editForm.src = station.value?.src || ''
}

async function loadStation() {
  if (!stationId.value) return
  loading.value = true
  try {
    station.value = await apiFetch(`/api/stations/${stationId.value}`)
    statusMessage.value = ''
    syncForm()
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить станцию'
  } finally {
    loading.value = false
  }
}

function toUtcInputValue(date = new Date()) {
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${date.getUTCFullYear()}-${pad(date.getUTCMonth() + 1)}-${pad(date.getUTCDate())}T${pad(
    date.getUTCHours()
  )}:${pad(date.getUTCMinutes())}`
}

function parseUtcInput(value: string) {
  if (!value) return null
  const [datePart, timePart] = value.split('T')
  if (!datePart || !timePart) return null
  const [year, month, day] = datePart.split('-').map(Number)
  const [hour, minute] = timePart.split(':').map(Number)
  if ([year, month, day, hour, minute].some((v) => Number.isNaN(v))) return null
  return new Date(Date.UTC(year, month - 1, day, hour, minute, 0))
}

function formatUtc(value: string) {
  const date = new Date(value)
  return date.toISOString().replace('T', ' ').slice(0, 16)
}

const soundingsPage = computed(() => Math.floor(soundingsOffset.value / soundingsLimit.value) + 1)
const soundingsPageCount = computed(() => Math.max(1, Math.ceil(soundingsTotal.value / soundingsLimit.value)))
const rangeStart = computed(() => (soundingsTotal.value === 0 ? 0 : soundingsOffset.value + 1))
const rangeEnd = computed(() => Math.min(soundingsTotal.value, soundingsOffset.value + soundings.value.length))
const jobRunning = computed(() => job.value && ['pending', 'running'].includes(job.value.status))
const exportRunning = computed(() => exportJob.value && ['pending', 'running'].includes(exportJob.value.status))
const bulkRunning = computed(() => exportRunning.value || pwvLoading.value)
const jobPercent = computed(() => {
  if (!job.value || !job.value.total) return 0
  return Math.min(100, Math.round((job.value.done / job.value.total) * 100))
})
const exportPercent = computed(() => {
  if (!exportJob.value || !exportJob.value.total) return 0
  return Math.min(100, Math.round((exportJob.value.done / exportJob.value.total) * 100))
})
const selectedCount = computed(() => selectedIds.value.length)
const allSelected = computed(() => {
  if (soundings.value.length === 0) return false
  return soundings.value.every((item) => selectedIds.value.includes(item.id))
})
const visibleRows = computed(() => {
  if (!selectedSounding.value) return []
  return selectedSounding.value.rows.slice(0, maxRows)
})
const pwvMin = computed(() => {
  const values = pwvItems.value.map((item) => item.pwv).filter((v) => typeof v === 'number') as number[]
  if (!values.length) return '—'
  return Math.min(...values).toFixed(2)
})
const pwvMax = computed(() => {
  const values = pwvItems.value.map((item) => item.pwv).filter((v) => typeof v === 'number') as number[]
  if (!values.length) return '—'
  return Math.max(...values).toFixed(2)
})
const pwvPolyline = computed(() => {
  const values = pwvItems.value.map((item) => item.pwv).filter((v) => typeof v === 'number') as number[]
  if (!values.length) return ''
  const minVal = Math.min(...values)
  const maxVal = Math.max(...values)
  const range = maxVal - minVal || 1
  const points = pwvItems.value.map((item, idx) => {
    const x = (idx / Math.max(1, pwvItems.value.length - 1)) * chartWidth
    const v = typeof item.pwv === 'number' ? item.pwv : minVal
    const y = chartHeight - ((v - minVal) / range) * (chartHeight - 20) - 10
    return `${x.toFixed(2)},${y.toFixed(2)}`
  })
  return points.join(' ')
})

function formatCell(value: any) {
  if (value == null) return ''
  if (typeof value === 'number') return value.toFixed(2)
  return String(value)
}

function formatOffset(value: string) {
  const date = new Date(value)
  if (!Number.isFinite(pwvOffsetHours.value)) return formatUtc(value)
  const shifted = new Date(date.getTime() + pwvOffsetHours.value * 3600 * 1000)
  return shifted.toISOString().replace('T', ' ').slice(0, 16)
}

function formatPwv(value: any) {
  if (typeof value !== 'number') return '—'
  return `${value.toFixed(3)} мм`
}

async function loadSoundings() {
  if (!stationId.value) return
  soundingsLoading.value = true
  try {
    const params: string[] = [`limit=${soundingsLimit.value}`, `offset=${soundingsOffset.value}`]
    const start = parseUtcInput(filterForm.startAt)
    const end = parseUtcInput(filterForm.endAt)
    if (start) params.push(`from=${encodeURIComponent(start.toISOString())}`)
    if (end) params.push(`to=${encodeURIComponent(end.toISOString())}`)
    const res = await apiFetch<any>(`/api/stations/${stationId.value}/soundings?${params.join('&')}`)
    soundings.value = res.items || []
    soundingsTotal.value = res.total || 0
    syncSelection()
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить профили'
  } finally {
    soundingsLoading.value = false
  }
}

async function openSounding(id: string) {
  try {
    selectedSounding.value = await apiFetch(`/api/stations/${stationId.value}/soundings/${id}`)
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить профиль'
  }
}

function goSoundingsPage(next: number) {
  const newOffset = (next - 1) * soundingsLimit.value
  if (newOffset < 0) return
  soundingsOffset.value = newOffset
  loadSoundings()
}

function resetSoundingsFilter() {
  filterForm.startAt = ''
  filterForm.endAt = ''
  soundingsOffset.value = 0
  loadSoundings()
}

function syncSelection() {
  const available = new Set(soundings.value.map((item) => item.id))
  selectedIds.value = selectedIds.value.filter((id) => available.has(id))
}

function toggleSelectAll() {
  if (allSelected.value) {
    selectedIds.value = []
  } else {
    selectedIds.value = soundings.value.map((item) => item.id)
  }
}

async function runBulkAction() {
  if (bulkAction.value === 'csv') {
    await startExport()
    return
  }
  if (bulkAction.value === 'pwv') {
    openPwvModal()
  }
}

async function startExport() {
  if (!stationId.value) return
  exportStatus.value = ''
  if (selectedIds.value.length === 0) {
    exportStatus.value = 'Выберите профили для экспорта'
    return
  }
  try {
    exportJob.value = await apiFetch(`/api/stations/${stationId.value}/soundings/export`, {
      method: 'POST',
      body: {
        ids: selectedIds.value,
      },
    })
    trackExportJob()
  } catch (e: any) {
    exportStatus.value = e?.data?.detail || e?.message || 'Не удалось запустить экспорт'
  }
}

async function pollExportJob() {
  if (!exportJob.value) return
  try {
    exportJob.value = await apiFetch(`/api/soundings/exports/${exportJob.value.id}`)
    if (!['pending', 'running'].includes(exportJob.value.status)) {
      stopExportPolling()
    }
  } catch (e: any) {
    exportStatus.value = e?.message || 'Не удалось получить статус экспорта'
    stopExportPolling()
  }
}

function trackExportJob() {
  stopExportPolling()
  exportTimer.value = setInterval(pollExportJob, 2000)
}

function stopExportPolling() {
  if (exportTimer.value) {
    clearInterval(exportTimer.value)
    exportTimer.value = null
  }
}

function downloadExport() {
  if (!exportJob.value) return
  const base = String(config.public.apiBase || '').replace(/\/$/, '')
  window.location.href = `${base}/api/soundings/exports/${exportJob.value.id}/download`
  exportJob.value.file_name = null
}

function openPwvModal() {
  pwvModalOpen.value = true
  pwvStatus.value = ''
  pwvItems.value = []
}

function closePwvModal() {
  pwvModalOpen.value = false
}

async function loadPwv() {
  if (!stationId.value) return
  pwvStatus.value = ''
  pwvLoading.value = true
  try {
    const res = await apiFetch<any>(`/api/stations/${stationId.value}/soundings/pwv`, {
      method: 'POST',
      body: {
        ids: selectedIds.value,
        min_height: pwvMinHeight.value || 0,
      },
    })
    pwvItems.value = res.items || []
  } catch (e: any) {
    pwvStatus.value = e?.data?.detail || e?.message || 'Не удалось рассчитать PWV'
  } finally {
    pwvLoading.value = false
  }
}

async function startSoundingJob() {
  if (!stationId.value) return
  jobStatus.value = ''
  const startAt = parseUtcInput(loadForm.startAt)
  const endAt = parseUtcInput(loadForm.endAt)
  if (!startAt || !endAt) {
    jobStatus.value = 'Укажите диапазон дат (UTC)'
    return
  }
  try {
    job.value = await apiFetch(`/api/stations/${stationId.value}/soundings/load`, {
      method: 'POST',
      body: {
        start_at: startAt.toISOString(),
        end_at: endAt.toISOString(),
        step_hours: loadForm.stepHours,
      },
    })
    trackJob()
  } catch (e: any) {
    jobStatus.value = e?.data?.detail || e?.message || 'Не удалось запустить загрузку'
  }
}

async function pollJob() {
  if (!job.value) return
  try {
    job.value = await apiFetch(`/api/soundings/jobs/${job.value.id}`)
    if (!['pending', 'running'].includes(job.value.status)) {
      stopJobPolling()
      await loadSoundings()
    }
  } catch (e: any) {
    jobStatus.value = e?.message || 'Не удалось получить статус'
    stopJobPolling()
  }
}

function trackJob() {
  stopJobPolling()
  jobTimer.value = setInterval(pollJob, 2000)
}

function stopJobPolling() {
  if (jobTimer.value) {
    clearInterval(jobTimer.value)
    jobTimer.value = null
  }
}

async function saveStation() {
  if (!stationId.value) return
  editStatus.value = ''
  try {
    const payload: any = {}
    if (editForm.name.trim()) payload.name = editForm.name.trim()
    if (editForm.lat != null) payload.lat = editForm.lat
    if (editForm.lon != null) payload.lon = editForm.lon
    if (editForm.src.trim()) payload.src = editForm.src.trim()
    station.value = await apiFetch(`/api/stations/${stationId.value}`, {
      method: 'PATCH',
      body: payload,
    })
    editStatus.value = 'Изменения сохранены'
    syncForm()
  } catch (e: any) {
    editStatus.value = e?.data?.detail || e?.message || 'Не удалось сохранить изменения'
  }
}

onMounted(() => {
  loadStation()
  const now = new Date()
  const start = new Date(now.getTime() - 24 * 3600 * 1000)
  loadForm.startAt = toUtcInputValue(start)
  loadForm.endAt = toUtcInputValue(now)
  filterForm.startAt = ''
  filterForm.endAt = ''
  loadSoundings()
})

onBeforeUnmount(() => {
  stopJobPolling()
  stopExportPolling()
})
</script>
