# ESP32 BLE Device Settings v2 — Device Implementation Pack


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## 1. Phạm vi

Bộ này **chỉ chứa công việc phía BLE Device**. Không có Web API, Web UI, gateway snapshot implementation hay gateway memory implementation.

Mục tiêu cuối:

```text
product registers generic settings
    -> gateway discovers schema/value over BLE
    -> gateway starts transaction
    -> device stages + validates changes
    -> device commits one atomic config blob + config_revision
    -> device confirms commit
    -> device reboots safely
    -> after reconnect values + revision remain correct
```

## 2. Quyết định kiến trúc đã khóa

1. `Settings` tách khỏi runtime `Feature`.
2. Device là source-of-truth cho values.
3. Descriptor là `static const` trong flash; registry chỉ giữ pointer.
4. V2 support: `BOOL`, `INT`, `STRING`, `ENUM`.
5. Save là transaction atomic: `BEGIN -> SET* -> COMMIT -> COMMIT_CONFIRM -> reboot`.
6. `config_revision` nằm trong cùng persisted config blob.
7. Device kiểm tra `expected_revision` tại `BEGIN`.
8. Transaction có `transaction_id`; duplicate phải idempotent.
9. Secret không bao giờ read back plaintext.
10. V2: mọi successful commit đều reboot.
11. Discovery/settings response phải stream bounded; không `calloc(count * 256)`.
12. BLE disconnect trước commit => abort staging; sau persisted commit => giữ commit và chờ confirm/timeout restart.

## 3. Thứ tự phase

| Phase | File | Exit gate |
|---|---|---|
| D0 | `DEVICE_PHASE_0_PROTOCOL_ALIGNMENT.md` | protocol vectors + compatibility decision pass |
| D1 | `DEVICE_PHASE_1_SETTINGS_CORE_PERSISTENCE.md` | local transaction + NVS atomic pass |
| D2 | `DEVICE_PHASE_2_BLE_DISCOVERY.md` | schema/value stream pass |
| D3 | `DEVICE_PHASE_3_TRANSACTION_COMMANDS.md` | BEGIN/SET/COMMIT/ABORT over BLE pass |
| D4 | `DEVICE_PHASE_4_CONFIRM_REBOOT_RECONCILE.md` | commit confirm + fallback reboot pass |
| D5 | `DEVICE_PHASE_5_REFERENCE_DEVICE_DEMO.md` | demo registers all setting types |
| D6 | `DEVICE_PHASE_6_HIL_FAULT_TESTS.md` | HIL/fault matrix pass |
| D7 | `DEVICE_PHASE_7_SOAK_RELEASE.md` | soak + NVS/power loss release gates pass |

Release checklist: `DEVICE_TEST_MATRIX_AND_RELEASE_CHECKLIST.md`.

## 4. Dependency

```text
D0 -> D1 -> D2 -> D3 -> D4 -> D5 -> D6 -> D7
```

D1 có thể phát triển trước transport. D2/D3 không merge trước khi D0 protocol contract khóa.

## 5. Device state machines

Transaction:

```text
IDLE
  -> ACTIVE
  -> COMMITTED_WAIT_CONFIRM
  -> RESTART_PENDING
```

Discovery không được giữ global heap lớn; response được generate từng frame.

## 6. Global limits đề xuất

```c
#define DEVICE_SETTING_MAX_COUNT        12
#define DEVICE_SETTING_ID_MAX_LEN       32
#define DEVICE_SETTING_TITLE_MAX_LEN    48
#define DEVICE_SETTING_GROUP_MAX_LEN    32
#define DEVICE_SETTING_UNIT_MAX_LEN     16
#define DEVICE_SETTING_STRING_MAX_LEN   64
#define DEVICE_SETTING_ENUM_MAX_OPTIONS 8
#define DEVICE_SETTING_TX_TIMEOUT_MS    10000
```

Giá trị exact phải được static assert/test theo CBOR frame <= `GW_MSG_MAX_LEN`.

## 7. Definition of Done — Device

- [ ] Protocol header không drift với gateway contract.
- [ ] Descriptor registry generic, immutable sau freeze.
- [ ] NVS config blob có format version + config_revision.
- [ ] Multi-setting commit atomic.
- [ ] Cross-field validation chạy trước NVS commit.
- [ ] Stale expected revision bị reject tại device.
- [ ] Duplicate transaction/commit idempotent.
- [ ] Secret readback không trả plaintext.
- [ ] Discovery streaming không phụ thuộc notify queue chứa toàn batch.
- [ ] Commit-confirm flow reboot an toàn.
- [ ] 100 save/reboot cycles pass.
- [ ] Disconnect/power-loss fault tests pass.
