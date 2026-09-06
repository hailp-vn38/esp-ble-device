# Phase 1 — Device Generic Feature + Protocol

## 1. Tổng quan

Phase này hoàn thiện semantic feature framework ở Device.

Hiện tại framework thực tế mới hỗ trợ:

```text
ON_OFF_LIGHT
BOOL
read_bool
publish_bool
```

và `read_feature_state` đang hard-code BOOL.

Target v2:

```text
BOOL + INT
specialized + GENERIC_VALUE
title
unit
decimals
read-only + writable
authoritative typed ACK
typed feature events
```

---

## 2. Mục tiêu

- generic registry không phụ thuộc LED;
- feature scalar tổng quát;
- INT state end-to-end;
- compact feature discovery;
- single source of truth cho write range;
- reject invalid feature↔tool binding trước khi advertise schema.

---

## 3. Cần thêm / sửa gì

Tóm tắt thay đổi:

| Nhóm | Cần làm |
|---|---|
| Protocol | thêm `GENERIC_VALUE`, `VALUE`, decimals key 31 |
| Feature registry | generic BOOL/INT + title/unit/decimals |
| Event | INT feature event |
| Command | typed `read_feature_state`, authoritative INT ACK |
| Discovery | compact `feature_item`, không runtime state/range |
| Validation | feature↔tool binding tại freeze |
| Limits | feature max 12, payload budget |

---

## 4. Protocol additions

### Feature type

```c
GW_FEATURE_GENERIC_VALUE = 2
```

### Property

```c
GW_PROP_VALUE = 8
```

### Decimals key

Keys hiện dùng đến 30.

Thêm:

```c
GW_KEY_FEATURE_DECIMALS = 31
```

Không thêm title key mới.

---

## 5. Metadata reuse

Trong `feature_item`:

```text
capability_label -> feature title
capability_unit  -> feature unit
value_type       -> feature state type
```

Ví dụ:

```text
feature_id       = dryer_temperature
capability_label = Nhiệt độ sấy
capability_unit  = °C
feature_decimals = 1
```

Giới hạn title theo `GW_MSG_CAP_LABEL_LEN` hiện có.

Không silent truncate.

---

## 6. Feature contract v2

Mọi advertised feature v2 phải readable.

Writable được xác định bằng:

```text
feature_tool present
```

Không cần thêm readable/writable wire flags mới trong v2.

Feature descriptor:

```c
typedef enum {
    DEVICE_FEATURE_VALUE_NONE = 0,
    DEVICE_FEATURE_VALUE_BOOL = 1,
    DEVICE_FEATURE_VALUE_INT  = 2,
} device_feature_value_type_t;

typedef int (*device_feature_read_bool_fn)(
    void *context, bool *out_value);

typedef int (*device_feature_read_int_fn)(
    void *context, int32_t *out_value);

typedef union {
    device_feature_read_bool_fn read_bool;
    device_feature_read_int_fn read_int;
} device_feature_reader_t;

typedef struct {
    uint8_t id;
    device_feature_value_type_t value_type;
} device_feature_property_t;

typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    char title[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];

    gw_feature_type_t type;
    uint16_t schema_version;
    uint16_t flags;
    uint8_t decimals;

    device_feature_property_t property;

    char write_tool[GW_MSG_COMMAND_LEN];

    device_feature_reader_t reader;
    void *context;
} device_feature_descriptor_t;
```

Không chứa:

```text
min
max
step
current state
```

---

## 7. Generic registration API

```c
typedef struct {
    const char *feature_id;
    const char *title;
    const char *unit;

    gw_feature_type_t type;
    uint16_t schema_version;
    uint16_t flags;

    uint8_t property_id;
    device_feature_value_type_t value_type;
    uint8_t decimals;

    const char *write_tool;  // NULL = read-only

    device_feature_reader_t reader;
    void *context;
} device_feature_config_t;

int device_feature_register(
    const device_feature_config_t *config);
```

Validation:

