<template>
  <div class="card">
    <div class="card-head">
      <h3>Ошибки устройства</h3>
      <span class="badge">{{ total }}</span>
    </div>
    <div class="inline fields">
      <label class="compact">С
        <input type="datetime-local" class="date-input" v-model="filters.from" />
      </label>
      <label class="compact">По
        <input type="datetime-local" class="date-input" v-model="filters.to" />
      </label>
      <label class="compact">Статус
        <select v-model="filters.status">
          <option value="all">Все</option>
          <option value="active">Active</option>
          <option value="cleared">Cleared</option>
        </select>
      </label>
    </div>
    <div class="inline fields">
      <label class="compact">Название
        <input type="text" v-model="filters.name" placeholder="code" />
      </label>
      <label class="compact">Лимит
        <input type="number" min="20" max="1000" step="20" v-model.number="filters.limit" />
      </label>
      <label class="compact">Страница
        <input type="number" min="1" :max="pageCount" step="1" v-model.number="filters.page" />
      </label>
    </div>
    <div class="actions">
      <button class="btn primary sm" @click="load" :disabled="loading">Применить</button>
      <button class="btn ghost sm" @click="reset" :disabled="loading">Сбросить</button>
      <button class="btn ghost sm" @click="goToPage(filters.page - 1)" :disabled="loading || filters.page <= 1">←</button>
      <button class="btn ghost sm" @click="goToPage(filters.page + 1)" :disabled="loading || filters.page >= pageCount">→</button>
      <span class="muted" v-if="rangeLabel">{{ rangeLabel }}</span>
      <span class="muted" v-if="status">{{ status }}</span>
    </div>
    <p class="muted" v-if="loading">Загрузка...</p>
    <div v-else-if="events.length === 0" class="muted">Ошибок не найдено</div>
    <div v-else class="error-list">
      <div v-for="event in events" :key="event.id" class="error-item">
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
</template>

<script setup lang="ts">
import type { ErrorEvent, ErrorEventsResponse } from '~/types/device'
import { localInputToIso } from '~/utils/datetime'

const props = defineProps<{ deviceId: string }>()
const { apiFetch } = useApi()

const events = ref<ErrorEvent[]>([])
const loading = ref(false)
const status = ref('')
const total = ref(0)
const filters = reactive({ from: '', to: '', status: 'all', name: '', limit: 200, page: 1 })
const pageCount = computed(() => Math.max(1, Math.ceil(total.value / filters.limit)))
const offset = computed(() => Math.max(0, (filters.page - 1) * filters.limit))
const rangeLabel = computed(() => {
  if (total.value === 0) return ''
  return `${offset.value + 1}–${offset.value + events.value.length} из ${total.value}`
})

let requestVersion = 0

const load = async () => {
  if (!props.deviceId) return
  const version = ++requestVersion
  loading.value = true
  status.value = ''
  try {
    const params = new URLSearchParams({ limit: String(filters.limit), offset: String(offset.value) })
    const from = localInputToIso(filters.from)
    const to = localInputToIso(filters.to)
    if (from) params.set('from', from)
    if (to) params.set('to', to)
    if (filters.status !== 'all') params.set('status', filters.status)
    if (filters.name.trim()) params.set('name', filters.name.trim())
    const response = await apiFetch<ErrorEventsResponse>(`/api/devices/${props.deviceId}/errors?${params}`)
    if (version !== requestVersion) return
    events.value = response.items
    total.value = response.total
    filters.page = Math.min(filters.page, Math.max(1, Math.ceil(response.total / filters.limit)))
  } catch (error: any) {
    if (version === requestVersion) status.value = error?.message || 'Не удалось загрузить ошибки'
  } finally {
    if (version === requestVersion) loading.value = false
  }
}

const reset = () => {
  Object.assign(filters, { from: '', to: '', status: 'all', name: '', limit: 200, page: 1 })
  load()
}

const goToPage = (next: number) => {
  const page = Math.min(Math.max(next, 1), pageCount.value)
  if (page === filters.page) return
  filters.page = page
  load()
}

const severityClass = (severity: string) => {
  const level = severity.toLowerCase()
  if (level === 'critical' || level === 'error') return 'danger'
  if (level === 'warning') return 'warn'
  return 'cool'
}

const formatErrorTime = (timestamp: string, createdAt: string) => {
  const date = new Date(timestamp || createdAt)
  return Number.isNaN(date.getTime()) ? (timestamp || createdAt) : date.toLocaleString('ru-RU')
}

watch(() => props.deviceId, () => {
  requestVersion += 1
  events.value = []
  total.value = 0
  status.value = ''
  filters.page = 1
  load()
})

let activationCount = 0
onMounted(load)
onActivated(() => {
  if (activationCount > 0) load()
  activationCount += 1
})
</script>
