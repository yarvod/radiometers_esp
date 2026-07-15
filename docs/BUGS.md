# Known Bugs

## Open

| # | Component | Description | Severity |
|---|-----------|-------------|----------|
| 1 | Firmware / WN90LP | Driver not yet wired into `app_main.cpp` — `meteo_client.initUart()` / `startTask()` not called | High |

## Closed

| # | Component | Description | Fix |
|---|-----------|-------------|-----|
| 2 | Firmware / WN90LP | `wn90lp.cpp` missing from `main/CMakeLists.txt` — file was not compiled | Added `"wn90lp.cpp"` to SRCS |
| 3 | Firmware / WN90LP | `UART_HW_FLOWCTRL_RTS` set incorrectly — RS485 direction is handled by `uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`, not flow control | Changed to `UART_HW_FLOWCTRL_DISABLE` |
| 4 | Firmware / WN90LP | `TAG` name conflict with `inline constexpr char TAG[]` in `app_state.h` | Renamed to `kTag` in `wn90lp.cpp` |
| 5 | Firmware / WN90LP | Task stack 4096 bytes — insufficient for `std::string` + file I/O | Increased to 8192 bytes |
| 6 | Firmware / GNSS | After receiver reconfiguration, COM2 emitted ZDA and RTCM but no periodic GGA, so the UI position cache stayed empty outside one-shot measurement requests | Added `GPGGA COM2 1` to periodic output setup and made NMEA sentence detection accept both GP/GN talker IDs |
