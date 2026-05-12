<template>
  <div class="page stations-page">
    <div class="header">
      <div>
        <h1>Станции зондирования</h1>
        <p class="muted">Источники с координатами, используемые для обновления профилей.</p>
      </div>
      <div class="actions">
        <div class="form-group">
          <label class="muted small">Дата/время (UTC)</label>
          <input type="datetime-local" v-model="refreshForm.datetimeLocal" step="10800" />
        </div>
        <button class="btn ghost" @click="openMapModal" :disabled="mapLoading">Посмотреть на карте</button>
        <button class="btn primary" @click="refreshStations" :disabled="refreshing">Обновить список</button>
      </div>
    </div>
    <div class="search-row">
      <div class="search-field">
        <span class="search-icon">🔎</span>
        <input
          type="text"
          class="search-input"
          v-model="searchQuery"
          placeholder="Поиск по названию или ID"
        />
        <button v-if="searchQuery" class="btn ghost sm" @click="clearSearch">Очистить</button>
      </div>
    </div>

    <p class="muted" v-if="statusMessage">{{ statusMessage }}</p>
    <p class="muted" v-if="refreshing">Идет обновление данных…</p>

    <div class="card table-card">
      <div class="card-head">
        <h3>Список станций</h3>
        <span class="badge success">{{ total }}</span>
      </div>
      <div v-if="loading" class="loading-row">
        <span class="muted">Загружаем станции…</span>
        <span class="loading-bar"></span>
      </div>
      <div v-if="stations.length === 0" class="muted empty">
        {{ searchQuery ? 'Нет станций по запросу' : 'Нет станций' }}
      </div>
      <div v-else class="table-wrap">
        <table class="table">
          <thead>
            <tr>
              <th>ID</th>
              <th>Название</th>
              <th>Координаты</th>
              <th>Источник</th>
              <th>Обновлено</th>
            </tr>
          </thead>
          <tbody>
            <tr
              v-for="station in stations"
              :key="station.station_id"
              class="table-row"
              tabindex="0"
              @click="openStation(station.station_id)"
              @keydown.enter="openStation(station.station_id)"
            >
              <td>
                <strong>{{ station.station_id }}</strong>
                <div class="muted small">Создано: {{ formatDate(station.created_at) }}</div>
              </td>
              <td>{{ station.name || '—' }}</td>
              <td>{{ formatCoords(station.lat, station.lon) }}</td>
              <td><span class="chip subtle">{{ station.src || '—' }}</span></td>
              <td>{{ station.updated_at ? formatDate(station.updated_at) : '—' }}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <div class="pagination" v-if="total > 0">
        <div class="muted">
          Показано {{ rangeStart }}–{{ rangeEnd }} из {{ total }}
        </div>
        <div class="pager">
          <button class="btn ghost sm" :disabled="page <= 1" @click="goPage(page - 1)">←</button>
          <span class="chip subtle">{{ page }} / {{ pageCount }}</span>
          <button class="btn ghost sm" :disabled="page >= pageCount" @click="goPage(page + 1)">→</button>
        </div>
      </div>
    </div>

    <div v-if="mapModalOpen" class="modal-backdrop" @click.self="closeMapModal">
      <div class="modal stations-map-modal">
        <div class="modal-head">
          <div>
            <h3>Станции на карте</h3>
            <p class="muted">{{ mapStatus || `Показано станций: ${mapStationPoints.length}` }}</p>
          </div>
          <button class="btn ghost" @click="closeMapModal">Закрыть</button>
        </div>
        <div ref="mapEl" class="stations-map"></div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { Map as LeafletMap, Marker } from 'leaflet'

definePageMeta({ layout: 'admin' })
useHead({ title: 'Станции зондирования' })

const { apiFetch } = useApi()

const stations = ref<any[]>([])
const total = ref(0)
const limit = ref(25)
const offset = ref(0)
const statusMessage = ref('')
const loading = ref(false)
const refreshing = ref(false)
const refreshForm = reactive({ datetimeLocal: '' })
const searchQuery = ref('')
const searchTimer = ref<ReturnType<typeof setTimeout> | null>(null)
const mapModalOpen = ref(false)
const mapLoading = ref(false)
const mapStatus = ref('')
const mapStations = ref<any[]>([])
const mapEl = ref<HTMLElement | null>(null)
let mapInstance: LeafletMap | null = null
let mapMarkers: Marker[] = []

const page = computed(() => Math.floor(offset.value / limit.value) + 1)
const pageCount = computed(() => Math.max(1, Math.ceil(total.value / limit.value)))
const rangeStart = computed(() => (total.value === 0 ? 0 : offset.value + 1))
const rangeEnd = computed(() => Math.min(total.value, offset.value + stations.value.length))
const mapStationPoints = computed(() =>
  mapStations.value.filter((station) => isValidCoord(station.lat, station.lon))
)

