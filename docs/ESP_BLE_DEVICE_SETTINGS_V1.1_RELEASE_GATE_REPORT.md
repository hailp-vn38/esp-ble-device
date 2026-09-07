# ESP BLE Device Settings v1.1 — F0–F9 Release Gate Report

**Date:** 2026-09-07
**Device repo:** `esp-ble-device`
**Target:** ESP32-S3
**Build toolchain:** ESP-IDF v5.5.5
**Gateway contract peer:** `/Users/lamphuchai/Desktop/esp32-ble-gateway`, `dev-device-settings` @ `55fcf556`

## Phase commits

| Phase | Commit | Result |
|---|---|---|
| F0 | `33e4f05` | Storage binding, lifecycle/build graph |
| F1 | `a65d502` | Active/staging isolation, load recovery, callback compatibility |
| F2 | `92a89c0` | Command-worker ownership, tx ID, disconnect/timeout handling |
| F3 | `71e6eb4` | Gateway contract alignment and type/command compatibility aliases |
| F4 | `e010f56` | Specialized stream framing and ATT payload bound |
| F5 | `5725d7f` | ACK/confirm/restart sequencing |
| F6 | `979e5a9` | Settings routing cleanup and capability advertisement |
| F7 | `b317249` | Factory reset and NVS recovery |
| F8 | `2a4c1ea` | Reference/demo product integration |
| F9 | pending | This release-gate report and final verification |

## Host verification

The following suites passed in the final run:

- `run_device_settings_tests.sh`
- `run_gateway_settings_tests.sh`
- `run_settings_stream_tests.sh`
- `run_settings_tx_tests.sh`
- `run_settings_confirm_tests.sh`
- `run_fault_injection_tests.sh`
- `run_reference_config_tests.sh`
- `run_soak_tests.sh`
- `run_device_protocol_tests.sh`
- `run_device_capability_tests.sh`
- `run_gateway_interop_check.sh`

Important final counts include:

- Settings registry: 135 checks
- Stream codec: 153 checks
- Transaction: 127 checks
- Confirm: 84 checks
- Fault injection: 107 checks
- Reference config: 113 checks
- Soak: 5570 checks
- Device protocol: 89 checks
- Device capability: 55 checks
- Gateway codec: 148 checks
- Gateway interop: 0 failures

## Firmware build verification

Both products passed clean builds with ESP-IDF v5.5.5:

```text
idf.py -C devices/reference_device build
idf.py -C devices/demo_device build
```

A final fullclean rebuild also passed for both products after deleting their generated build directories.

Final application sizes were below the 1.875 MiB application partition. The builds emitted pre-existing ESP-IDF Kconfig warnings for renamed/unknown NimBLE symbols; these did not prevent compilation.

## Hardware verification

The connected target was confirmed through `/dev/cu.usbmodem2101`:

```text
Chip: ESP32-S3 (QFN56), revision v0.2
Flash: 4 MB
PSRAM: 2 MB
MAC: ac:27:6e:cc:f2:24
```

Reference firmware was flashed successfully and image hashes were verified by esptool.

BLE scan found the reference device:

```text
GW-REF (B6B483A0-D191-ABBB-1603-E07BD14866A8)
```

The HIL connection test could not proceed beyond pairing. macOS CoreBluetooth returned:

```text
CBErrorDomain Code=14: Peer removed pairing information
```

The same failure reproduced on retry. The current HIL harness does not clear the macOS CoreBluetooth pairing cache, and its bond-reset tests occur after an established connection. Therefore HW-DEV-001 through the Settings BLE transaction tests could not be completed in this environment.

Serial monitor verification was also unavailable because `idf.py monitor` requires a TTY in this runner. No claim is made that the HIL transaction path passed.

## Gateway contract note

The local Gateway implementation uses a different Settings extension family from the original Device guide: Gateway discovery/value frame names, command names, numeric keys, and type enum values differ. F3 migrated Device constants/codec behavior toward the selected Gateway contract while retaining compatibility aliases for older Device vectors. Full two-way BLE transaction verification remains blocked by the stale pairing state above.

## Release status

**Host/build gate:** PASS.

**Hardware flash/chip identification:** PASS.

**BLE HIL Settings gate:** BLOCKED by stale macOS pairing information (`CBErrorDomain Code=14`) and non-TTY serial monitor limitations.

This report intentionally does not mark the release as fully hardware-validated. To close the remaining gate, clear the host/device bond state using the approved lab procedure or a fresh macOS BLE host, then rerun the HIL discovery, Settings read/write/commit/reboot, disconnect fault, and soak matrix.
