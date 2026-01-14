<template>
  <div class="page devices-page">
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
useHead({ title: 'Устройства' })

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
