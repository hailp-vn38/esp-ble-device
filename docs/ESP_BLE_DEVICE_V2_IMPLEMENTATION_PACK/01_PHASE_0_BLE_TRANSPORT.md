# Phase 0 — BLE Transport Foundation

## 1. Tổng quan

Phase này xử lý blocker trước khi tăng số feature.

Hiện `ble_peripheral` có:

```c
#define BLE_NOTIFY_QUEUE_DEPTH 16
```

`ble_peripheral_notify_batch()` reject nếu `count > 16`.

Demo v2 cần:

```text
7 tools
10 features
begin + end + ACK
= 20 messages
```

Max schema contract:

```text
12 tools
12 features
begin + end + ACK
= 27 messages
```

Vì vậy capability discovery phải chuyển từ full-batch sang ordered streaming sequence.

---

## 2. Mục tiêu


- capability transaction không phụ thuộc queue depth;
- giữ ordering tuyệt đối;
- không interleave event khác giữa begin/end/ACK;
- không allocate `N * GW_MSG_MAX_LEN`;
- validate payload theo negotiated MTU;
- fail sạch nếu disconnect/timeout;
- giữ API notify bình thường cho ACK/event nhỏ.

---

## 3. Cần thêm / sửa gì

### Thêm ordered notify sequence API

Đề xuất:

```c
int ble_peripheral_notify_sequence_begin(void);

int ble_peripheral_notify_sequence_send(
    const uint8_t *data,
    size_t len,
    TickType_t timeout);

void ble_peripheral_notify_sequence_end(void);
```

Behavior:

1. `begin()` acquire `notify_submit_mutex`.
2. `send()`:
   - kiểm tra READY;
   - kiểm tra CCCD;
   - `len <= gw_ble_max_tx_payload(mtu)`;
   - chờ queue có ít nhất 1 slot;
   - copy một notify vào queue.
3. `end()` release mutex.
4. mọi error path phải release mutex.

Không gọi `ble_peripheral_notify()` bên trong sequence vì API đó có thể acquire cùng mutex.

---

### Refactor `send_capabilities()`

Bỏ:

```text
calloc(items)
calloc(batch_count * 256)
encode toàn bộ snapshot trước
notify_batch(all)
```

Thay bằng:

```text
sequence_begin
  encode begin -> send
  encode tool 0 -> send
  ...
  encode feature 0 -> send
  ...
  encode end -> send
  encode ACK -> send
sequence_end
```

Chỉ cần:

```c
uint8_t buffer[GW_MSG_MAX_LEN];
gw_message_t message;
```

Nếu stack pressure cao, đặt scratch buffer trong command context hoặc static worker-owned buffer.

---

### Transaction failure

Nếu fail giữa sequence:

```text
abort sequence
không gửi capabilities_end
release mutex
```

Nếu link vẫn READY và có thể notify:

```text
gửi failure ACK sau khi release sequence
```

Gateway staging snapshot phải bị timeout/reject, không commit partial schema.

---

### MTU policy

Preferred MTU hiện là 256.

Runtime luôn dùng:

```c
gw_ble_max_tx_payload(negotiated_mtu)
```

Với MTU 256:

```text
ATT payload = 253 bytes
```

Policy v2:

- không fragmentation trong Phase 0;
- nếu một capability item lớn hơn payload → discovery fail rõ ràng;
- Phase 1 phải giữ worst-case item <= 240 byte;
- low-MTU connection không được commit partial schema.

---

### Diagnostics

Thêm counters nếu hữu ích:

```c
uint32_t notify_sequence_started;
uint32_t notify_sequence_completed;
uint32_t notify_sequence_aborted;
uint32_t notify_sequence_timeout;
uint32_t notify_oversize;
```

Không bắt buộc expose ra Web UI ở Phase 0.

---

## 4. Sửa ở đâu

### Device repo

| File | Thay đổi |
|---|---|
| `components/ble_peripheral/include/ble_peripheral.h` | khai báo sequence API |
| `components/ble_peripheral/ble_peripheral.c` | sequence mutex/queue handling |
| `components/device_command/device_command.c` | stream `send_capabilities()` |
| `components/device_command/include/device_command.h` | chỉ sửa nếu cần error code |
| `test/host/*` hoặc component tests | capability sequence tests |

