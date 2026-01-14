// https://nuxt.com/docs/api/configuration/nuxt-config
export default defineNuxtConfig({
  devtools: { enabled: false },
  modules: ['@pinia/nuxt'],
  css: ['~/assets/main.css'],
  imports: {
    dirs: ['stores'],
  },
  runtimeConfig: {
    public: {
      mqttUrl: process.env.NUXT_PUBLIC_MQTT_URL || 'ws://localhost:9001',
      mqttUser: process.env.NUXT_PUBLIC_MQTT_USER || '',
      mqttPassword: process.env.NUXT_PUBLIC_MQTT_PASSWORD || '',
      apiBase: process.env.NUXT_PUBLIC_API_BASE || 'http://localhost:8000',
    },
  },
});
