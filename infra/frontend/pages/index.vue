<template>
  <div class="page">
    <h1>Устройства</h1>
    <p class="muted">Кликайте на устройство, чтобы открыть управление.</p>
    <div class="device-list">
      <div
        v-for="dev in deviceArray"
        :key="dev.id"
        class="device-card card"
        @click="go(dev.id)"
      >
        <div class="device-head">
          <div class="title">{{ dev.id }}</div>
          <span class="chip" :class="{ online: dev.online }">
            {{ dev.online ? 'Online' : 'Offline' }}
          </span>
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
.page { max-width: 900px; margin: 0 auto; padding: 24px; display: flex; flex-direction: column; gap: 12px; }
.muted { color: var(--muted); }
.device-list { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 14px; }
.device-card { cursor: pointer; transition: transform 0.12s ease, box-shadow 0.12s ease; border-radius: 14px; }
.device-card:hover { transform: translateY(-3px); box-shadow: 0 10px 22px rgba(0,0,0,0.08); }
.device-head { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
.title { font-weight: 700; font-size: 18px; color: #1f2933; }
.meta { font-size: 13px; color: var(--muted); margin-top: 8px; }
.chip { padding: 6px 10px; border-radius: 999px; background: #f1f3f4; border: 1px solid var(--border); color: var(--muted); font-weight: 600; }
.chip.online { background: #e6f8ed; color: #2e8b57; border-color: rgba(46, 139, 87, 0.35); }
</style>
