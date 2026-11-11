# Implementation Plan: Proxy Second Port

## Overview

Add a second TCP proxy instance (default port 502) by reusing `proxy_start()`. Work is in C for ESP-IDF. Steps: add Kconfig options, make `proxy.c` logging and startup errors per-instance, then wire the second call and compile-time port checks into `main.c`. The design has no Correctness Properties section, so only unit/build/integration style tests are used.

## Tasks

- [x] 1. Add Kconfig options for the second proxy instance
  - [x] 1.1 Add options to `main/Kconfig.projbuild`
    - Add `SECOND_SERVER_LISTEN_PORT` (int, default 502, range 1 65535), `SECOND_PROXY_TARGET_ADDRESS` (string, default "10.0.0.2") and `SECOND_PROXY_TARGET_PORT` (int, default 502, range 1 65535) after `PROXY_TARGET_PORT` in the "Ethernet & Proxy Settings" menu
    - Give each a non-empty prompt and help text; port help texts state the range 1 to 65535
    - Leave existing options unchanged
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.6, 2.1, 2.3, 5.1_

  - [x] 1.2 Add options to `main/Kconfig.projbuild.example`
    - Add the same three options with identical names, types, ranges, prompts and help texts
    - Use default "192.168.4.2" for `SECOND_PROXY_TARGET_ADDRESS`
    - _Requirements: 1.5, 2.2, 5.1_

  - [ ]* 1.3 Write a script to compare the new Kconfig blocks
    - Add a small script (for example `scripts/check_kconfig_second_port.py`) that extracts the three new blocks from both files and checks they match except for the target address default
    - Check the expected defaults and ranges
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 2.1, 2.2_

- [x] 2. Make `proxy.c` safe and traceable for multiple instances
  - [x] 2.1 Add per-instance logging and task names in `main/proxy.c`
    - Add `PLOGI`, `PLOGW`, `PLOGE` macros that prefix `[port N]`
    - Replace all `ESP_LOGx` calls in `proxy_task` and `proxy_start` with them, using `cfg->listen_port` or the `listen_port` argument
    - Add a `uint16_t listen_port` parameter to `send_all()` and update its callers
    - Build the task name `"proxy_%u"` in a local `char name[16]` in `proxy_start()` and pass it to `xTaskCreate()`
    - Keep timeouts, keep-alive settings and the 1460-byte buffer unchanged
    - _Requirements: 4.1, 4.5, 5.3_

  - [x] 2.2 Add one-shot startup error logging in `main/proxy.c`
    - Log exactly one `[port N] startup failed: <step>` message for allocation (buffer or socket), bind (with errno) and listen (with errno)
    - Make sure each failure path releases everything already allocated (listener, buffer, `cfg`) and deletes only its own task
    - In `proxy_start()`, log the port and free `cfg` if `malloc` or `xTaskCreate` fails
    - _Requirements: 4.2, 4.4_

  - [x] 2.3 Update the doc comment in `main/proxy.h`
    - State that `proxy_start()` may be called several times for independent instances
    - Keep the signature `void proxy_start(uint16_t listen_port, const char *remote_ip, uint16_t remote_port)`
    - _Requirements: 5.2_

- [x] 3. Wire the second instance in `main/main.c`
  - [x] 3.1 Add compile-time port checks and the second `proxy_start()` call
    - Add `PORT_IN_RANGE` and `_Static_assert` checks: both listen ports in 1..65535 (messages name the setting) and `CONFIG_SECOND_SERVER_LISTEN_PORT != CONFIG_SERVER_LISTEN_PORT` ("must differ")
    - Call `proxy_start(CONFIG_SECOND_SERVER_LISTEN_PORT, CONFIG_SECOND_PROXY_TARGET_ADDRESS, CONFIG_SECOND_PROXY_TARGET_PORT)` after the existing call
    - Remove the obsolete "TODO: Make these configurable via menuconfig" comment
    - _Requirements: 3.1, 3.2, 6.1, 6.2, 6.3_

  - [ ]* 3.2 Write build tests for the port checks
    - Add a script that runs `idf.py build` with default config and expects exit 0
    - Add cases with equal ports and an out-of-range port (set via sdkconfig override) that expect a non-zero exit and the expected message, with no `.bin` produced
    - Restore the original sdkconfig afterwards
    - _Requirements: 2.3, 5.2, 6.1, 6.2, 6.3_

- [x] 4. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 5. Make the second proxy optional via `SECOND_PROXY_ENABLE`
  - [x] 5.1 Add the switch to `main/Kconfig.projbuild`
    - Add `config SECOND_PROXY_ENABLE` (bool, default y, non-empty prompt and help) in the "Ethernet & Proxy Settings" menu before `SECOND_SERVER_LISTEN_PORT`
    - Add `depends on SECOND_PROXY_ENABLE` to `SECOND_SERVER_LISTEN_PORT`, `SECOND_PROXY_TARGET_ADDRESS` and `SECOND_PROXY_TARGET_PORT`
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 7.1, 7.3, 7.4_

  - [x] 5.2 Add the switch to `main/Kconfig.projbuild.example`
    - Add the identical `SECOND_PROXY_ENABLE` block and the same `depends on` lines
    - Keep `default "192.168.4.2"` for `SECOND_PROXY_TARGET_ADDRESS`
    - _Requirements: 1.5, 7.2, 7.3, 7.4_

  - [x] 5.3 Guard the second proxy in `main/main.c`
    - Wrap the second `proxy_start()` call in `#if CONFIG_SECOND_PROXY_ENABLE ... #endif`
    - Wrap the `_Static_assert` checks that reference `CONFIG_SECOND_SERVER_LISTEN_PORT` (range and "must differ") in `#if CONFIG_SECOND_PROXY_ENABLE ... #endif`
    - Keep the `CONFIG_SERVER_LISTEN_PORT` range check unconditional
    - Make sure no `CONFIG_SECOND_*` symbol is referenced outside the guard
    - _Requirements: 3.2, 6.1, 6.2, 6.3, 6.4, 7.5, 7.6, 7.7_

  - [x] 5.4 Document the option in `README.md`
    - Add a section naming `SECOND_PROXY_ENABLE` (default y), the three Second_Port_Options and their defaults, and that they are visible only when the switch is set
    - _Requirements: 7.8_

  - [ ]* 5.5 Extend the Kconfig comparison script and build tests
    - Extend `scripts/check_kconfig_second_port.py` (from task 1.3) to check `SECOND_PROXY_ENABLE` (bool, default y) and the `depends on` lines in both files
    - Extend the build test script (from task 3.2) with a case that builds with `CONFIG_SECOND_PROXY_ENABLE` unset (exit 0, also with equal port values) and checks the default build has it set
    - Restore the original sdkconfig afterwards
    - _Requirements: 1.5, 6.4, 7.1, 7.2, 7.6_

- [x] 6. Final checkpoint - Build with the switch on and off
  - Run `idf.py build` with the default configuration (switch enabled) and with `SECOND_PROXY_ENABLE` disabled; both must exit 0
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Hardware integration tests listed in the design (parallel echo, unreachable target, log attribution, 30 s inactivity) need a device and are run manually, so they are not coding tasks here
- No property tests: the design has no Correctness Properties section

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "1.2", "2.1", "2.3"] },
    { "id": 1, "tasks": ["1.3", "2.2", "3.1"] },
    { "id": 2, "tasks": ["3.2", "5.1", "5.2", "5.4"] },
    { "id": 3, "tasks": ["5.3"] },
    { "id": 4, "tasks": ["5.5"] }
  ]
}
```
