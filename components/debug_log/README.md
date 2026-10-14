<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# debug_log

Optional live log over Wi-Fi. Mirrors the ESP-IDF log output into a ring
buffer of 500 lines and shows it in a **Debug** tab of the web interface, so
the device can be diagnosed without a USB connection.

## Behaviour

- `debug_log_init()` installs an `esp_log_set_vprintf()` hook. The original
  output function is always called first, so console output is unchanged.
- Log messages arrive in several chunks; lines are assembled and committed on
  each newline. ANSI colour codes are stripped.
- The buffer is allocated in PSRAM and guarded by a spinlock. The hook never
  allocates, never blocks and never logs, so recording, test plans, the web UI
  and SD access are never delayed. Capturing has the lowest priority: the hook
  tries the spinlock a single time and discards the chunk when a reader or a
  clear request holds it, rather than waiting. Discarded chunks are counted
  (`debug_log_get_dropped()`), reported as `dropped` by the status endpoints and
  shown in the Debug tab; the partially assembled line is dropped as well so
  unrelated text is never merged.
- Lines longer than `DEBUG_LOG_LINE_MAX` are truncated; the oldest line is
  overwritten when the buffer is full.
- Disabled by default. The flag is stored in NVS (`debug_log` / `enabled`) and
  applied by `debug_log_web_start()`. Capturing runs until then so start-up
  messages are kept when the feature is enabled. Disabling frees the buffer.

Bootloader output, messages logged before `app_main()` and plain `printf()`
output are not captured and still require the USB console.

## Integration

```c
debug_log_init();        /* first statement of app_main() */
debug_log_web_init();    /* before the web server starts */
device_app_start();
debug_log_web_start();   /* applies the persisted setting, needs NVS */
```

Add `debug` to `CONFIG_OTA_SUITE_WEB_TAB_ORDER` to place the tab.

## HTTP API

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/debug/status` | GET | Buffer cursor and persisted enable flag |
| `/api/debug/settings` | POST | `{"enabled":true\|false}` |
| `/api/debug/log?after=<seq>&max=<n>` | GET | Up to 120 lines newer than `seq` |
| `/api/debug/clear` | POST | Drops buffered lines |

All endpoints require an authenticated session; POST routes also require the
CSRF token from `/api/status`.
