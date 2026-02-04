<template>
  <div class="page station-page" v-if="stationId">
    <div class="header">
      <button class="btn ghost back" @click="navigateTo('/stations')">← Назад</button>
      <div>
        <h2>Станция {{ station?.id || stationId }}</h2>
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
    </div>
  </div>
</template>

<script setup lang="ts">
definePageMeta({ layout: 'admin' })
useHead({ title: 'Станция' })

const route = useRoute()
const { apiFetch } = useApi()

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
})
</script>
