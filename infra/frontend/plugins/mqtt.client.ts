import { defineNuxtPlugin, useRuntimeConfig } from '#app'
import { connect, MqttClient } from 'mqtt'

let client: MqttClient | null = null

export default defineNuxtPlugin((nuxtApp) => {
  const config = useRuntimeConfig()
  const url = config.public.mqttUrl

  if (!client) {
    client = connect(url, {
      protocolVersion: 5,
      reconnectPeriod: 3000,
      connectTimeout: 8000,
    })
  }

  nuxtApp.provide('mqtt', client)
})

declare module '#app' {
  interface NuxtApp {
    $mqtt: MqttClient
  }
}
