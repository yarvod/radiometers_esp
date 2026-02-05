<template>
  <div class="page schedule-page">
    <div class="header">
      <div>
        <h1>Расписание профилей</h1>
        <p class="muted">Выберите станции и настройте интервал/офсет для автоматической загрузки.</p>
      </div>
    </div>

    <p class="muted" v-if="statusMessage">{{ statusMessage }}</p>

    <div class="card">
      <div class="card-head">
        <h3>Настройки расписания</h3>
        <span class="badge accent">UTC</span>
      </div>
      <div class="inline fields">
        <label class="compact">Интервал, часы
          <input type="number" min="1" max="24" v-model.number="config.interval_hours" />
        </label>
        <label class="compact">Офсет, часы
          <input type="number" min="0" max="23" v-model.number="config.offset_hours" />
        </label>
      </div>
      <div class="actions">
        <button class="btn primary" @click="saveConfig">Применить</button>
      </div>
      <p class="muted" v-if="configStatus">{{ configStatus }}</p>
    </div>

    <div class="grid">
      <div class="card">
        <div class="card-head">
          <h3>Станции</h3>
          <span class="badge">{{ stations.length }}</span>
        </div>
        <div class="search-row">
          <div class="search-field">
            <span class="search-icon">🔎</span>
            <input type="text" class="search-input" v-model="stationQuery" placeholder="Поиск по названию или ID" />
          </div>
        </div>
        <div v-if="stationsLoading" class="loading-row">
          <span class="muted">Загружаем станции…</span>
          <span class="loading-bar"></span>
        </div>
        <div v-else class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th>ID</th>
                <th>Название</th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="station in stations" :key="station.station_id" class="table-row">
                <td>{{ station.station_id }}</td>
                <td>{{ station.name || '—' }}</td>
                <td>
                  <button class="btn ghost sm" @click="addSchedule(station.station_id)">Добавить</button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Выбранные станции</h3>
          <span class="badge success">{{ schedule.length }}</span>
        </div>
        <div v-if="scheduleLoading" class="loading-row">
          <span class="muted">Загружаем расписание…</span>
          <span class="loading-bar"></span>
        </div>
        <div v-else-if="schedule.length === 0" class="muted empty">Нет станций</div>
        <div v-else class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th>ID</th>
                <th>Название</th>
                <th>Вкл</th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="item in schedule" :key="item.id" class="table-row">
                <td>{{ item.station_code }}</td>
                <td>{{ item.station_name || '—' }}</td>
                <td>
                  <label class="checkbox">
                    <input type="checkbox" :checked="item.enabled" @change="toggleSchedule(item, $event)" />
                    <span>{{ item.enabled ? 'Да' : 'Нет' }}</span>
                  </label>
                </td>
                <td>
                  <button class="btn warning ghost sm" @click="removeSchedule(item)">Удалить</button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
definePageMeta({ layout: 'admin' })
useHead({ title: 'Расписание профилей' })

const { apiFetch } = useApi()

const stations = ref<any[]>([])
const stationsLoading = ref(false)
const stationQuery = ref('')
const schedule = ref<any[]>([])
const scheduleLoading = ref(false)
const statusMessage = ref('')
const configStatus = ref('')
const config = reactive({ interval_hours: 3, offset_hours: 2 })
const searchTimer = ref<ReturnType<typeof setTimeout> | null>(null)

async function loadStations() {
  stationsLoading.value = true
  try {
    const suffix = stationQuery.value ? `&query=${encodeURIComponent(stationQuery.value)}` : ''
    const res = await apiFetch<any>(`/api/stations?limit=50&offset=0${suffix}`)
    stations.value = res.items || []
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить станции'
  } finally {
    stationsLoading.value = false
  }
}

async function loadSchedule() {
  scheduleLoading.value = true
  try {
    const res = await apiFetch<any>(`/api/soundings/schedule`)
    schedule.value = res.items || []
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить расписание'
  } finally {
    scheduleLoading.value = false
  }
}

async function loadConfig() {
  try {
    const res = await apiFetch<any>(`/api/soundings/schedule/config`)
    config.interval_hours = res.interval_hours
    config.offset_hours = res.offset_hours
  } catch (e: any) {
    statusMessage.value = e?.message || 'Не удалось загрузить настройки'
  }
}

async function saveConfig() {
  configStatus.value = ''
  try {
    await apiFetch(`/api/soundings/schedule/config`, {
      method: 'PUT',
      body: {
        interval_hours: config.interval_hours,
        offset_hours: config.offset_hours,
      },
    })
    configStatus.value = 'Настройки сохранены'
  } catch (e: any) {
    configStatus.value = e?.data?.detail || e?.message || 'Не удалось сохранить настройки'
  }
}

async function addSchedule(stationId: string) {
  try {
    await apiFetch(`/api/soundings/schedule`, {
      method: 'POST',
      body: { station_id: stationId },
    })
    await loadSchedule()
  } catch (e: any) {
    statusMessage.value = e?.data?.detail || e?.message || 'Не удалось добавить'
  }
}

async function toggleSchedule(item: any, event: Event) {
  const target = event.target as HTMLInputElement
  try {
    await apiFetch(`/api/soundings/schedule/${item.id}`, {
      method: 'PATCH',
      body: { enabled: target.checked },
    })
    await loadSchedule()
  } catch (e: any) {
    statusMessage.value = e?.data?.detail || e?.message || 'Не удалось обновить'
  }
}

async function removeSchedule(item: any) {
  if (!confirm(`Удалить станцию ${item.station_code}?`)) return
  try {
    await apiFetch(`/api/soundings/schedule/${item.id}`, { method: 'DELETE' })
    await loadSchedule()
  } catch (e: any) {
    statusMessage.value = e?.data?.detail || e?.message || 'Не удалось удалить'
  }
}

watch(
  () => stationQuery.value,
  () => {
    if (searchTimer.value) {
      clearTimeout(searchTimer.value)
    }
    searchTimer.value = setTimeout(loadStations, 300)
  }
)

onMounted(() => {
  loadStations()
  loadSchedule()
  loadConfig()
})
</script>
