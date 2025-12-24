import { defineNuxtPlugin, useRuntimeConfig } from '#app'
import { connect, MqttClient } from 'mqtt'

let client: MqttClient | null = null

export default defineNuxtPlugin((nuxtApp) => {
  if (process.server) {
    return
  }
  const config = useRuntimeConfig()
  const url = config.public.mqttUrl

  if (!client) {
    client = connect(url, {
      protocolVersion: 5,
      reconnectPeriod: 3000,
      connectTimeout: 8000,
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
