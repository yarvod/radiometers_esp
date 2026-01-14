<template>
  <div class="page">
    <div class="header">
      <div>
        <h1>Устройства</h1>
        <p class="muted">Кликайте на устройство, чтобы открыть управление.</p>
      </div>
    </div>
    <p class="muted" v-if="createStatus">{{ createStatus }}</p>
    <div class="device-list">
      <div
        v-for="dev in deviceArray"
        :key="dev.id"
        class="device-card card"
        @click="go(dev.id)"
      >
        <div class="device-head">
          <div class="title">{{ dev.display_name || dev.id }}</div>
          <span class="chip" :class="{ online: dev.online }">
            {{ dev.online ? 'Online' : 'Offline' }}
          </span>
        </div>
        <div class="meta" v-if="dev.lastSeen">
          Обновлено: {{ new Date(dev.lastSeen).toLocaleTimeString() }}
        </div>
        <div class="meta muted" v-else-if="dev.dbSeen">
          В базе: {{ new Date(dev.dbSeen).toLocaleString() }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'

definePageMeta({ layout: 'admin' })

const { apiFetch } = useApi()
const nuxtApp = useNuxtApp()
const devicesStore = useDevicesStore()
devicesStore.init(nuxtApp.$mqtt)

const dbDevices = ref<any[]>([])
const createStatus = ref('')

const deviceArray = computed(() => {
  const fromMqtt = new Map(Array.from(devicesStore.devices.values()).map((dev) => [dev.id, dev]))
  return dbDevices.value.map((db) => {
    const live = fromMqtt.get(db.id)
    return {
      id: db.id,
      display_name: db.display_name,
      online: live?.online ?? false,
      lastSeen: live?.lastSeen,
      dbSeen: db.last_seen_at,
    }
  })
})

async function loadDevices() {
  try {
    dbDevices.value = await apiFetch<any[]>('/api/devices')
  } catch (e: any) {
    createStatus.value = e?.message || 'Не удалось загрузить список устройств'
  }
}

function go(id: string) {
  navigateTo(`/${id}`)
}

onMounted(loadDevices)
</script>

<style scoped>
.page { max-width: 1100px; margin: 0 auto; padding: 24px; display: flex; flex-direction: column; gap: 12px; }
.header { display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
.muted { color: var(--muted); }
.device-list { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 14px; }
.device-card { cursor: pointer; transition: transform 0.12s ease, box-shadow 0.12s ease; border-radius: 14px; }
.device-card:hover { transform: translateY(-3px); box-shadow: 0 10px 22px rgba(0,0,0,0.08); }
.device-head { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
.title { font-weight: 700; font-size: 18px; color: #1f2933; }
.meta { font-size: 13px; color: var(--muted); margin-top: 8px; }
.chip { padding: 6px 10px; border-radius: 999px; background: #f1f3f4; border: 1px solid var(--border); color: var(--muted); font-weight: 600; }
.chip.online { background: #e6f8ed; color: #2e8b57; border-color: rgba(46, 139, 87, 0.35); }
</style>
