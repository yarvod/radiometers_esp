# Active workplan: device page rollout smoke

S3 recovery backend and migration `00023` are deployed on production. The API,
MQTT worker, and ARQ worker are healthy; timestamp-only uniqueness has been
replaced with content fingerprints without deleting same-second measurements.
The remaining rollout validation must cover production MinIO access and a real
recovery run:

- [x] Apply migration `00023` and verify healthy backend, MQTT, and ARQ
  containers after the restart-safe recovery from the partial first attempt.
- [ ] Confirm `APP_S3_ENDPOINT`, `APP_S3_ACCESS_KEY`, and `APP_S3_SECRET_KEY`
  against the production MinIO bucket for the pilot device.
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