- non-empty bounded `feature_id`;
- unique `feature_id`;
- title bounded;
- unit bounded;
- type/property hợp lệ;
- BOOL ⇒ decimals=0;
- INT ⇒ decimals <= 3;
- reader đúng type;
- write_tool bounded nếu có;
- `GENERIC_VALUE` ⇒ `GW_PROP_VALUE`;
- `GENERIC_VALUE` v2 ⇒ INT;
- max feature = 12.

---

## 8. Generic read API

```c
typedef struct {
    device_feature_value_type_t type;
    union {
        bool bool_value;
        int32_t int_value;
    } value;
} device_feature_value_t;

int device_feature_read(
    const char *feature_id,
    uint8_t property_id,
    device_feature_value_t *out);

int device_feature_read_bool(...);
int device_feature_read_int(...);
```

`read_feature_state` dùng generic read, không switch property bằng hard-code.

---

## 9. Publish API

```c
int device_feature_publish_bool(...);
int device_feature_publish_int(...);
```

INT path:

```text
feature module
→ device_feature_publish_int
→ device_event_publish_feature_int
→ gw_build_feature_event_int
→ BLE notify
```

---

## 10. Command authoritative state

Khuyến nghị response dùng tagged state thay vì thêm nhiều bool flags:

```c
typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    uint8_t property_id;
    device_feature_value_t value;
} device_cmd_feature_state_t;

typedef struct {
    bool success;
    int int_value;        // legacy/general result
    bool long_running;

    bool has_feature_state;
    device_cmd_feature_state_t feature_state;
} device_cmd_response_t;
```

Helpers:

```c
int device_command_response_set_feature_bool(...);
int device_command_response_set_feature_int(...);
```

Ưu điểm:

- không thể set BOOL và INT cùng lúc;
- API rõ hơn;
- ít branch/flag hơn;
- dễ mở rộng.

ACK encoder:

```text
value.type BOOL -> feature_value_bool
value.type INT  -> feature_value_int
```

---

## 11. Writable feature ↔ tool binding

Tool giữ:

```text
value_type
min/max/step
unit
flags
```

Feature chỉ giữ `write_tool`.

Khi `device_command_freeze()`:

1. iterate toàn feature writable;
2. tìm public command capability tương ứng;
3. validate:
   - command tồn tại;
   - advertised/public;
   - `tool.value_type == feature.value_type`;
   - unit tương thích;
   - BOOL tool không có numeric range requirement;
   - INT tool có valid range/step;
4. fail freeze nếu invalid.

Demo v2 rule:

```text
một writable feature -> một tool
một public write tool -> tối đa một semantic feature
```

---

## 12. Compact `feature_item`

Gửi:

```text
feature_id
feature_type
feature_schema_version
feature_flags
property_id
value_type
capability_label = title
capability_unit = unit
feature_decimals
feature_tool nếu writable
```

Không gửi:

```text
current value
min/max/step
```

Initial state được đọc sau schema commit.

---

## 13. Sửa ở đâu

### Device repo

| File | Thay đổi |
|---|---|
| `components/gateway_protocol/include/gateway_protocol.h` | enums + property + key 31 + message field |
| `components/gateway_protocol/gateway_protocol.c` | encode/decode decimals + INT event |
| `components/device_feature/include/device_feature.h` | generic API + INT |
| `components/device_feature/feature_registry.c` | generic register/read/publish |
| `components/device_event/include/device_event.h` | publish INT |
| `components/device_event/device_event.c` | INT event |
| `components/device_command/include/device_command.h` | typed authoritative state |
| `components/device_command/device_command.c` | binding validation, typed read, compact feature item |
| `devices/reference_device/main/reference_product.c` | compatibility-only changes nếu API đổi |
| `test/host/*` | feature/protocol/command tests |

---

## 14. Checklist

### Protocol

- [ ] `GENERIC_VALUE=2` giống Gateway.
- [ ] `VALUE=8` giống Gateway.
- [ ] key 31 chỉ dùng decimals.
- [ ] old unknown keys vẫn tolerated.
- [ ] feature item không gửi current state.
- [ ] title/unit reuse field hiện có.
- [ ] worst-case feature item <= 240 bytes.

### Feature core