Không cần sửa GATT UUID.

---

## 5. Checklist

### Transport

- [ ] `notify_batch()` behavior cũ vẫn regression pass.
- [ ] sequence API không deadlock.
- [ ] mọi return path release mutex.
- [ ] sequence không interleave event.
- [ ] queue full được chờ bounded timeout.
- [ ] disconnect giữa sequence abort sạch.
- [ ] READY/CCCD check trước mỗi send.
- [ ] payload check dùng negotiated MTU.
- [ ] không tăng `BLE_NOTIFY_QUEUE_DEPTH`.

### Capability sender

- [ ] bỏ full `calloc` batch.
- [ ] dùng một encode buffer.
- [ ] sequence order giữ nguyên.
- [ ] begin có đúng `tool_total` / `feature_total`.
- [ ] end chỉ gửi khi toàn item đã enqueue.
- [ ] ACK cuối sequence.
- [ ] failure path không commit partial snapshot.

### Memory

- [ ] không còn allocation `batch_count * 256`.
- [ ] không tăng queue RAM.
- [ ] stack usage được kiểm tra.

---

## 6. Test plan

### T0.1 — Current reference snapshot

Reference device nhỏ:

```text
2 tools + 1 feature + 3 = 6 messages
```

Expected: pass.

### T0.2 — Demo snapshot

```text
7 + 10 + 3 = 20
```

Expected: pass dù queue depth=16.

### T0.3 — Max contract

```text
12 + 12 + 3 = 27
```

Expected: pass.

### T0.4 — Queue pressure

Làm worker drain chậm.

Expected:

- sender wait;
- không mất ordering;
- không partial enqueue transaction do timeout xử lý sai.

### T0.5 — Disconnect mid-sequence

Disconnect sau item 5.

Expected:

- sequence abort;
- mutex release;
- không END;
- reconnect sau đó discovery mới chạy bình thường.

### T0.6 — Oversize payload

Force encoded item > `MTU - 3`.

Expected:

- reject trước queue;
- counter increment;
- schema không commit.

### T0.7 — Concurrent feature event

Trigger feature event khi capability sequence đang gửi.

Expected:

```text
BEGIN
items...
END
ACK
feature_event
```

Không được:

```text
BEGIN
item
feature_event
item
...
```

### T0.8 — Heap regression

So sánh peak free heap trước/sau `describe_capabilities`.

Expected:

- không còn spike từ full snapshot allocation;
- free heap sau transaction trở về baseline.

---

## 7. Exit criteria

- [ ] 20-message demo snapshot pass.
- [ ] 27-message max snapshot pass.
- [ ] no interleave.
- [ ] no deadlock.
- [ ] no batch heap allocation.
- [ ] low-MTU fail-safe.
- [ ] reference device regression pass.

Phase 1 chỉ bắt đầu sau khi Phase 0 pass.

---

## 8. Implementation status — 2026-09-06

Đã triển khai phần source/build của Phase 0:

- thêm ordered sequence API với mutex dùng chung cho mọi producer;
- `send()` chờ bounded từng queue slot, kiểm tra READY/CCCD/MTU trước mỗi frame;
- thêm abort path và diagnostic counters cho sequence, timeout và oversize;
- giữ `BLE_NOTIFY_QUEUE_DEPTH = 16`;
- refactor `send_capabilities()` sang một `GW_MSG_MAX_LEN` encode buffer;
- không còn `calloc(batch_count * 256)` hoặc `notify_batch(all)` trong capability sender;
- cập nhật host BLE contract test.

Đã xác minh:

- host BLE: 22 checks, 0 failures;
- host protocol: 89 checks, 0 failures;
- ESP-IDF 6.1-rc1 firmware build pass;
- flash ESP32-S3 `/dev/cu.usbmodem2101` pass, image hash verified.

Chưa xác minh runtime BLE capability sequence T0.1–T0.8: hardware harness hiện còn hard-code protocol v3/schema reference cũ, còn CoreBluetooth smoke probe không khởi tạo được scanner trong môi trường Python hiện tại. Vì vậy các exit criteria runtime chưa được đánh dấu hoàn thành.
