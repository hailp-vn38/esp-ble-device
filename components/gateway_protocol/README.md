# gateway_protocol

ESP-GATT Protocol v3 wire contract for the Device side. Shared dependency:
constants, message model and CBOR codec mirroring the Gateway
(`esp-ble-gateway/components/cbor_codec`, `ble_central` UUID constants).

> Khi shared repo `esp-gatt-protocol` được tách ra, component này sẽ được
> thay bằng submodule — API giữ nguyên.

## Purpose

- Protocol/UUID constants: service `0xABF0`, COMMAND `0xABF1`,
  STATUS `0xABF2`, CCCD `0x2902`, `GW_PROTOCOL_VERSION = 3`.
- `gw_message_t`: wire message model (giống hệt `gw_message_t` Gateway).
- CBOR encode/decode với numeric keys (0..21), gồm capability discovery.
- ACK/event builders + TX validation theo integration contract.

## Public API

Xem `include/gateway_protocol.h`.

- `gw_message_init` — zero-init, TX mặc định emit v3 explicit.
- `gw_ble_max_tx_payload(mtu)` — `min(256, mtu - 3)`; không hard-code 253.
- `gw_message_encode(msg, buf, cap)` → độ dài hoặc `-gw_result_t`.
  Luôn emit: `protocol_version, type, command, int_value, bool_value`;
  optional: `device_id, request_id, name, device_type, ble_addr(+type)` và
  capability metadata (`snapshot_id`, sequence/schema/revision).
- `gw_message_decode(buf, len, msg)` — strict như decoder Gateway:
  bắt buộc `type/command/int_value/bool_value`; version 1..3 (thiếu → v3);
  `request_id` nếu có phải ≥ 1; không chấp nhận trailing bytes.
- `gw_build_ack` / `gw_build_event` — builder encode đúng contract echo.
- `gw_message_valid_ack` / `gw_message_valid_event` — kiểm tra bắt buộc
  trước khi notify (ACK: device_id + command non-empty + request_id ≥ 1).

## State ownership

Stateless. Không dùng mutex, không cấp phát động.

## Concurrency

An toàn gọi từ nhiều task (chỉ function thuần trên buffer người dùng cung cấp).

## Errors

`gw_result_t` trong header; `gw_result_str()` để log.

## Tests

Host unit tests: `test/host/run_gateway_protocol_tests.sh`
(golden vectors từ integration contract #184/#185/#70/#71/#72,
edge cases version/request_id/string limits/trailing bytes).

## Known limitations

- Decoder chỉ hỗ trợ definite-length items (QCBOR Gateway encoder cũng
  chỉ emit definite-length).
- Chưa có JSON conversion (Device không cần).
