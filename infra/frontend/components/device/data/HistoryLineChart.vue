<template>
  <div class="chart-box">
    <div class="chart-head-row">
      <h4>{{ definition.title }}</h4>
      <div class="chart-head-controls">
        <div class="axis-controls">
          <label class="checkbox axis-auto"><input v-model="axis.auto" type="checkbox" /> <span>Авто Y</span></label>
          <label class="compact">min<input v-model.number="axis.min" type="number" step="any" :disabled="axis.auto" /></label>
          <label class="compact">max<input v-model.number="axis.max" type="number" step="any" :disabled="axis.auto" /></label>
          <button class="btn ghost sm" type="button" @click="resetAxis">Сброс</button>
        </div>
        <slot name="controls" />
      </div>
    </div>
    <slot name="before" />
    <div v-if="axisError" class="axis-error">{{ axisError }}</div>
    <div class="chart-body"><canvas ref="canvas"></canvas></div>
  </div>
</template>

<script setup lang="ts">
import type { HistoryChartDefinition } from '~/types/charts'

const props = defineProps<{ definition: HistoryChartDefinition }>()
const canvas = ref<HTMLCanvasElement | null>(null)
const axis = reactive<{ auto: boolean; min: number | ''; max: number | '' }>({ auto: true, min: '', max: '' })
let ChartCtor: any = null
let chart: any = null

const axisNumber = (value: number | '') => value === '' || !Number.isFinite(Number(value)) ? undefined : Number(value)
const axisError = computed(() => {
  if (axis.auto) return ''
  const min = axisNumber(axis.min); const max = axisNumber(axis.max)
  return min !== undefined && max !== undefined && min >= max ? 'min должен быть меньше max' : ''
})
const yScale = () => {
  const result: Record<string, any> = { ticks: { maxTicksLimit: 6 } }
  if (!axis.auto && !axisError.value) {
    const min = axisNumber(axis.min); const max = axisNumber(axis.max)
    if (min !== undefined) result.min = min
    if (max !== undefined) result.max = max
  }
  return result
}
const options = () => ({
  responsive: true,
  maintainAspectRatio: false,
  animation: false,
  normalized: true,
  interaction: { mode: 'index', intersect: false },
  plugins: { legend: { position: 'top', align: 'start', labels: { boxWidth: 10, boxHeight: 10, padding: 12, usePointStyle: true } } },
  layout: { padding: { top: 4, right: 8, bottom: 4, left: 4 } },
  scales: { x: { ticks: { maxTicksLimit: 6, autoSkip: true, maxRotation: 0, minRotation: 0 }, grid: { display: false } }, y: yScale() },
})
const render = async () => {
  if (!ChartCtor || !canvas.value) return
  await nextTick()
  const datasets = props.definition.datasets.map((dataset) => ({
    ...dataset,
    data: Array.isArray(dataset.data) ? [...dataset.data] : dataset.data,
  }))
  if (!chart) {
    chart = new ChartCtor(canvas.value, { type: 'line', data: { labels: props.definition.labels, datasets }, options: options() })
    return
  }
  const hidden = new Map<string, boolean>()
  chart.data.datasets.forEach((dataset: any, index: number) => {
    if (dataset?.label) hidden.set(dataset.label, !chart.isDatasetVisible(index))
  })
  datasets.forEach((dataset) => { if (hidden.has(String(dataset.label))) dataset.hidden = hidden.get(String(dataset.label)) })
  chart.data.labels = props.definition.labels
  chart.data.datasets = datasets
  chart.options.scales.y = yScale()
  chart.update('none')
}
const resetAxis = () => Object.assign(axis, { auto: true, min: '', max: '' })
watch(() => props.definition, render, { deep: true })
watch(axis, render, { deep: true })
onMounted(async () => { const module: any = await import('chart.js/auto'); ChartCtor = module?.Chart || module?.default || module; render() })
onBeforeUnmount(() => { chart?.destroy(); chart = null })
</script>
