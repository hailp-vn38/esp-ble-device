# Phase 2 — Gateway Schema + Template + Runtime State

## 1. Tổng quan

Gateway hiện đã có:

- `device_schema`;
- `device_template`;
- `device_state`;
- schema persistence NVS;
- WebSocket gateway events.

Nhưng cần cập nhật để hiểu feature v2 và tách schema khỏi runtime state.

---

## 2. Mục tiêu

- decode generic feature metadata;
- compact schema;
- sửa FAN/DIMMER semantics;
- thêm GENERIC_VALUE;
- migrate persisted schema;
- active-read initial state cho BOOL + INT;
- validate writable binding ở Gateway;
- giữ backward compatibility với device cũ.

---

## 3. Cần thêm / sửa gì

| Nhóm | Cần làm |
|---|---|
| CBOR | decode/encode generic enums + decimals |
| Schema | compact feature metadata, bỏ runtime state |
| Template | fix FAN/DIMMER, thêm GENERIC_VALUE |
| Persistence | bump schema store v2 |
| State | seed BOOL + INT |
| Validation | feature/property/tool consistency |
| Compatibility | fallback title/decimals cho device cũ |

---

## 4. Cbor codec

Update cả enum/constants và `gw_message_t` giống Device:

```c
GW_FEATURE_GENERIC_VALUE = 2
GW_PROP_VALUE = 8
CBOR_KEY_FEATURE_DECIMALS = 31
```

Thêm:

```c
uint8_t feature_decimals;
int has_feature_decimals;
```

Reuse:

```text
capability_label -> title cho feature_item
capability_unit  -> feature unit
```

---

## 5. Compact `device_schema_feature_t`

Target:

```c
typedef struct {
    device_feature_id_t feature_id;
    char title[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];

    uint8_t feature_type;
    uint16_t feature_schema_version;
    uint16_t feature_flags;

    uint8_t property_id;
    uint8_t value_type;
    uint8_t decimals;

    int8_t writable_tool_index;
} device_schema_feature_t;
```

Bỏ khỏi schema:

```text
feature_value_bool
feature_value_int
min_value
max_value
step
```

Lý do:

```text
runtime value -> device_state
write range   -> device_schema_tool_t
```

---

## 6. Feature staging

Trong `device_schema_protocol.c`:

1. validate required:
   - snapshot_id;
   - sequence;
   - feature_id;
   - feature_type;
   - property_id;
   - value_type;
2. copy title:
   ```text
   capability_label nếu có
   fallback feature_id
   ```
3. copy unit;
4. decimals default=0;
5. resolve `feature_tool` nếu có;
6. validate bound tool:
   - tool exists;
   - type matches;
   - unit compatible;
7. check duplicate feature ID;
8. validate template/property mapping.

---

## 7. Template fixes

Current target corrections:

```text
DIMMABLE_LIGHT
  primary_property = LEVEL

FAN
  primary_property = PERCENT_SETTING
```

Add:

```text
GENERIC_VALUE
  semantic_name = "value"
  primary_property = VALUE
```

Property info:

```text
VALUE -> INT
```

Nếu `feature_type` known và property không khớp primary property:

```text
schema item reject
```

Không silently accept invalid semantic pair.

---

## 8. Persistence migration

Hiện schema store:

```c
#define SCHEMA_STORE_SCHEMA_VERSION 1
```

và ghi nguyên arrays của `device_schema_tool_t` / `device_schema_feature_t`.

V2 đổi struct layout nên phải:

```c
#define SCHEMA_STORE_SCHEMA_VERSION 2
```

Load policy:

- v2 blob đúng size/version → load;
- v1 blob → ignore và erase/rewrite sau discovery;
- corrupted length → erase/log;
- không cast v1 bytes thành v2 struct.

Recommended:

```text
on load version mismatch:
  log
  nvs_erase_key()
  continue
```

---

## 9. Runtime state ownership

`device_state` là nơi duy nhất giữ:

```text
bool/int value
valid
updated_at
```

`device_schema_snapshot_t` không giữ current feature value.

Web API enrich state từ `device_state_snapshot()`.

---

## 10. Initial state seed

Hiện Gateway seed chỉ active-read BOOL properties.

V2:

```text
for each committed feature
  if property != NONE:
      submit read_feature_state
```

Không hard-code:

```text
ON_OFF
CONTACT
```

Device sẽ dispatch BOOL/INT theo feature registry.

Gateway command ACK sẽ update `device_state`.

Để tránh burst:

