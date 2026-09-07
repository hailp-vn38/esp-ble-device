## Mục tiêu và nguyên tắc thực hiện

Triển khai Device Settings v1.1 từ F0 đến F9 trong repo `/Users/lamphuchai/Desktop/esp-ble-device`, theo đúng thứ tự bắt buộc. Mỗi phase là một đơn vị độc lập:

1. chỉ sửa phạm vi của phase đó;
2. chạy toàn bộ test bắt buộc của phase;
3. chạy `git diff --check` và kiểm tra thay đổi;
4. chỉ khi test đạt mới tạo một commit riêng cho phase;
5. nếu test/build/HIL thất bại thì dừng ở phase đó, sửa nguyên nhân, chạy lại rồi mới commit.

Giữ nguyên file hướng dẫn đang untracked `docs/ESP_BLE_DEVICE_SETTINGS_FIX_IMPLEMENTATION_GUIDE_v1.1_IMPLEMENTATION_READY.md`; không ghi đè hoặc xóa file này.

## F0 — Build graph và storage binding

### Thay đổi
- Thêm `device_settings_storage_config_t` và `device_settings_configure()` vào `device_settings.h`.
- Chuyển lifecycle sang `device_settings_init()` chỉ reset runtime state; `device_settings_configure()` bind một lần các buffer active/staging, config size, format version, defaults/validation callback.
- Kiểm tra NULL, cùng pointer, alignment, kích thước tối thiểu của persisted header và double-configure.
- Loại bỏ việc product-facing sử dụng các loose setter; giữ helper nội bộ nếu cần cho tương thích test trong phase chuyển tiếp.
- Sửa boot order trong `device_app` để gọi init rồi configure sau khi NVS/profile metadata đã sẵn sàng.
- Bổ sung dependency `device_settings` cho `device_app` và `device_command`; bổ sung dependency/source cần thiết cho product CMake.
- Đảm bảo `reference_config.c` được đưa vào SRCS của reference product và thêm `demo_config.c` placeholder/source khi cần cho integration.

### Test và cổng F0
- Bổ sung/cập nhật host tests: configure trước init, init→configure, double configure, NULL active/staging, cùng pointer, blob quá nhỏ.
- Chạy:
  - `test/host/run_device_settings_tests.sh`
  - `test/host/run_gateway_settings_tests.sh`
- Nếu công cụ IDF khả dụng, build sạch reference và demo để bắt lỗi dependency/link sớm.
- Commit riêng: `feat(settings): add storage binding and fix build graph`.

## F1 — Active/staging, typed descriptor ABI và defaults

### Thay đổi
- Thay `.ctx` bằng `read_ctx` và `stage_ctx`.
- Thêm typed callback unions cho BOOL/INT/STRING/ENUM; không còn ABI callback `void *out` mơ hồ trong public descriptor.
- Enforce invariant: read callback chỉ nhìn active, stage callback chỉ ghi staging; READONLY không có stage callback.
- Để core sở hữu persisted header; product defaults callback chỉ khởi tạo payload.
- Implement defaults callback, validation callback trên payload theo API đã khóa.
- Implement `device_settings_load_result_t` và boot load algorithm: valid blob, absent key, sai size, sai format, reserved/header lỗi, payload validation lỗi; mọi nhánh recovery đều copy active→staging.
- Không tự động ghi NVS chỉ vì thiếu key; revision mặc định là 0.
- Bổ sung kiểm tra descriptor đầy đủ tại freeze: ID/title/group/unit length, enum options, numeric range/step, callback presence/flags.
- Migrate reference descriptors và toàn bộ host test fixtures sang active/staging + typed callbacks.

### Test và cổng F1
- Isolation: SET staging không làm thay đổi read active trước commit.
- Commit: active đổi sau đúng một commit thành công.
- Defaults/recovery cho no-key, wrong-size, wrong-format, corrupt header/payload và invalid defaults.
- Chạy:
  - `run_device_settings_tests.sh`
  - `run_reference_config_tests.sh`
  - `run_settings_stream_tests.sh`
- Commit riêng: `refactor(settings): isolate active staging and typed callbacks`.

## F2 — Single-owner transaction state machine

