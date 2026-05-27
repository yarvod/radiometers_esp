# Бэкапы Postgres и MinIO в Google Drive

Схема сделана логической, а не через копирование docker volumes:

- Postgres сохраняется через `pg_dump` в custom dump.
- MinIO сохраняется через S3 API: все бакеты зеркалируются во временную папку и попадают в один архив.
- Архив и `.sha256` можно автоматически отправлять в личный Google Drive через `rclone`.

## Настройка на сервере

1. Установите `rclone` на сервер.
2. Настройте Google Drive remote:

   ```bash
   rclone config
   ```

   Создайте remote, например `gdrive`, с типом `drive`. Если сервер без браузера, `rclone` предложит авторизоваться на локальной машине и вставить token обратно на сервер.

3. Создайте конфиг бэкапов:

   ```bash
   cd /path/to/radiometer_termo/infra
   cp backup.env.example backup.env
   nano backup.env
   ```

   Минимально проверьте:

   ```bash
   BACKUP_RCLONE_REMOTE=gdrive:radiometer-backups
   BACKUP_UPLOAD=1
   ```

4. Запустите первый бэкап вручную:

   ```bash
   ./scripts/backup.sh
   ```

## Автоматический запуск

Пример cron на ежедневный бэкап в 03:10 UTC:

```cron
10 3 * * * cd /path/to/radiometer_termo/infra && ./scripts/backup.sh >> /var/log/radiometer-backup.log 2>&1
```

Проверка списка архивов в Google Drive:

```bash
rclone ls gdrive:radiometer-backups
```

## Восстановление

1. Поднимите свежий compose-стек с тем же `.env`.
2. Скачайте нужный архив:

   ```bash
   cd /path/to/radiometer_termo/infra
   rclone copy gdrive:radiometer-backups/radiometer-backup-YYYYMMDDTHHMMSSZ.tar.zst ./backups/
   rclone copy gdrive:radiometer-backups/radiometer-backup-YYYYMMDDTHHMMSSZ.tar.zst.sha256 ./backups/
   ```

3. Проверьте checksum:

   ```bash
   cd backups
   sha256sum -c radiometer-backup-YYYYMMDDTHHMMSSZ.tar.zst.sha256
   cd ..
   ```

   На macOS вместо `sha256sum` используйте `shasum -a 256 -c ...`.

4. Восстановите данные:

   ```bash
   ./scripts/restore.sh ./backups/radiometer-backup-YYYYMMDDTHHMMSSZ.tar.zst
   ```

`restore.sh` по умолчанию останавливает `backend`, `worker` и `arq-worker`, очищает схему `public` в Postgres, восстанавливает dump, зеркалит бакеты в MinIO и снова запускает app-сервисы.

Для частичного восстановления:

```bash
./scripts/restore.sh ./backups/radiometer-backup-YYYYMMDDTHHMMSSZ.tar.zst --postgres-only
./scripts/restore.sh ./backups/radiometer-backup-YYYYMMDDTHHMMSSZ.tar.zst --s3-only
```

## Важные замечания

- Архив содержит все данные базы и объектов S3, поэтому его надо считать секретным.
- Первое S3-зеркалирование может быть долгим; следующие бэкапы тоже создают полный архив, зато восстановление простое и быстрое.
- Если данных станет много, следующий шаг по скорости и месту - перейти на `restic` через `rclone`, но для текущего infra один переносимый архив проще обслуживать.
