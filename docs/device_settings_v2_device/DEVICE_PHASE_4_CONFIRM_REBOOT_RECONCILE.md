# Device Phase D4 — Commit Confirmation, Safe Restart & Reconciliation Support


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Không reboot quá sớm sau persisted commit. Cho gateway cơ hội xác nhận đã nhận commit result, nhưng vẫn đảm bảo device cuối cùng reboot nếu confirm bị mất.

## Success flow

```text
GW -> COMMIT(tx)
DEV -> validate + NVS commit(new_revision)
DEV -> ACK COMMIT(new_revision)
DEV state = COMMITTED_WAIT_CONFIRM
GW -> COMMIT_CONFIRM(tx,new_revision)
DEV -> ACK/accept confirm
DEV -> schedule safe restart
```

## Confirm timeout fallback

Nếu COMMIT_CONFIRM không tới:

```text
COMMITTED_WAIT_CONFIRM
  -- timeout --> RESTART_PENDING
```

Timeout phải đủ cho normal BLE delivery nhưng bounded. Exact value cấu hình/test được.

## Disconnect after commit

Nếu disconnect khi state `COMMITTED_WAIT_CONFIRM`:

- không rollback persisted config;
- schedule restart immediately hoặc short grace;
- boot lại với revision mới;
- gateway sẽ reconcile bằng `read_settings`.

## Safe restart helper

Không gọi `esp_restart()` trực tiếp từ BLE GATT callback/notify task critical path.

API kiểu:

```c
esp_err_t device_app_schedule_restart(uint32_t delay_ms);
```

Worker/timer riêng:

```text
mark restart pending
-> allow current handler unwind
-> optional transport grace
-> stop product if required
-> esp_restart()
```

## Idempotency after commit

Trong RAM trước restart giữ tối thiểu:

```text
last_committed_transaction_id
last_committed_revision
state
```

Nếu duplicate COMMIT/CONFIRM tới trước reboot, trả deterministic result.

Sau reboot, gateway không phụ thuộc tx-id persistence; `config_revision` + values là reconciliation source-of-truth.

## Tests

### Unit
- [ ] COMMIT enters wait-confirm, not restart immediately.
- [ ] correct confirm schedules restart.
- [ ] wrong tx id confirm ignored/reject.
- [ ] wrong revision confirm reject.
- [ ] timeout schedules restart.
- [ ] duplicate confirm safe.

### HIL fault
- [ ] normal commit-confirm-reboot.
- [ ] drop COMMIT ACK: persisted config still boots correctly.
- [ ] drop COMMIT_CONFIRM: timeout reboot.
- [ ] disconnect immediately after NVS commit: reboot + revision correct.
- [ ] disconnect before COMMIT received: no change.
- [ ] power cycle after commit before confirm: config/revision remain new.

## Checklist

- [ ] Reboot never precedes NVS commit result.
- [ ] Reboot not called from critical callback stack.
- [ ] Confirm timeout exists.
- [ ] Disconnect-after-commit policy deterministic.
- [ ] New revision visible after reboot via `read_settings`.

## Exit gate

All ACK/confirm/disconnect fault tests result in one of only two valid persisted outcomes: old config+old revision or new config+new revision; never mixed.
