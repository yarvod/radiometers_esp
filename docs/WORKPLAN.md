# Active workplan: device page rollout smoke

The corrective code pass has no open P1 findings after independent backend and
frontend review. Automated checks are green. One deployment-level verification
remains because it requires an authenticated running stack and real device data:

- [ ] Exercise restart/external-power confirmations, PID state, calibration tab
  switching, GNSS import/refresh, and empty/populated meteo ranges in the browser.

This smoke check is not reproducible against the current local login page without a
running backend and test account. It must be completed before production rollout.
