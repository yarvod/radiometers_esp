# Nuxt3 фронт для ESP (MQTT + MinIO)

Структура:
- `nuxt.config.ts` — конфиг с включением Nitro dev серверов.
- `package.json` — зависимости (`nuxt`, `mqtt`, `pinia`).
- `plugins/mqtt.client.ts` — подключение MQTT по WebSocket.
- `stores/devices.ts` — состояние устройств, подключение/подписки.
- `pages/index.vue` — список устройств.
- `pages/[deviceId].vue` — карточка устройства с кнопками/данными.

Запуск:
```bash
cd infra/frontend
npm install
npm run dev
```

Брокер MQTT:
- По умолчанию `ws://localhost:9001` (Mosquitto из docker-compose).
- Топики: `<deviceId>/cmd` для команд, `<deviceId>/resp` для ответов (reqId эхо).
- ESP уже реализует эти топики.

MinIO:
- Для скачивания логов лучше использовать пресайны от бэкенда; тут не реализовано.

Безопасность:
- В проде не оставляйте брокер открытым — добавьте auth/ACL или прокси.