function floorTo12HoursUtc(date: Date) {
  const copy = new Date(date)
  copy.setUTCMinutes(0, 0, 0)
  const hours = copy.getUTCHours()
  copy.setUTCHours(Math.floor(hours / 12) * 12)
  return copy
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

function normalizeUtcInput(value: string) {
  const parsed = parseUtcInput(value)
  if (!parsed) return value
  return toUtcInputValue(floorTo12HoursUtc(parsed))
}

function formatDate(value: string) {
  return new Date(value).toLocaleString()
}

function formatCoords(lat?: number, lon?: number) {
  if (lat == null || lon == null) return '—'
  return `${lat.toFixed(4)}, ${lon.toFixed(4)}`
}

function isValidCoord(lat?: number, lon?: number) {
  return Number.isFinite(lat) && Number.isFinite(lon) && lat! >= -90 && lat! <= 90 && lon! >= -180 && lon! <= 180
}

function escapeHtml(value: string) {
  return value.replace(/[&<>"']/g, (char) => {
    const map: Record<string, string> = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;' }
    return map[char] || char
  })
}

async function loadStations() {
  loading.value = true
  try {
    const queryParam = searchQuery.value.trim()
    const suffix = queryParam ? `&query=${encodeURIComponent(queryParam)}` : ''
    const res = await apiFetch<any>(`/api/stations?limit=${limit.value}&offset=${offset.value}${suffix}`)
    stations.value = res.items || []
    total.value = res.total || 0
    statusMessage.value = ''
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить список станций'
  } finally {
    loading.value = false
  }
}

async function loadMapStations() {
  mapLoading.value = true
  mapStatus.value = 'Загружаем станции для карты…'
  try {
    const queryParam = searchQuery.value.trim()
    const suffix = queryParam ? `&query=${encodeURIComponent(queryParam)}` : ''
    const collected: any[] = []
    let nextOffset = 0
    let expectedTotal = 0
    do {
      const res = await apiFetch<any>(`/api/stations?limit=500&offset=${nextOffset}${suffix}`)
      const items = res.items || []
      collected.push(...items)
      expectedTotal = res.total || collected.length
      nextOffset += items.length
      if (items.length === 0) break
    } while (collected.length < expectedTotal)
    mapStations.value = collected
    const skipped = collected.length - mapStationPoints.value.length
    mapStatus.value = skipped > 0 ? `Без координат: ${skipped}` : ''
  } catch (e: any) {
    mapStatus.value = e?.data?.detail || e?.message || 'Не удалось загрузить станции для карты'
    mapStations.value = []
  } finally {
    mapLoading.value = false
  }
}

async function openMapModal() {
  mapModalOpen.value = true
  await loadMapStations()
  await nextTick()
  renderMap()
}

function closeMapModal() {
  mapModalOpen.value = false
  destroyMap()
}

function destroyMap() {
  mapMarkers = []
  if (mapInstance) {
    mapInstance.remove()
    mapInstance = null
  }
}

async function renderMap() {
  if (!process.client || !mapModalOpen.value || !mapEl.value) return
  destroyMap()
  const points = mapStationPoints.value
  if (points.length === 0) {
    mapStatus.value = mapStatus.value || 'У станций нет координат для отображения'
    return
  }
  const L = await import('leaflet')
  const centerLat = points.reduce((sum, station) => sum + station.lat, 0) / points.length
  const centerLon = points.reduce((sum, station) => sum + station.lon, 0) / points.length
  mapInstance = L.map(mapEl.value, {
    center: [centerLat, centerLon],
    zoom: points.length === 1 ? 6 : 3,
    scrollWheelZoom: true,
  })
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    maxZoom: 18,
    attribution: '&copy; OpenStreetMap',
  }).addTo(mapInstance)
  const bounds = L.latLngBounds([])
  const stationIcon = L.divIcon({
    className: 'station-map-marker',
    html: '<span></span>',
    iconSize: [22, 22],
    iconAnchor: [11, 11],
  })
  points.forEach((station) => {
    const label = `${station.station_id}${station.name ? ` · ${station.name}` : ''}`
    const marker = L.marker([station.lat, station.lon], { icon: stationIcon })
      .bindPopup(`<strong>${escapeHtml(station.station_id)}</strong><br>${escapeHtml(station.name || 'Без названия')}`)
      .bindTooltip(escapeHtml(label), { direction: 'top', offset: [0, -12] })
      .addTo(mapInstance!)
    mapMarkers.push(marker)
    bounds.extend([station.lat, station.lon])
  })
  if (points.length > 1) {
    mapInstance.fitBounds(bounds, { padding: [36, 36], maxZoom: 8 })
  }
  setTimeout(() => mapInstance?.invalidateSize(), 50)
}

async function refreshStations() {
  refreshing.value = true
  statusMessage.value = ''
  try {
    const payload: any = {}
    if (refreshForm.datetimeLocal) {
      const normalized = normalizeUtcInput(refreshForm.datetimeLocal)
      const parsed = parseUtcInput(normalized)
      if (parsed) {
        payload.datetime = parsed.toISOString()
      }
    }
    await apiFetch('/api/stations/refresh', { method: 'POST', body: payload })
    await loadStations()
    statusMessage.value = 'Список станций обновлен'
  } catch (e: any) {
    statusMessage.value = e?.data?.detail || e?.message || 'Не удалось обновить станции'
  } finally {
    refreshing.value = false
  }
}

function goPage(next: number) {
  const newOffset = (next - 1) * limit.value
  if (newOffset < 0) return
  offset.value = newOffset
  loadStations()
}

function clearSearch() {
  searchQuery.value = ''
}

function openStation(id: string) {
  navigateTo(`/stations/${id}`)
}

onMounted(() => {
  refreshForm.datetimeLocal = toUtcInputValue(floorTo12HoursUtc(new Date()))
  loadStations()
})

onBeforeUnmount(() => {
  destroyMap()
})

watch(
  () => refreshForm.datetimeLocal,
  (value) => {
    if (!value) return
    const normalized = normalizeUtcInput(value)
    if (normalized !== value) {
      refreshForm.datetimeLocal = normalized
    }
  }
)

watch(
  () => searchQuery.value,
  () => {
    if (searchTimer.value) {
      clearTimeout(searchTimer.value)
    }
    searchTimer.value = setTimeout(() => {
      offset.value = 0
      loadStations()
    }, 350)
  }
)
</script>
