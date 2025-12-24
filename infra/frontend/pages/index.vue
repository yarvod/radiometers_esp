<template>
  <div class="page">
    <h1>Устройства</h1>
    <p>Кликайте на устройство, чтобы открыть управление.</p>
    <div class="device-list">
      <div
        v-for="dev in deviceArray"
        :key="dev.id"
        class="device-card"
        @click="go(dev.id)"
      >
        <div class="title">{{ dev.id }}</div>
        <div class="status" :class="{ online: dev.online }">
          {{ dev.online ? 'Online' : 'Offline' }}
        </div>
        <div class="meta" v-if="dev.lastSeen">
          Обновлено: {{ new Date(dev.lastSeen).toLocaleTimeString() }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'

const nuxtApp = useNuxtApp()
const devicesStore = useDevicesStore()
devicesStore.init(nuxtApp.$mqtt)

const deviceArray = computed(() => Array.from(devicesStore.devices.values()))

function go(id: string) {
  navigateTo(`/${id}`)
}
</script>

<style scoped>
.page { max-width: 800px; margin: 0 auto; padding: 24px; }
.device-list { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 12px; }
.device-card { border: 1px solid #ddd; border-radius: 8px; padding: 12px; cursor: pointer; transition: box-shadow 0.2s; }
.device-card:hover { box-shadow: 0 2px 8px rgba(0,0,0,0.1); }
.title { font-weight: 600; }
.status { margin-top: 4px; }
.status.online { color: #2ecc71; }
.meta { font-size: 12px; color: #888; }
</style>
