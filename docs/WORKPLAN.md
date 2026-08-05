# Active workplan: device page rollout smoke

S3 recovery backend implementation is complete locally. Deployment validation
must confirm the production MinIO credentials, per-device bucket configuration,
and a recovery run containing both an MQTT duplicate and an offline-only row:

- [ ] Apply migration `00023` and configure `APP_S3_ENDPOINT`,
  `APP_S3_ACCESS_KEY`, and `APP_S3_SECRET_KEY` for the ARQ worker.
- [ ] Enable recovery for one device, trigger an immediate run, and verify that
  the cursor/object ledger advances for both `radiometers/` and `meteo/`.
- [ ] Confirm an MQTT-present content fingerprint is skipped, an offline-only
  CSV row is inserted once, and two distinct rows in one ISO second are retained.

The corrective code pass has no open P1 findings after independent backend and
frontend review. Automated checks are green. One deployment-level verification
remains because it requires an authenticated running stack and real device data:

- [ ] Deploy the PostgreSQL wind-direction normalization fix and confirm the real
  `/api/meteo-readings` request no longer returns 500 in `ru1` backend logs.
- [ ] Exercise restart/external-power confirmations, PID state, calibration tab
  switching, GNSS import/refresh, and empty/populated meteo ranges in the browser.

This smoke check is not reproducible against the current local login page without a
running backend and test account. It must be completed before production rollout.
