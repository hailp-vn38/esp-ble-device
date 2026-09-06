# Device Phase D2 — BLE Settings Discovery & Value Streaming


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Expose registered Settings schema + current values qua BLE bằng bounded streaming, không tạo batch lớn trong heap/notify queue.

## Files

```text
components/device_settings/device_settings_protocol.c
components/device_command/device_command.c
components/ble_peripheral/ble_peripheral.c        # chỉ nếu cần TX flow-control helper
components/gateway_protocol/...                  # codec contract từ D0
```

## Commands

```text
describe_settings
read_settings
```

Device chỉ chấp nhận khi BLE security/CCCD/READY policy hiện tại đã pass.

## Schema stream

```text
settings_begin
  schema_revision
  config_revision
  total
setting_item ...
setting_option_item ... (nếu cần)
settings_end
ACK(describe_settings)
```

Mỗi item encode rồi enqueue/send từng frame. Không allocate:

```c
storage[count][GW_MSG_MAX_LEN]
```

cho toàn discovery.

## Values stream

```text
settings_values_begin(config_revision,total)
setting_value ...
settings_values_end
ACK(read_settings)
```

Secret:

```text
setting_value(secret) -> configured only
```

## Backpressure

Nếu notify queue gần đầy:

- yield/wait bounded theo transport helper;
- không enqueue toàn stream atomically;
- disconnect => stop stream cleanly;
- no unbounded retry loop.

Nếu cần helper mới:

```c
esp_err_t ble_peripheral_notify_wait_room(size_t required,
                                          TickType_t timeout);
```

Không thay đổi critical BLE callback để làm encode nặng.

## Snapshot consistency phía Device

Trong một stream:

- capture `schema_revision`/`config_revision` đầu stream;
- nếu config đổi giữa stream (thường không vì transaction serialized), abort/restart hoặc emit same captured coherent snapshot;
- không trộn values từ hai revisions.

## Tests — Codec/stream

- [ ] zero settings.
- [ ] 1 setting từng type.
- [ ] max settings.
- [ ] max enum options.
- [ ] max string metadata.
- [ ] every frame <= 256 bytes.
- [ ] settings_begin.total matches emitted logical settings.
- [ ] schema revision stable.
- [ ] secret emits configured only.

## Tests — BLE HIL

### Basic
- connect/bond/CCCD ready;
- send `describe_settings`;
- verify order begin/items/end/ACK;
- send `read_settings`;
- verify values/revision.

### Disconnect mid stream
- disconnect after Nth item;
- no crash/deadlock;
- reconnect and new discovery succeeds.

### Queue pressure
- artificially slow notification consumer;
- stream completes or bounded-time fails;
- unrelated feature notification path remains functional.

### Old gateway
- connect old gateway;
- no Settings request expected;
- control/features remain unchanged.

## Memory checklist

- [ ] No `count * GW_MSG_MAX_LEN` discovery allocation.
- [ ] Encode buffer fixed/bounded and scoped.
- [ ] No descriptor string duplication.
- [ ] Queue depth is not required to fit whole stream.
- [ ] Stream stops immediately on disconnect/cancel.

## Exit gate

Repeated 100 schema/value discovery cycles pass on HIL with stable heap and no notify queue deadlock.