### Thay đổi
- Đưa các result về `device_settings_result_t`; mapping sang wire status sẽ nằm ở command layer.
- Thêm transaction ID vào API SET; mọi BEGIN/SET/COMMIT/ABORT/CONFIRM đều kiểm tra đúng transaction ID.
- Implement state runtime bounded gồm state, tx ID, expected revision, last committed tx/revision và generation.
- BEGIN: reject tx ID zero, stale revision; retry cùng tx/revision trả OK; transaction khác trả BUSY.
- SET: validation đúng thứ tự, assignment-only, không NVS, timeout không refresh.
- COMMIT: full validation, `UINT32_MAX` → REVISION_EXHAUSTED, ghi revision cùng blob, đúng một `nvs_commit`, active promotion sau persist thành công; duplicate COMMIT không ghi lần hai.
- ABORT idempotent ở IDLE và discard staging ở ACTIVE.
- Xóa timer/restart ownership khỏi `device_settings`; bỏ reverse dependency tới `device_app`.
- Mở rộng `device_command` queue item bằng bounded internal event type cho RX, disconnect và timeout; BLE callback/timer callback chỉ post event.
- Đưa timeout absolute từ BEGIN và generation chống stale event vào command worker.
- Disconnect ACTIVE abort staging; disconnect WAIT_CONFIRM chỉ chuyển orchestration sang restart pending.

### Test và cổng F2
- Host tests cho wrong tx ID, duplicate BEGIN/COMMIT, overflow, timeout generation và mọi thứ tự queue/disconnect/SET/COMMIT.
- Fault tests phải chứng minh disconnect trước commit giữ config/revision cũ.
- Chạy:
  - `run_settings_tx_tests.sh`
  - `run_fault_injection_tests.sh`
  - toàn bộ registry/persistence tests của F0/F1
- Commit riêng: `refactor(settings): serialize transactions through command worker`.

## F3 — Gateway contract gate

### Thay đổi
- Đối chiếu Device với checkout Gateway local `/Users/lamphuchai/Desktop/esp32-ble-gateway`, branch/ref Settings hiện hành.
- So symbol/byte-level: protocol version, UUID, message type, command strings, numeric CBOR keys, type/flag/status enums, revision widths, tx ID width, string limits, request ID và secret semantics.
- Xuất hoặc lấy golden vectors cho discovery, values, BEGIN/SET/COMMIT/CONFIRM.
- Xác định một contract duy nhất. Không tự tạo key mới, không silently truncate schema revision/config revision.
- Nếu Gateway implementation thực tế khác guide, cập nhật Device theo contract Gateway authoritative và ghi rõ deviation trong test/vector; nếu contract không thể quyết định an toàn, dừng tại F3 thay vì merge wire change.

### Test và cổng F3
- Device vectors decode được bằng Gateway và Gateway vectors decode được bằng Device.
- Chạy Gateway-side codec/unit test nếu checkout và toolchain khả dụng, cùng `run_gateway_interop_check.sh` nếu có dependency.
- Chỉ commit khi hai chiều pass: `protocol(settings): align device with gateway contract`.

## F4 — Specialized codec và RAM cleanup

### Thay đổi
- Giữ common `gw_message_t` chỉ còn Settings capability-level fields (`settings_supported`, schema revision nếu contract yêu cầu); chuyển metadata/transaction fields sang `gw_settings_command_t` và specialized codec.
- Đổi decoder Settings sang raw `buf,len`; common decoder chỉ lấy envelope/routing fields.
- Tách command constants khỏi message-type constants.
- Stream encoder dùng type thật: begin/item/option/end/values begin/value/values end.
- Mọi stream frame có protocol/type/device_id/command/request_id; begin/end mang schema/config revision; item mang option_count; option mang parent setting ID/sequence/index.
- Thêm specialized Settings ACK encoder: bool success, `int_value=status`, optional tx ID/config revision; không overload status bằng revision.
- Không heap allocation theo setting; kiểm tra duplicate/missing/unknown/truncated CBOR và bounded string.
- Thêm MTU/ATT payload API, target `GW_SETTINGS_MIN_MTU=247`, payload `244` theo contract; reject frame vượt payload.
- Thêm preflight encode toàn bộ descriptor/options sau freeze, trước advertising.
- Ghi nhận `sizeof(gw_message_t)`, `sizeof(gw_settings_command_t)`, `sizeof(cmd_rx_msg_t)` trước/sau.

