// https://nuxt.com/docs/api/configuration/nuxt-config
export default defineNuxtConfig({
  devtools: { enabled: false },
  modules: ['@pinia/nuxt'],
  imports: {
    dirs: ['stores'],
  },
  runtimeConfig: {
    public: {
      mqttUrl: process.env.NUXT_PUBLIC_MQTT_URL || 'ws://localhost:9001',
      mqttUser: process.env.NUXT_PUBLIC_MQTT_USER || '',
      mqttPassword: process.env.NUXT_PUBLIC_MQTT_PASSWORD || '',
    },
  },
});