- tối đa 10–12 reads;
- dùng existing command service queue;
- best effort;
- nếu queue đầy, retry/coalesce nếu cần;
- spontaneous event vẫn có thể populate cache.

---

## 11. Writable validation

Khi resolve `writable_tool_index`:

```text
feature.value_type == tool.value_type
```

Nếu unit feature/tool đều non-empty:

```text
strcmp(unit) == 0
```

Generic numeric feature:

- INT;
- bound tool INT;
- tool step > 0;
- min <= max.

Read-only:

```text
writable_tool_index = -1
```

---

## 12. Sửa ở đâu

### Gateway repo

| File | Thay đổi |
|---|---|
| `components/cbor_codec/include/cbor_codec.h` | enum/property/key/message field |
| `components/cbor_codec/cbor_codec.c` | encode/decode decimals |
| `components/device_schema/include/device_schema.h` | compact feature struct |
| `components/device_schema/device_schema_protocol.c` | stage/validate metadata |
| `components/device_schema/device_schema_validate.c` | semantic/tool validation |
| `components/device_schema/device_schema_store.c` | schema version 2 migration |
| `components/device_template/device_template.c` | generic + FAN/DIMMER fixes |
| `components/device_template/include/device_template.h` | nếu cần enum helper |
| `components/device_state/device_state.c` | seed INT + BOOL |
| `components/device_schema/test/*` | schema tests |
| `components/device_template/test/*` | template tests |
| `components/cbor_codec/test/*` | key31 tests |
| `components/device_state/test/*` | seed typed state |

---

## 13. Checklist

### Codec

- [ ] enum giống Device.
- [ ] decimals key=31.
- [ ] absent decimals -> 0.
- [ ] old messages decode.
- [ ] generic value decode.

### Schema

- [ ] no runtime value in feature struct.
- [ ] title fallback.
- [ ] unit stored.
- [ ] value_type stored.
- [ ] decimals stored.
- [ ] duplicate feature reject.
- [ ] writable tool resolve.
- [ ] type mismatch reject.
- [ ] semantic property mismatch reject.

### Template

- [ ] dimmer->LEVEL.
- [ ] fan->PERCENT_SETTING.
- [ ] generic->VALUE.
- [ ] VALUE->INT.

### Persistence

- [ ] store version=2.
- [ ] v1 ignored safely.
- [ ] v1 key cleanup/rewrite.
- [ ] reboot after new schema loads correctly.

### State

- [ ] seed BOOL.
- [ ] seed INT.
- [ ] ACK state apply.
- [ ] event state apply.
- [ ] state remains separate from schema.

---

## 14. Test plan

### T2.1 — Decode old feature

No decimals/title metadata.

Expected fallback:

```text
title=feature_id
decimals=0
```

### T2.2 — Generic feature commit

Expected schema ready.

### T2.3 — FAN template

FAN + PERCENT_SETTING.

Expected valid.

FAN + ON_OFF.

Expected reject under v2 semantic contract.

### T2.4 — DIMMER template

DIMMABLE_LIGHT + LEVEL valid.

### T2.5 — Persistence v1

Load old blob.

Expected:

- no crash;
- no invalid memcpy interpretation;
- record invalidated;
- refresh creates v2 blob.

### T2.6 — Persistence v2 reboot

Discover → persist → reboot.

Expected same title/unit/decimals/tool mapping.

### T2.7 — Initial BOOL seed

Expected state cache valid.

### T2.8 — Initial INT seed

Fan/temp/generic values.

Expected cache valid without waiting spontaneous event.

### T2.9 — Binding mismatch

Feature INT bound BOOL tool.

Expected schema error/no commit.

### T2.10 — Maximum schema

12 tools + 12 features.

Expected commit ready.

---

## 15. Exit criteria

- [ ] compact schema stable.
- [ ] store migration safe.
- [ ] generic template stable.
- [ ] FAN/DIMMER fixed.
- [ ] INT seed stable.
- [ ] old device fallback works.

## 16. Implementation status

- Gateway schema compact đã chuyển sang `title/unit/value_type/decimals` và bỏ runtime value khỏi schema.
- Đã thêm `GENERIC_VALUE`/`VALUE`, sửa template DIMMER/FAN, validate writable tool, seed read cho BOOL/INT và NVS schema migration version 2.
- Gateway firmware build pass bằng ESP-IDF 6.1-rc1.
- Gateway test firmware build pass.
- Runtime flash/interop chưa xác nhận: cổng USB hiện có chưa được định danh chắc chắn là board Gateway, nên chưa flash để tránh ghi nhầm reference-device.
