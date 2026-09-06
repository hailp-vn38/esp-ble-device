# Device Phase D3 — BLE Transaction Commands


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Wire D1 transaction core vào BLE command pipeline, giữ command handling bounded và idempotent.

## Commands

```text
settings_tx_begin
settings_tx_set
settings_tx_commit
settings_tx_abort
```

Fields tối thiểu:

```text
request_id
transaction_id
expected_revision (BEGIN)
setting_id + typed value/action (SET)
```

## Command handler rules

### BEGIN

```text
decode
-> validate transaction id
-> local tx_begin(expected_revision)
-> ACK {status,current_revision}
```

Conflict phải distinguish được với generic reject.

### SET

- transaction id must match active;
- setting id bounded;
- decode value using Settings-specific view;
- no heap copy nếu scalar;
- string phải copy vào bounded staging/product buffer trước khi raw RX frame hết lifetime;
- ACK only after staging accepted.

### COMMIT

```text
validate_all
-> NVS commit blob+new_revision
-> state COMMITTED_WAIT_CONFIRM
-> ACK {committed=true,new_revision}
```

**Không restart ngay trong handler.**

### ABORT

- only active transaction;
- duplicate safe;
- no NVS mutation.

## Serialization

Chỉ một Settings transaction active/device. Runtime commands/features vẫn có thể hoạt động nếu product policy cho phép, nhưng command nào thay cùng config backend cần explicit lock/order.

## Error mapping đề xuất

```text
OK
INVALID_ARGUMENT
UNSUPPORTED
TYPE_MISMATCH
OUT_OF_RANGE
READONLY
REVISION_CONFLICT
TX_BUSY
TX_NOT_ACTIVE
TX_ID_MISMATCH
VALIDATION_FAILED
PERSIST_FAILED
INTERNAL_ERROR
```

## Tests — Unit

- [ ] decode all 4 SET types.
- [ ] malformed SET no staging mutation.
- [ ] wrong tx id reject.
- [ ] SET before BEGIN reject.
- [ ] second BEGIN different tx => busy.
- [ ] stale expected revision conflict.
- [ ] commit validation error no NVS mutation.
- [ ] duplicate request id deterministic.
- [ ] duplicate COMMIT returns same committed revision.

## Tests — Integration/HIL

1. Multi-field success.
2. Invalid second field -> transaction remains active or error policy deterministic; commit invalid set cannot partially persist.
3. Disconnect before commit -> staging eventually aborts.
4. Reconnect after aborted tx -> active values unchanged.
5. NVS failure injection -> COMMIT PERSIST_FAILED, no revision increment.
6. Two gateways/clients cannot overlap transaction (as BLE connection policy permits).

## Checklist

- [ ] No restart in COMMIT handler.
- [ ] No NVS write in SET.
- [ ] Device validates expected revision itself.
- [ ] Transaction ID checked on every tx command.
- [ ] String ownership valid beyond RX frame lifetime.
- [ ] Error status stable enough for gateway mapping.

## Exit gate

Gateway test harness/raw BLE client can complete atomic multi-field transaction, and all fault cases preserve active config consistency.
