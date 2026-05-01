<template>
  <div class="card gps-panel">
    <div class="card-head">
      <h3>GPS / GNSS</h3>
      <span class="badge" :class="{ success: hasGps }">{{ hasGps ? 'Detected' : 'Not detected' }}</span>
    </div>

    <div class="status-row">
      <span class="chip strong">Port: COM2</span>
      <span class="chip subtle">Config mode: {{ form.mode }}</span>
      <span class="chip" :class="{ online: actualMode }">Actual mode: {{ actualMode || '--' }}</span>
    </div>

    <div class="form-group">
      <label>Mode</label>
      <select :value="form.mode" @change="onModeChange">
        <option value="base_time_60">BASE TIME 60</option>
        <option value="base">BASE</option>
        <option value="rover_uav">ROVER UAV</option>
        <option value="keep">Keep current</option>
      </select>
    </div>

    <div class="form-group">
      <label>RTCM codes</label>
      <input :value="form.rtcmText" placeholder="1004,1006,1033" @input="onRtcmInput" />
      <p class="muted">По умолчанию: 1004, 1006, 1033. Команда применяется на Unicore COM2 через MQTT.</p>
    </div>

    <div class="actions">
      <button class="btn primary" :disabled="saving" @click="$emit('save')">Сохранить и применить</button>
      <button class="btn ghost" :disabled="saving" @click="$emit('probe')">Обновить actual mode</button>
    </div>

    <p class="muted" v-if="status">{{ status }}</p>
  </div>
</template>

<script setup lang="ts">
defineProps<{
  hasGps: boolean
  actualMode: string
  form: { mode: string; rtcmText: string }
  saving: boolean
  status: string
}>()

const emit = defineEmits<{
  save: []
  probe: []
  'update:mode': [value: string]
  'update:rtcmText': [value: string]
}>()

const onModeChange = (event: Event) => {
  emit('update:mode', (event.target as HTMLSelectElement).value)
}

const onRtcmInput = (event: Event) => {
  emit('update:rtcmText', (event.target as HTMLInputElement).value)
}
</script>
