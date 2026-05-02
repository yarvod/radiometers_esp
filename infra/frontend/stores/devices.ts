import { defineStore } from 'pinia'
import type { MqttClient } from 'mqtt'

type DeviceState = {
  id: string
  online: boolean
  lastSeen?: number
  state?: any
}

type PendingReq = {
  resolve: (val: any) => void
  reject: (err: any) => void
  timeout: ReturnType<typeof setTimeout>
}

const makeReqId = () => {
  if (globalThis.crypto?.randomUUID) {
    return globalThis.crypto.randomUUID()
  }
  return `web-${Date.now()}-${Math.random().toString(16).slice(2)}`
}

const parsePayload = (payload: Buffer) => {
  try {
    return JSON.parse(payload.toString())
  } catch (_) {
    return {}
  }
}

const normalizeDeviceState = (msg: any) => {
  if (!msg || typeof msg !== 'object') return {}
  const state: Record<string, any> = { ...msg }
  if (typeof msg.adc1 === 'number' && typeof state.voltage1 !== 'number') state.voltage1 = msg.adc1
  if (typeof msg.adc2 === 'number' && typeof state.voltage2 !== 'number') state.voltage2 = msg.adc2
  if (typeof msg.adc3 === 'number' && typeof state.voltage3 !== 'number') state.voltage3 = msg.adc3
  if (typeof msg.adc1Cal === 'number' && typeof state.voltage1_cal !== 'number') state.voltage1_cal = msg.adc1Cal
  if (typeof msg.adc2Cal === 'number' && typeof state.voltage2_cal !== 'number') state.voltage2_cal = msg.adc2Cal
  if (typeof msg.adc3Cal === 'number' && typeof state.voltage3_cal !== 'number') state.voltage3_cal = msg.adc3Cal
  if (typeof msg.busV === 'number' && typeof state.inaBusVoltage !== 'number') state.inaBusVoltage = msg.busV
  if (typeof msg.busI === 'number' && typeof state.inaCurrent !== 'number') state.inaCurrent = msg.busI
  if (typeof msg.busP === 'number' && typeof state.inaPower !== 'number') state.inaPower = msg.busP
  return state
}

