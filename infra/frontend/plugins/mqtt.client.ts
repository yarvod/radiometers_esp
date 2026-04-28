import { defineNuxtPlugin, useRuntimeConfig } from '#app'
import type { MqttClient } from 'mqtt'
import mqtt from 'mqtt'

let client: MqttClient | null = null

const localHosts = new Set(['localhost', '127.0.0.1', '[::1]', '::1'])

function sameOriginMqttUrl() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${window.location.host}/mqtt`
}

function resolveMqttUrl(raw: string) {
  if (!process.client) return raw
  const trimmed = (raw || '').trim()
  if (!trimmed) return sameOriginMqttUrl()

  const pageHost = window.location.hostname
  const pageIsLocal = localHosts.has(pageHost)
  const parsed = new URL(trimmed, window.location.href)
  const configuredIsLocal = localHosts.has(parsed.hostname)

  if (configuredIsLocal && !pageIsLocal) {
    return sameOriginMqttUrl()
  }
  if (window.location.protocol === 'https:' && parsed.protocol === 'ws:') {
    parsed.protocol = 'wss:'
  }
  if (!parsed.pathname || parsed.pathname === '/') {
    parsed.pathname = '/mqtt'
  }
  return parsed.toString()
}

export default defineNuxtPlugin((nuxtApp) => {
  if (process.server) {
    return
  }
  const config = useRuntimeConfig()
  const url = resolveMqttUrl(config.public.mqttUrl)

  if (!client) {
    client = mqtt.connect(url, {
      protocolVersion: 4,
      reconnectPeriod: 3000,
      connectTimeout: 8000,
      clean: true,
      resubscribe: true,
      queueQoSZero: false,
      username: config.public.mqttUser || undefined,
      password: config.public.mqttPassword || undefined,
    })
  }

  nuxtApp.provide('mqtt', client)
})

declare module '#app' {
  interface NuxtApp {
    $mqtt: MqttClient
  }
}
