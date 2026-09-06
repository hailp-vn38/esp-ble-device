# Device Phase D0 — Protocol Alignment & Extension Gate


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Khóa wire contract trước khi implement Settings. Hiện device `main` và gateway `dev-ws` cùng ghi Protocol v4 nhưng gateway đã có các extension field mà device header chưa đồng bộ; không được tiếp tục mở rộng trên nền drift này.

## Files cần review/sửa

```text
components/gateway_protocol/include/gateway_protocol.h
components/gateway_protocol/gateway_protocol.c     # nếu có codec source tách riêng
tests/... protocol codec tests
```

## Tasks

### D0.1 Đồng bộ protocol baseline

- Đối chiếu protocol version, UUID, CBOR key, enum message type, feature type, property type.
- Bổ sung/đồng bộ các key hiện gateway đã dùng hợp lệ, nhưng không thêm Settings semantics trước compatibility test.
- Tạo bảng numeric-key authoritative trong comment hoặc tài liệu protocol test.

### D0.2 Settings capability advertisement

Device mới phải có cách báo:

```text
settings_supported = true
settings_schema_revision = N
```

Gateway cũ không được bắt buộc hiểu field này.

### D0.3 Specialized Settings codec

Không mở rộng shared `gw_message_t` bằng fixed-size strings/enum arrays cho Settings.

Device nên có API riêng kiểu:

```c
esp_err_t gw_settings_encode_begin(...);
esp_err_t gw_settings_encode_item(...);
esp_err_t gw_settings_encode_value(...);
esp_err_t gw_settings_decode_command(...);
```

Buffer output vẫn bounded bởi `GW_MSG_MAX_LEN`.

### D0.4 Message families cần khóa

Discovery:

```text
describe_settings
settings_begin
setting_item
setting_option_item       # nếu option không fit item
settings_end
read_settings
settings_values_begin
setting_value
settings_values_end
```

Update:

```text
settings_tx_begin
settings_tx_set
settings_tx_commit
settings_tx_abort
settings_commit_confirm
```

Mọi request/ack cần `request_id`. Transaction commands cần `transaction_id`.

### D0.5 Version decision

Giữ v4 extension chỉ khi các test chứng minh decoder cũ bỏ qua unknown/additive key/message an toàn. Nếu old gateway/device reject frame mới chỉ vì unknown additive data, bump protocol v5.

## Tests

### Unit codec

- [ ] Encode/decode minimal Settings command.
- [ ] Unknown additive key không corrupt parser.
- [ ] Missing mandatory key reject.
- [ ] Wrong type reject.
- [ ] Oversized string reject trước encode.
- [ ] Every encoded frame `<= GW_MSG_MAX_LEN`.
- [ ] Fuzz truncated CBOR returns error, không crash/leak.

### Golden vectors

Lưu byte-exact vectors cho ít nhất:

1. `describe_settings`.
2. `settings_begin`.
3. BOOL `setting_item`.
4. INT item có min/max/step/unit.
5. ENUM + option.
6. STRING max length.
7. `settings_tx_begin(expected_revision, transaction_id)`.
8. `settings_tx_set` cho từng type.
9. `settings_tx_commit`.
10. `settings_commit_confirm`.

### Compatibility HIL

- [ ] old gateway ↔ old device: regression pass.
- [ ] new gateway ↔ old device: old device vẫn control/feature bình thường, Settings unsupported.
- [ ] old gateway ↔ new device: old gateway vẫn control/feature bình thường.
- [ ] new gateway ↔ new device: Settings enabled.

## Checklist

- [ ] Device header là authoritative cho version/keys đã thống nhất.
- [ ] Không reuse numeric key có nghĩa khác.
- [ ] Không tăng `gw_message_t` để chứa Settings strings.
- [ ] Capability advertisement backward-compatible.
- [ ] Version v4/v5 được quyết định bằng test, không bằng giả định.
- [ ] Golden vectors committed.

## Exit gate

Không qua D1/D2 transport integration cho tới khi compatibility matrix và golden vectors pass.