- [ ] max features = 12.
- [ ] BOOL register/read/publish.
- [ ] INT register/read/publish.
- [ ] read-only feature không có tool.
- [ ] duplicate ID reject.
- [ ] title overflow reject.
- [ ] unit overflow reject.
- [ ] invalid decimals reject.
- [ ] generic value property mismatch reject.

### Command

- [ ] `read_feature_state` validate `has_feature_id`.
- [ ] validate `has_property_id`.
- [ ] typed read dispatch.
- [ ] authoritative BOOL ACK.
- [ ] authoritative INT ACK.
- [ ] binding validate khi freeze.
- [ ] invalid binding ngăn advertise schema.

---

## 15. Test plan

### T1.1 — BOOL compatibility

Register `light_main`.

Expected:

- discovery đúng;
- read BOOL đúng;
- publish BOOL đúng;
- ACK BOOL đúng.

### T1.2 — INT writable

`fan_main`:

```text
PERCENT_SETTING
INT
0..100 via set_fan_speed
```

Expected full path pass.

### T1.3 — Read-only INT

`temperature_main`.

Expected:

- no feature_tool;
- read INT;
- event INT.

### T1.4 — Generic value

```text
dryer_temperature
title=Nhiệt độ sấy
unit=°C
decimals=1
GENERIC_VALUE/VALUE
```

Expected metadata đúng.

### T1.5 — Binding type mismatch

Feature INT → tool BOOL.

Expected freeze fail.

### T1.6 — Unit mismatch

Feature `°C` → tool `rpm`.

Expected freeze fail.

### T1.7 — Raw scale

Tool:

```text
min=300 max=1000 step=5
```

Request `655`.

Hardware clamp `650`.

Expected ACK `650`.

### T1.8 — Read request invalid

Missing feature_id/property.

Expected failure ACK / validation failure, không crash.

### T1.9 — CBOR max strings

Max bounded `device_id/feature_id/title/unit/tool`.

Expected encoded <= target budget.

### T1.10 — reference_device regression

Expected behavior cũ không đổi.

---

## 16. Exit criteria

- [ ] generic registry pass.
- [ ] INT read/event/ACK pass.
- [ ] compact discovery pass.
- [ ] binding validation pass.
- [ ] raw scale invariant pass.
- [ ] reference regression pass.

---

## 17. Implementation status — 2026-09-06

Đã triển khai phần source của Phase 1:

- thêm `GW_FEATURE_GENERIC_VALUE`, `GW_PROP_VALUE` và `GW_KEY_FEATURE_DECIMALS=31`;
- mở rộng feature registry lên 12 feature, hỗ trợ BOOL/INT, title/unit/decimals;
- thêm generic register/read/publish API và giữ compatibility API ON/OFF cũ;
- thêm typed INT feature event và INT protocol builder;
- `read_feature_state` chuyển sang generic typed dispatch;
- authoritative ACK chuyển sang tagged BOOL/INT feature state;
- thêm validation feature ↔ public tool tại `device_command_freeze()`;
- discovery gửi title/unit/decimals/tool compact metadata, không gửi runtime value/range;
- cập nhật `reference_device` dùng generic registration API.
- cập nhật hardware harness sang protocol v4, typed feature metadata và typed BOOL ACK checks.

Đã xác minh:

- host feature: 36 checks, 0 failures;
- host device protocol: 89 checks, 0 failures;
- host gateway protocol: 136 checks, 0 failures;
- host BLE/capability/identity/interop regression: 0 failures;
- ESP-IDF 6.1-rc1 build pass;
- flash ESP32-S3 `/dev/cu.usbmodem2101` pass, image hash verified.

Chưa xác minh đầy đủ T1.1–T1.10 trên BLE runtime. Harness đã scan thấy `GW-REF` nhưng macOS CoreBluetooth từ chối connect với `CBErrorDomain Code=14` do peer còn bonding record cũ; erase NVS device và power-cycle Bluetooth không xóa được paired record. Vì vậy Phase 1 chưa được đánh dấu hoàn tất và chưa tạo Phase 1 commit.
