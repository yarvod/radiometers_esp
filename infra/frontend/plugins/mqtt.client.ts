import { defineNuxtPlugin, useRuntimeConfig } from '#app'
import type { MqttClient } from 'mqtt'
import mqtt from 'mqtt'

let client: MqttClient | null = null

export default defineNuxtPlugin((nuxtApp) => {
  if (process.server) {
    return
  }
  const config = useRuntimeConfig()
  const url = config.public.mqttUrl

  if (!client) {
    client = mqtt.connect(url, {
      protocolVersion: 4,
      reconnectPeriod: 3000,
      connectTimeout: 8000,
      clean: true,
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
