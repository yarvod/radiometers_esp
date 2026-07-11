<template>
  <div class="chart-stack">
    <HistoryLineChart :definition="teff" />
    <HistoryLineChart :definition="tau">
      <template #controls>
        <div class="inline fields compact-controls">
          <label class="checkbox"><input :checked="average" type="checkbox" :disabled="stations.length === 0" @change="$emit('update:average', ($event.target as HTMLInputElement).checked)" /><span>Усреднить</span></label>
          <label class="compact">Станция
            <select :value="primaryStation" :disabled="average || stations.length === 0" @change="$emit('update:primaryStation', ($event.target as HTMLSelectElement).value)">
              <option v-for="station in stations" :key="station.station_id" :value="station.station_id">{{ stationLabel(station) }}</option>
            </select>
          </label>
        </div>
      </template>
    </HistoryLineChart>
    <HistoryLineChart :definition="pwv">
      <template #controls><span v-if="pwvStatus" class="muted">{{ pwvStatus }}</span></template>
      <template #before>
        <div class="gnss-panel">
          <div v-if="gnssDatasets.length" class="chip-select">
            <button v-for="item in gnssDatasets" :key="item.id" class="chip-option" :class="{ selected: selectedGnssIds.includes(item.id) }" type="button" @click="$emit('toggle-gnss', item.id)">{{ item.name }} · {{ item.measurement_count }}</button>
          </div>
          <p v-else class="muted">GNSS источники добавляются во вкладке GPS</p>
          <p v-if="gnssStatus" class="muted">{{ gnssStatus }}</p>
        </div>
      </template>
    </HistoryLineChart>
  </div>
</template>

<script setup lang="ts">
import HistoryLineChart from './HistoryLineChart.vue'
import type { GnssData } from '~/types/device'
import type { StationOption } from '~/types/device-data'
import type { HistoryChartDefinition } from '~/types/charts'

defineProps<{
  teff: HistoryChartDefinition
  tau: HistoryChartDefinition
  pwv: HistoryChartDefinition
  average: boolean
  primaryStation: string
  stations: StationOption[]
  gnssDatasets: GnssData[]
  selectedGnssIds: string[]
  gnssStatus: string
  pwvStatus: string
}>()
defineEmits<{ 'update:average': [value: boolean]; 'update:primaryStation': [value: string]; 'toggle-gnss': [id: string] }>()
const stationLabel = (station: StationOption) => station.name ? `${station.station_id} · ${station.name}` : station.station_id
</script>