### Test và cổng F4
- Cập nhật stream/gateway tests cho exact type, routing ID, request ID, revisions, option count, malformed CBOR, unknown keys, ATT limit.
- Chạy:
  - `run_gateway_settings_tests.sh`
  - `run_settings_stream_tests.sh`
  - `run_device_protocol_tests.sh`
  - `run_gateway_interop_check.sh` nếu build được
- Commit riêng: `refactor(protocol): specialize settings codec and bound frames`.

## F5 — ACK/CONFIRM/restart sequencing

### Thay đổi
- `device_settings_tx_commit()` chỉ persist/promote/state; không arm confirm timer.
- `device_command` enqueue COMMIT ACK trước, chỉ arm confirm timer nếu enqueue thành công; nếu enqueue fail thì fallback restart.
- CONFIRM core chỉ validate state/tx/revision và trả result; command worker encode/enqueue CONFIRM ACK trước rồi mới set RESTART_PENDING/schedule restart, kể cả ACK enqueue lỗi.
- Inject `device_cmd_restart_fn` từ `device_app`; không gọi trực tiếp `device_app` trong Settings core.
- Timer callback chỉ post timeout event; không block timer service task. Restart cleanup/blocking work chạy ở app-safe context.
- Implement fallback khi mất COMMIT ACK, mất CONFIRM hoặc mất CONFIRM ACK; không rollback persisted config.

### Test và cổng F5
- Fault tests: drop COMMIT ACK, drop CONFIRM, drop CONFIRM ACK, power-cycle sau NVS commit; kết quả chỉ được là old+old hoặc new+new.
- Chạy:
  - `run_settings_confirm_tests.sh`
  - `run_fault_injection_tests.sh`
  - `run_settings_tx_tests.sh`
- Commit riêng: `fix(settings): sequence commit confirmation before restart`.

## F6 — Command routing và capability advertisement

### Thay đổi
- Xóa/retire built-in Settings stub handler không còn reachable; chỉ giữ một Settings dispatcher.
- Dùng `GW_COMMAND_SETTINGS_*` cho transaction commands, không dùng message-type constant làm command.
- Hoàn thiện routing cho CONFIRM và raw specialized decode.
- Capability discovery chỉ quảng bá Settings khi configure, freeze, storage/load và handlers đều thành công; schema revision lấy từ profile đúng width.
- Product không hỗ trợ Settings không quảng bá support và không route Settings như supported.
- Giữ normal feature/control path không đổi.

### Test và cổng F6
- Test profile không Settings, profile Settings, exact schema revision, old Gateway bỏ qua additive fields, normal feature discovery.
- Chạy:
  - `run_device_capability_tests.sh`
  - `run_settings_stream_tests.sh`
  - `run_settings_tx_tests.sh`
  - `run_device_feature_tests.sh`
- Commit riêng: `fix(command): finalize settings routing and capability advertisement`.

## F7 — Factory reset và recovery

### Thay đổi
- Thêm `device_settings_factory_reset()`; erase đúng namespace/key Settings.
- Reject hoặc áp dụng policy rõ ràng khi restart committed đang pending.
- Abort ACTIVE, xóa NVS key, rebuild product defaults/header revision 0, copy active→staging.
- Thêm profile `supports_factory_reset` và optional product reset callback.
- `device_app_factory_reset()` thực hiện clear bonds, reset Settings, product reset callback và schedule reboot theo policy.
- Không xóa NVS ngoài phạm vi Settings một cách vô tình.

### Test và cổng F7
- Save revision 5 → factory reset → reboot/load → defaults revision 0; không còn stale tx state; bonds reset theo policy.
- Bổ sung test NVS erase failure và pending transaction policy.
- Chạy:
  - `run_device_settings_tests.sh`
  - `run_fault_injection_tests.sh`
  - `run_soak_tests.sh`
- Commit riêng: `feat(settings): add deterministic factory reset and recovery`.

## F8 — reference_device và demo_device integration

### Reference
- Sửa CMake source/dependency.
- Migrate toàn bộ descriptors sang read active/stage staging.
- Tách/defaults/validation theo API mới.
- Tắt `admin_token` SECRET trong normal profile; giữ field/padding hoặc bump format version nếu layout persisted thay đổi.
- Giữ nguyên LED/features behavior.

