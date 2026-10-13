# Полный ежедневный бэкап радиометра в Google Drive

Архив `radiometer-full-YYYYMMDDTHHMMSSZ.tar.zst` содержит согласованный
PostgreSQL custom dump, SQL глобальных ролей для ручного восстановления
дополнительных ролей, полный volume MinIO (объекты, версии, метаданные, IAM),
постоянные данные MQTT/Redis/экспортов, точные Docker-образы всех сервисов,
`.env`, Compose, сертификаты, скрипты и эту инструкцию. Сохраняются конфиги
Nginx радиометра/S3 и сертификаты Let's Encrypt трёх доменов.
Статическая страница `/radiometer-backup-privacy` обеспечивает ссылку на
политику использования Google Drive для публикации OAuth-приложения.
Её исходник `infra/backup-privacy.html` хранится в Git, а Nginx отдаёт
копию из `/var/www/radiometer-backup/privacy.html`, чтобы не открывать
доступ к домашней директории пользователя веб-серверу.

Архив содержит пароли и приватные ключи. Серверные файлы доступны только root;
папку Google Drive нельзя делать публичной.

## Согласованность и расписание

На время dump базы и копирования volumes останавливаются только `backend`,
`worker`, `arq-worker`, `mosquitto`, `minio`, `redis` радиометра. PostgreSQL
и frontend остаются запущены. API, MQTT и S3 временно недоступны; длительность
записывается в `manifest.json`. Образы, сжатие и загрузка обрабатываются при
работающих сервисах. Каждый архив полный, не инкрементальный.

Таймер запускается ежедневно в **04:00 Europe/Moscow**. `Persistent=true`
догоняет пропущенный запуск после включения сервера. Блокировка исключает
параллельные бэкапы; `finally` и `ExecStopPost` возвращают остановленные
контейнеры после ошибки или принудительного завершения.

Локально хранятся две последние успешные копии; неудачные загрузки автоматически
не удаляются. На Drive автоудаления нет: следите за свободной квотой.
Перед снимком проверяются авторизация и минимум 4 GiB свободного места.
Успех загрузки подтверждается readback размера и MD5 каждого файла.
SHA256 архива лежит рядом в отдельном файле с относительным именем.

## Google-приложение и rclone

В своём Google Cloud-проекте включите `drive.googleapis.com`. Создайте приложение
Google Auth Platform и OAuth client **Desktop app**. Для долговременной работы
используйте **In production**: в Testing refresh token со scope Drive обычно
истекает через семь дней. Service account не подходит для личного My Drive:
у него нет квоты на владение файлами.

В rclone remote `radiometer-gdrive` укажите свои `client_id`/`client_secret`,
scope **`drive.file`**, и один раз подтвердите Google OAuth. Токен хранится
в `infra/backup-secrets/rclone.conf` с правами `600`. Access token автоматически
обновляется через refresh token; пароль Google серверу не нужен.
Не выводите конфиг и токены в логи или чат.

Scope открывает только созданные приложением или явно выбранные через Google
Picker файлы. Приложение само создаёт `radiometer-backups`; затем её ID
записывается в `root_folder_id` remote, и цель загрузки — `radiometer-gdrive:`.

## Установка на Linux

Нужны root, Docker Compose v2, Python >=3.10, tar, zstd, rclone.
Код хранится в Git в `/home/devuser/radiometers_esp/infra/scripts/`.
После обновления через `git push` и `git pull` скопируйте
`infra/full-backup.env.example` в `infra/backup.env` с правами `600`.
OAuth JSON и rclone-конфиг лежат в `infra/backup-secrets/`, архивы — в
`infra/backups/`. Эти пути исключены из Git. Файлы с секретами принадлежат root.

Установите service/timer из `infra/scripts/` в `/etc/systemd/system/`:

```bash
systemctl daemon-reload
systemctl start radiometer-backup.service
systemctl enable --now radiometer-backup.timer
systemctl list-timers radiometer-backup.timer
journalctl -u radiometer-backup.service --no-pager
cat /home/devuser/radiometers_esp/infra/backups/.state/last-success.json
```

Для страницы приватности установите `infra/backup-privacy.html` в
`/var/www/radiometer-backup/privacy.html` и подключите
`infra/scripts/nginx-backup-privacy.conf` внутри HTTPS `server` сайта;
проверьте `nginx -t` и перезагрузите Nginx. URL должен отвечать HTTP 200.

Явный локальный запуск без загрузки:

```bash
python3 /home/devuser/radiometers_esp/infra/scripts/full_backup.py --no-upload
```

`last-success.json` содержит время, архив, размер, длительность остановки и
признак проверенной загрузки. Ошибки дают ненулевой статус systemd и видны в журнале.

## Восстановление на свежем сервере

1. Установите Docker/Compose v2, Python >=3.10 и zstd. Для доменов — Nginx/Certbot.
   Скачайте с Drive архив **и соответствующий .sha256** в одну папку.
2. Проверьте checksum и распакуйте в пустой закрытый каталог:

   ```bash
   sha256sum -c radiometer-full-YYYYMMDDTHHMMSSZ.tar.zst.sha256
   install -d -m 700 /root/radiometer-restore
   tar --zstd -xf radiometer-full-YYYYMMDDTHHMMSSZ.tar.zst -C /root/radiometer-restore
   ```

3. Восстановите данные и запустите все сервисы одной командой:

   ```bash
   python3 /root/radiometer-restore/restore.py --target /home/devuser/radiometers_esp/infra
   docker compose -f /home/devuser/radiometers_esp/infra/compose.restore.json ps
   ```

   Скрипт отказывается перезаписывать непустой target, существующие контейнеры
   проекта и именованные volumes. Образы загружаются из архива; GHCR не нужен.
   Дополнительные PostgreSQL-роли при необходимости восстановите вручную из
   `postgres/globals.sql`; основной пользователь базы создаётся из Compose.
4. Добавьте `--host-config` к команде восстановления для установки сохранённых
   доменных конфигов Nginx/сертификатов. Режим требует отсутствия соответствующих
   файлов и каталогов на новом сервере. Существующие конфиги объединяйте вручную
   из `host/`. Общие nginx.conf/mime.types/snippets сохранены для справки и
   автоматически не заменяют установленный Nginx. Обновите DNS на новый IP,
   проверьте HTTPS и возобновите продление Certbot.
5. Верните ежедневный бэкап по разделу установки: новый `BACKUP_INFRA`,
   `COMPOSE_FILE=compose.restore.json`, повторная авторизация Drive и таймер.

## Проверка восстановления без влияния на production

```bash
python3 /root/radiometer-restore/restore.py \
  --target /root/radiometer-restore-check/infra \
  --project radiometer-restore-check --isolated
```

`--isolated` убирает опубликованные порты и запрещает внешний доступ сети.
Проверяйте БД, MinIO, Redis, MQTT и API через docker exec. Реальные устройства
в тестовую сеть не подключаются. После проверки удалите только тестовый
project/volumes, сохранив исходный архив.

Для `radiometer-full-*` используйте `restore.py`, лежащий внутри архива.
Прежние частичные архивы `radiometer-backup-*.tar.*` не подходят для этого
сценария и требуют ручного восстановления PostgreSQL/S3.