export const useDevicesStore = defineStore('devices', {
  state: () => ({
    devices: new Map<string, DeviceState>(),
    pending: new Map<string, PendingReq>(),
    mqttReady: false,
    mqttOnline: false,
  }),
  actions: {
    init(mqtt?: MqttClient | null) {
      if (this.mqttReady || !mqtt) return
      this.mqttReady = true

      const subscribeTopics = () => {
        this.mqttOnline = true
        for (const topic of ['+/resp', '+/state', '+/measure', '+/error']) {
          mqtt.subscribe(topic, { qos: 0 }, (err) => {
            if (err) console.error(`MQTT subscribe ${topic} failed`, err)
          })
        }
        console.info('MQTT connected')
      }

      mqtt.on('connect', subscribeTopics)
      if (mqtt.connected) {
        subscribeTopics()
      }

      mqtt.on('message', (topic, payload) => {
        const parts = topic.split('/')
        if (parts.length < 2) return
        const deviceId = parts[0]
        const channel = parts[1]
        const msg = parsePayload(payload)

        if (channel === 'resp') {
          this.markOnline(deviceId)
          const reqId = msg.reqId
          if (reqId && this.pending.has(reqId)) {
            const p = this.pending.get(reqId)!
            clearTimeout(p.timeout)
            this.pending.delete(reqId)
            msg.ok ? p.resolve(msg) : p.reject(msg)
          }
          if (msg.data && deviceId) {
            this.setState(deviceId, msg.data)
          }
          return
        }

        if (channel === 'state' || channel === 'measure') {
          this.markOnline(deviceId)
          this.setState(deviceId, msg)
          console.debug(`${channel} update`, deviceId, msg)
          return
        }

        if (channel === 'error') {
          this.markOnline(deviceId)
        }
      })
      mqtt.on('error', (err) => console.error('MQTT error', err))
      mqtt.on('reconnect', () => console.warn('MQTT reconnecting...'))
      mqtt.on('close', () => {
        this.mqttOnline = false
        console.warn('MQTT closed')
      })
      mqtt.on('offline', () => {
        this.mqttOnline = false
        console.warn('MQTT offline')
      })
    },
    markOnline(id: string) {
      const prev = this.devices.get(id) || { id, online: false }
      prev.online = true
      prev.lastSeen = Date.now()
      this.devices.set(id, prev)
    },
    setState(id: string, state: any) {
      const prev = this.devices.get(id)
      this.devices.set(id, {
        ...(prev || { id }),
        id,
        state: { ...(prev?.state || {}), ...normalizeDeviceState(state) },
        online: true,
        lastSeen: Date.now(),
      })
    },
    sendCommand(mqtt: MqttClient | null | undefined, deviceId: string, cmd: any) {
      if (!mqtt) throw new Error('MQTT client not ready')
      if (!deviceId) throw new Error('Device id is empty')
      if (!mqtt.connected) throw new Error('MQTT не подключен')
      const reqId = makeReqId()
      const topic = `${deviceId}/cmd`
      const msg = JSON.stringify({ ...cmd, reqId })
      const p = new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
          this.pending.delete(reqId)
          reject(new Error(`MQTT command timeout: ${topic}`))
        }, 8000)
        this.pending.set(reqId, { resolve, reject, timeout })
      })
      mqtt.publish(topic, msg, { qos: 0 }, (err?: Error) => {
        if (!err) return
        const pending = this.pending.get(reqId)
        if (!pending) return
        clearTimeout(pending.timeout)
        this.pending.delete(reqId)
        pending.reject(err)
      })
      return p
    },
    async getState(mqtt: MqttClient, deviceId: string) {
      const res: any = await this.sendCommand(mqtt, deviceId, { type: 'get_state' })
      if (res?.data) {
        this.setState(deviceId, res.data)
      }
      return res?.data
    },
    async logStart(mqtt: MqttClient, deviceId: string, payload: { filename: string; useMotor: boolean; durationSec: number }) {
      return this.sendCommand(mqtt, deviceId, { type: 'log_start', ...payload })
    },
    async logStop(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'log_stop' })
    },
    async stepperMove(mqtt: MqttClient, deviceId: string, payload: { steps: number; reverse: boolean; speedUs: number }) {
      return this.sendCommand(mqtt, deviceId, { type: 'stepper_move', ...payload })
    },
    async stepperEnable(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'stepper_enable' })
    },
    async stepperDisable(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'stepper_disable' })
    },
    async stepperFindZero(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'stepper_find_zero' })
    },
    async stepperZero(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'stepper_zero' })
    },
    async heaterSet(mqtt: MqttClient, deviceId: string, power: number) {
      return this.sendCommand(mqtt, deviceId, { type: 'heater_set', power })
    },
    async fanSet(mqtt: MqttClient, deviceId: string, power: number) {
      return this.sendCommand(mqtt, deviceId, { type: 'fan_set', power })
    },
    async pidApply(mqtt: MqttClient, deviceId: string, payload: { setpoint: number; sensors: number[]; kp: number; ki: number; kd: number }) {
      return this.sendCommand(mqtt, deviceId, { type: 'pid_apply', ...payload })
    },
    async pidEnable(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'pid_enable' })
    },
    async pidDisable(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'pid_disable' })
    },
    async restartDevice(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'restart' })
    },
    async externalPowerSet(mqtt: MqttClient, deviceId: string, enabled: boolean) {
      return this.sendCommand(mqtt, deviceId, { type: 'external_power_set', enabled })
    },
    async externalPowerCycle(mqtt: MqttClient, deviceId: string, offMs = 1000) {
      return this.sendCommand(mqtt, deviceId, { type: 'external_power_cycle', offMs })
    },
    async configSyncInternalFlash(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'config_sync_internal_flash' })
    },
    async wifiApply(mqtt: MqttClient, deviceId: string, payload: { mode: string; ssid: string; password: string }) {
      return this.sendCommand(mqtt, deviceId, { type: 'wifi_apply', ...payload })
    },
    async netApply(mqtt: MqttClient, deviceId: string, payload: { mode: string; priority: string }) {
      return this.sendCommand(mqtt, deviceId, { type: 'net_apply', ...payload })
    },
    async gpsApply(mqtt: MqttClient, deviceId: string, payload: { mode: string; rtcmTypes: number[] }) {
      return this.sendCommand(mqtt, deviceId, { type: 'gps_apply', ...payload })
    },
    async gpsProbe(mqtt: MqttClient, deviceId: string) {
      return this.sendCommand(mqtt, deviceId, { type: 'gps_probe' })
    },
  },
})
