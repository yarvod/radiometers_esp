<template>
  <div class="card">
    <div class="card-head">
      <div>
        <h3>Обработанные GNSS данные</h3>
        <p class="muted">Загруженные наборы для сравнения и PWV-графиков.</p>
      </div>
      <button class="btn ghost sm" type="button" @click="load" :disabled="loading">Обновить</button>
    </div>
    <div class="gnss-admin">
      <div class="gnss-list">
        <div class="inline fields">
          <label class="compact">Новый источник
            <input v-model="createName" placeholder="Например: внешняя станция" />
          </label>
          <button class="btn primary sm" type="button" @click="createDataset" :disabled="loading">Добавить</button>
        </div>
        <div v-if="datasets.length" class="gnss-list-items">
          <button
            v-for="item in datasets"
            :key="item.id"
            class="gnss-list-item"
            :class="{ selected: selectedId === item.id }"
            type="button"
            @click="select(item.id)"
          >
            <strong>{{ item.name }}</strong>
            <span class="muted small">{{ item.measurement_count }} точек</span>
          </button>
        </div>
        <p v-else class="muted">GNSS источники пока не добавлены</p>
      </div>

      <div class="gnss-detail" v-if="selected">
        <div class="form-group">
          <label>Название</label>
          <input v-model="editForm.name" />
        </div>
        <div class="form-group">
          <label>Описание</label>
          <textarea v-model="editForm.description" rows="3"></textarea>
        </div>
        <div class="status-row">
          <span class="chip subtle">Точек: {{ selected.measurement_count }}</span>
          <span class="chip subtle">Начало: {{ formatLocalDateTime(selected.start_at) }}</span>
          <span class="chip subtle">Конец: {{ formatLocalDateTime(selected.end_at) }}</span>
        </div>
        <div class="actions">
          <button class="btn primary" type="button" @click="updateDataset" :disabled="loading">Сохранить</button>
          <button class="btn danger ghost" type="button" @click="deleteDataset" :disabled="loading">Удалить источник</button>
        </div>
        <div class="form-group">
          <label>Импорт текстового файла</label>
          <input ref="fileInput" type="file" accept=".txt,text/plain" @change="onFileChange" />
        </div>
        <button class="btn primary sm" type="button" @click="importData" :disabled="importing || !importFile">
          {{ importing ? 'Загрузка…' : 'Импортировать' }}
        </button>
      </div>
    </div>
    <p class="muted" v-if="status">{{ status }}</p>
  </div>
</template>

<script setup lang="ts">
import type { GnssData, GnssDataImportResponse } from '~/types/device'
import { formatLocalDateTime } from '~/utils/datetime'

const props = defineProps<{ deviceId: string }>()
const emit = defineEmits<{ changed: [] }>()
const { apiFetch } = useApi()

const datasets = ref<GnssData[]>([])
const selectedId = ref('')
const createName = ref('')
const editForm = reactive({ name: '', description: '' })
const importFile = ref<File | null>(null)
const fileInput = ref<HTMLInputElement | null>(null)
const loading = ref(false)
const importing = ref(false)
const status = ref('')
const selected = computed(() => datasets.value.find((item) => item.id === selectedId.value) || null)
let requestVersion = 0

const seedEdit = (item: GnssData | null) => {
  editForm.name = item?.name || ''
  editForm.description = item?.description || ''
}

const select = (id: string) => {
  selectedId.value = id
  seedEdit(datasets.value.find((item) => item.id === id) || null)
  importFile.value = null
  if (fileInput.value) fileInput.value.value = ''
}

const load = async () => {
  if (!props.deviceId) return
  const version = ++requestVersion
  loading.value = true
  try {
    const items = await apiFetch<GnssData[]>(`/api/devices/${props.deviceId}/gnss-data`)
    if (version !== requestVersion) return
    datasets.value = items || []
    const available = new Set(datasets.value.map((item) => item.id))
    if (!selectedId.value || !available.has(selectedId.value)) selectedId.value = datasets.value[0]?.id || ''
    seedEdit(selected.value)
    status.value = ''
  } catch (error: any) {
    if (version === requestVersion) status.value = error?.data?.detail || error?.message || 'Не удалось загрузить GNSS источники'
  } finally {
    if (version === requestVersion) loading.value = false
  }
}

const createDataset = async () => {
  const name = createName.value.trim()
  if (!name || !props.deviceId) return
  loading.value = true
  try {
    const item = await apiFetch<GnssData>(`/api/devices/${props.deviceId}/gnss-data`, {
      method: 'POST', body: { name },
    })
    datasets.value = [...datasets.value, item]
    createName.value = ''
    select(item.id)
    status.value = 'GNSS источник добавлен'
    emit('changed')
  } catch (error: any) {
    status.value = error?.data?.detail || error?.message || 'Не удалось добавить GNSS источник'
  } finally {
    loading.value = false
  }
}

const updateDataset = async () => {
  if (!selected.value) return
  loading.value = true
  try {
    const item = await apiFetch<GnssData>(`/api/devices/${props.deviceId}/gnss-data/${selected.value.id}`, {
      method: 'PATCH', body: { name: editForm.name, description: editForm.description || null },
    })
    datasets.value = datasets.value.map((existing) => existing.id === item.id ? item : existing)
    seedEdit(item)
    status.value = 'GNSS источник обновлен'
    emit('changed')
  } catch (error: any) {
    status.value = error?.data?.detail || error?.message || 'Не удалось обновить GNSS источник'
  } finally {
    loading.value = false
  }
}

const deleteDataset = async () => {
  const item = selected.value
  if (!item) return
  if (process.client && !window.confirm(`Удалить GNSS источник "${item.name}" вместе со всеми точками?`)) return
  loading.value = true
  try {
    await apiFetch(`/api/devices/${props.deviceId}/gnss-data/${item.id}`, { method: 'DELETE' })
    datasets.value = datasets.value.filter((existing) => existing.id !== item.id)
    select(datasets.value[0]?.id || '')
    status.value = 'GNSS источник удален'
    emit('changed')
  } catch (error: any) {
    status.value = error?.data?.detail || error?.message || 'Не удалось удалить GNSS источник'
  } finally {
    loading.value = false
  }
}

const onFileChange = (event: Event) => {
  importFile.value = (event.target as HTMLInputElement).files?.[0] || null
}

const importData = async () => {
  if (!selected.value || !importFile.value) return
  importing.value = true
  status.value = 'Загружаю GNSS файл...'
  try {
    const form = new FormData()
    form.append('file', importFile.value)
    const response = await apiFetch<GnssDataImportResponse>(
      `/api/devices/${props.deviceId}/gnss-data/${selected.value.id}/import`,
      { method: 'POST', body: form },
    )
    await load()
    select(response.dataset.id)
    status.value = `GNSS импорт: ${response.upserted_rows} строк, дублей ${response.duplicate_rows}, пропущено ${response.skipped_rows}`
    importFile.value = null
    if (fileInput.value) fileInput.value.value = ''
    emit('changed')
  } catch (error: any) {
    const detail = error?.data?.detail
    status.value = Array.isArray(detail?.errors) ? detail.errors.join('; ') : detail || error?.message || 'Не удалось импортировать GNSS файл'
  } finally {
    importing.value = false
  }
}

watch(() => props.deviceId, () => {
  requestVersion += 1
  datasets.value = []
  selectedId.value = ''
  status.value = ''
  load()
})

onMounted(load)
</script>