### Demo
- Thêm `demo_config.h/.c` với header + payload và 8 settings: sensor enabled, sample interval, startup dryer temp, startup fan mode, startup fan percent, device label, diagnostic logging, readonly serial number.
- Đăng ký defaults/validation/descriptors static; không SECRET.
- Sau load, apply startup dryer temperature và fan mode/percent deterministic.
- Refactor `demo_sensor_start()` nhận interval/enabled callbacks; bỏ hard-coded 3000 ms; sensor task đọc active committed config.
- Tách persistent startup setting khỏi runtime feature state.
- Cập nhật profile/CMake direct dependency.

### Test và cổng F8
- `run_reference_config_tests.sh` và test mới cho demo config/descriptor/defaults/validation.
- Fullclean build:
  - `idf.py -C devices/reference_device fullclean build`
  - `idf.py -C devices/demo_device fullclean build`
- Nếu build pass, flash firmware phù hợp lên phần cứng ESP32-S3 và kiểm tra boot/advertising.
- Commit riêng: `feat(products): integrate generic settings into reference and demo devices`.

## F9 — Release gate, HIL và soak

### Host/release
- Cập nhật tất cả test hiện có, không xóa assertion để che thay đổi behavior.
- Chạy toàn bộ:
  - `run_device_settings_tests.sh`
  - `run_gateway_settings_tests.sh`
  - `run_settings_stream_tests.sh`
  - `run_settings_tx_tests.sh`
  - `run_settings_confirm_tests.sh`
  - `run_fault_injection_tests.sh`
  - `run_reference_config_tests.sh`
  - `run_soak_tests.sh`
  - các protocol/capability/feature/BLE tests liên quan.
- `git diff --check`; kiểm tra kích thước struct, heap/stack metrics và không có secret plaintext log.

### HIL trên phần cứng đã kết nối
- Dùng cổng `/dev/cu.usbmodem2101` cho flash/log nếu đúng target hiện tại; xác nhận port trước khi thao tác.
- Build/flash reference và demo khi được chọn; monitor boot logs.
- Mở rộng `test/hardware/test_hw_dev.py` hoặc harness Settings riêng để kiểm tra:
  - capability Settings supported/schema revision;
  - describe stream order/type/device_id/request_id/options/revisions;
  - read active values;
  - BOOL/INT/ENUM/STRING SET và multi-field commit;
  - confirm/restart/reconnect/read-back revision;
  - stale revision, range/step/enum/string/readonly/cross-field errors;
  - disconnect trước commit, sau commit, drop ACK/CONFIRM, stale timeout, NVS failure nếu injectable;
  - MTU dưới target và frame rejection.
- Soak tối thiểu: 100 discovery, 100 reads, 100 save/reboot/reconnect, 100 disconnect-before-commit; ghi heap trước/sau, min heap/largest block, stack high-water, NVS revision, reboot và reconnect success.
- Kiểm tra compatibility: old Gateway + new Device control/features, new Gateway + old Device Settings unsupported, new Gateway + new Device full path.

### Cổng cuối và commit
- Chỉ đạt F9 khi host tests, cả hai IDF builds, golden vectors, HIL và soak đạt; mọi failure phải được sửa trong commit F9 hoặc commit sửa phase tương ứng rồi rerun từ cổng bị ảnh hưởng.
- Commit riêng: `test(settings): close host hil and soak release gate`.
- Báo cáo cuối sẽ liệt kê từng commit F0–F9, test command/output, firmware flashed, HIL/soak metrics và mọi giới hạn còn lại.

## Điều kiện dừng bắt buộc

- Không chuyển qua phase nếu phase trước chưa có test pass và commit.
- Không merge F3/F4 nếu Gateway contract/golden vectors hai chiều không khớp.
- Không tự ý sửa Gateway repo trong phạm vi này; nếu contract Gateway local không thể được xác định hoặc yêu cầu breaking change ngoài Device scope, dừng tại F3 và báo cáo blocker cụ thể.
- Không bật SECRET production khi chưa có contract KEEP/SET/CLEAR và configured-only readback đã được kiểm chứng.