<template>
  <div class="chart-stack">
    <HistoryLineChart v-for="definition in definitions" :key="definition.key" :definition="definition" />
  </div>
</template>

<script setup lang="ts">
import HistoryLineChart from './HistoryLineChart.vue'
import type { HistoryChartDefinition } from '~/types/charts'
defineProps<{ definitions: HistoryChartDefinition[] }>()
</script>
