import { defineStore } from 'pinia'
import type { MqttClient, ISubscriptionGrant } from 'mqtt'

type DeviceState = {
  id: string
  online: boolean
  lastSeen?: number
  state?: any
}

type PendingReq = {
  resolve: (val: any) => void
  reject: (err: any) => void
  timeout: NodeJS.Timeout
}

export const useDevicesStore = defineStore('devices', {
  state: () => ({
    devices: new Map<string, DeviceState>(),
    pending: new Map<string, PendingReq>(),
    mqttReady: false,
  }),
  actions: {
    init(mqtt?: MqttClient | null) {
      if (this.mqttReady || !mqtt) return
      this.mqttReady = true

      mqtt.on('connect', () => {
        mqtt.subscribe("+/resp")
        mqtt.subscribe("+/state")
        console.info('MQTT connected')
      })

      mqtt.on('message', (topic, payload) => {
        const parts = topic.split('/')
        if (parts.length >= 2 && parts[1] === 'resp') {
          const deviceId = parts[0]
          const txt = payload.toString()
          let msg: any = {}
          try { msg = JSON.parse(txt) } catch (_) {}
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
        }
        if (parts.length >= 2 && parts[1] === 'state') {
          const deviceId = parts[0]
          this.markOnline(deviceId)
          const txt = payload.toString()
          let msg: any = {}
          try { msg = JSON.parse(txt) } catch (_) {}
          this.setState(deviceId, msg)
          console.debug('state update', deviceId, msg)
        }
      })
      mqtt.on('error', (err) => console.error('MQTT error', err))
      mqtt.on('reconnect', () => console.warn('MQTT reconnecting...'))
      mqtt.on('close', () => console.warn('MQTT closed'))
    },
    markOnline(id: string) {
      const prev = this.devices.get(id) || { id, online: false }
      prev.online = true
      prev.lastSeen = Date.now()
      this.devices.set(id, prev)
    },
    setState(id: string, state: any) {
      const prev = this.devices.get(id) || { id, online: true }
      prev.state = state
      prev.online = true
      prev.lastSeen = Date.now()
      this.devices.set(id, prev)
    },
    sendCommand(mqtt: MqttClient | null | undefined, deviceId: string, cmd: any) {
      if (!mqtt) throw new Error('MQTT client not ready')
      const reqId = crypto.randomUUID()
      const topic = `${deviceId}/cmd`
      const msg = JSON.stringify({ ...cmd, reqId })
      const p = new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
          this.pending.delete(reqId)
          reject(new Error('timeout'))
        }, 8000)
        this.pending.set(reqId, { resolve, reject, timeout })
      })
      mqtt.publish(topic, msg, { qos: 0 })
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
    async heaterSet(mqtt: MqttClient, deviceId: string, power: number) {
      return this.sendCommand(mqtt, deviceId, { type: 'heater_set', power })
    },
    async fanSet(mqtt: MqttClient, deviceId: string, power: number) {
      return this.sendCommand(mqtt, deviceId, { type: 'fan_set', power })
    },
  },
})
