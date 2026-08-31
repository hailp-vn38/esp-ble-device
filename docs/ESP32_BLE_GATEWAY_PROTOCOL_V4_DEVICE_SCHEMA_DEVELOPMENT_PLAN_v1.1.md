# ESP32 BLE Gateway — Protocol v4 + Device Schema Development Plan

**Version:** 1.1  
**Date:** 2026-08-31  
**Gateway:** `hailp-vn38/esp-ble-gateway`  
**Device:** `hailp-vn38/esp-ble-device`  
**Target:** ESP32-S3 / ESP-IDF / BLE GATT / Web UI / MCP / Xiaozhi MCP

---

## 1. Mục tiêu

Chuyển toàn bộ Gateway + BLE Device sang **Protocol v4-only** và thay `device_capabilities` bằng **`device_schema`**.

Kiến trúc đích:

```text
BLE Device
   |
   | Protocol v4 / CBOR
   v
Protocol Codec
   |
   v
device_schema
   |\
   | +--> tools[]
   |
   +----> features[]
             |
             +--> device_template
             +--> device_state
                         |
              +----------+----------+
              |          |          |
              v          v          v
           Web UI       MCP     Xiaozhi MCP
```

Matter không còn thuộc kiến trúc.

Semantic source of truth của Gateway:

```text
device_schema + device_state + device_template
```

---

## 2. Quyết định kiến trúc đã khóa

### D1. Chỉ hỗ trợ Protocol v4

```text
v1 -> reject
v2 -> reject
v3 -> reject
v4 -> accept
```

```c
#define GW_PROTOCOL_VERSION 4u

if (msg->protocol_version != GW_PROTOCOL_VERSION) {
    return GW_ERR_UNSUPPORTED_VERSION;
}
```

Không giữ compatibility path v1/v2/v3.

### D2. Chuyển hẳn sang `device_schema`

Xóa:

```text
components/device_capabilities/
```

Thay bằng:

```text
components/device_schema/
```

Không giữ compatibility facade tên `device_capabilities`.

Wire v4 hiện tại vẫn giữ các message:

```text
describe_capabilities
capabilities_begin
capability_item
feature_item
capabilities_end
```

Nhưng Gateway coi toàn bộ transaction này là **device schema discovery**.

### D3. Xóa `device_type` cấp thiết bị

Device không còn category `light/fan/sensor/plug/generic`.

Device chỉ chứa:

```text
device_id
name
BLE identity
tools[]
features[]
runtime state
```

Phải xóa:

```text
GW_MSG_DEVICE_TYPE_LEN
GW_KEY_DEVICE_TYPE
gw_message_t.device_type
DEVICE_TYPE_MAX_LEN
device_entry_t.type
device_app_profile_t.device_type
"device_type" trong product.json
"type" trong /api/devices
```

Không xóa:

- `gw_message_t.type`: đây là loại message.
- `feature_type`: đây là semantic type để chọn template.

### D4. Không migrate persisted capability-v3 blob

Namespace cũ:

```text
dev_caps
```

Không migrate.

Namespace mới:

```text
dev_schema
```

Gateway load `dev_schema`, bỏ qua `dev_caps`, sau đó rediscover bằng Protocol v4 khi cần. Có thể erase `dev_caps` để thu hồi NVS, nhưng cleanup lỗi không được block boot.

### D5. Giữ device registry nhưng bỏ type

`device_store` vẫn giữ:

```text
device_id
name
ble_addr
ble_addr_type
has_ble_identity
```

Khuyến nghị:

```text
DEVICE_STORE_SCHEMA_VERSION 2 -> 3
```

Migration registry v2 -> v3 chỉ bỏ `type_N`, vẫn giữ ID/name/BLE identity để user không phải add/pair lại.

---

## 3. Protocol v4 semantic contract

Các key semantic giữ nguyên:

```text
22 feature_id
23 feature_type
24 feature_schema_version
25 feature_flags
26 property_id
27 feature_value_bool
28 feature_value_int
29 feature_tool
30 feature_total
```

`GW_KEY_DEVICE_TYPE = 7` được đổi thành:

```c
GW_KEY_RESERVED_7 = 7
```

Không renumber key 8..30.

Policy:

```text
encoder -> không emit key 7
decoder -> key 7 thì ignore
```

Feature template được chọn bằng:

```text
feature_type + feature_schema_version
```

---

## 4. Data model Gateway

### Tool

```c
typedef struct {
    char command[GW_MSG_COMMAND_LEN];
    char label[GW_MSG_CAP_LABEL_LEN];
    char unit[GW_MSG_CAP_UNIT_LEN];
    uint8_t value_type;
    uint8_t flags;
    int32_t min_value;
    int32_t max_value;
    uint32_t step;
} device_schema_tool_t;
```

### Feature

```c
typedef struct {
    char feature_id[GW_FEATURE_ID_LEN];
    gw_feature_type_t feature_type;
    uint16_t schema_version;
    uint16_t flags;
    uint8_t property_id;
    uint8_t value_type;
    char write_tool[GW_MSG_COMMAND_LEN];
} device_schema_feature_t;
```

### Schema info

```c
typedef enum {
    DEVICE_SCHEMA_STATE_UNKNOWN = 0,
    DEVICE_SCHEMA_STATE_DISCOVERING,
    DEVICE_SCHEMA_STATE_READY,
    DEVICE_SCHEMA_STATE_ERROR,
} device_schema_state_t;

typedef struct {
    char device_id[GW_MSG_DEVICE_ID_LEN];
    device_schema_state_t state;
    uint32_t revision;
    uint16_t tool_count;
    uint16_t feature_count;
} device_schema_info_t;
```

Limits ban đầu:

```c
#define DEVICE_SCHEMA_MAX_TOOLS    12
#define DEVICE_SCHEMA_MAX_FEATURES  8
```

Không dùng giant snapshot API copy-by-value. Dùng indexed getter.

---

# 5. Phase implementation plan + checklist

## PHASE V4-01 — Freeze BLE Device Protocol v4

### Mục tiêu

Đưa `esp-ble-device` về strict v4 và xóa `device_type`.

### Phạm vi

```text
components/gateway_protocol/
components/device_app/
devices/reference_device/
test/host/
```

### Checklist implementation

- [ ] Set `GW_PROTOCOL_VERSION = 4` ở single source of truth.
- [ ] Xóa `GW_MSG_DEVICE_TYPE_LEN`.
- [ ] Xóa `gw_message_t.device_type`.
- [ ] Đổi `GW_KEY_DEVICE_TYPE = 7` thành `GW_KEY_RESERVED_7 = 7`.
- [ ] Encoder không emit key 7.
- [ ] Decoder ignore key 7.
- [ ] Decoder chỉ accept protocol version 4.
- [ ] Xóa default/fallback về Protocol v3.
- [ ] Xóa toàn bộ v1/v2/v3 compatibility branch.
- [ ] Xóa `device_app_profile_t.device_type`.
- [ ] Xóa log `type=%s` trong `device_app`.
- [ ] Xóa `.device_type = ...` khỏi reference product.
- [ ] Xóa `"device_type"` khỏi `product.json`.
- [ ] Đổi `product.json.protocol_version` thành `4`.
- [ ] Xóa command fallback kiểu `protocol_version >= 3 ? bool : int`.
- [ ] Giữ `gw_message_t.type`.
- [ ] Giữ `feature_type` và `gw_feature_type_t`.
- [ ] Update README/comment thành v4-only.

### Checklist test

- [ ] Encode/decode `device_command` v4.
- [ ] Encode/decode `device_ack` v4.
- [ ] Encode/decode `device_event` v4.
- [ ] Encode/decode `capabilities_begin`.
- [ ] Encode/decode `capability_item`.
- [ ] Encode/decode `feature_item`.
- [ ] Encode/decode `capabilities_end`.
- [ ] Encode/decode `read_feature_state`.
- [ ] Encode/decode `feature_state`.
- [ ] v1 reject.
- [ ] v2 reject.
- [ ] v3 reject.
- [ ] v4 accept.
- [ ] Payload có reserved key 7 không crash decoder.

### Exit criteria

- [ ] Device chỉ emit/accept v4.
- [ ] Không còn device-level type trong Device runtime API.
- [ ] Host tests pass.
- [ ] Reference firmware build pass.

---

## PHASE V4-02 — Upgrade Gateway codec sang v4

### Mục tiêu

Đưa `esp-ble-gateway/components/cbor_codec` lên đúng v4 contract.

### Checklist implementation

- [ ] Set `GW_PROTOCOL_VERSION = 4`.
- [ ] Xóa `GW_MSG_DEVICE_TYPE_LEN`.
- [ ] Xóa `gw_message_t.device_type`.
- [ ] Reserve CBOR key 7.
- [ ] Thêm `GW_FEATURE_ID_LEN`.
- [ ] Thêm `feature_id` + `has_feature_id`.
- [ ] Thêm `feature_type` + `has_feature_type`.
- [ ] Thêm `feature_schema_version`.
- [ ] Thêm `feature_flags`.
- [ ] Thêm `property_id`.
- [ ] Thêm `feature_value_bool`.
- [ ] Thêm `feature_value_int`.
- [ ] Thêm `feature_tool`.
- [ ] Thêm `feature_total`.
- [ ] Thêm `gw_feature_type_t`.
- [ ] Thêm `gw_feature_property_t`.
- [ ] Decoder yêu cầu explicit protocol version.
- [ ] Decoder chỉ accept v4.
- [ ] Encoder chỉ emit v4.
- [ ] JSON helpers không expose device_type.

### Checklist interoperability

- [ ] Device `feature_item` decode được ở Gateway.
- [ ] Gateway `read_feature_state` decode được ở Device.
- [ ] Device `feature_state` decode được ở Gateway.
- [ ] Numeric CBOR keys 22..30 giống tuyệt đối hai repo.
- [ ] Không có enum/value drift giữa hai repo.

### Exit criteria

- [ ] Gateway decode được v4 discovery hiện tại của Device.
- [ ] Gateway reject v1/v2/v3.
- [ ] Codec tests pass.
- [ ] Interop tests pass.

---

## PHASE V4-03 — Replace `device_capabilities` with `device_schema`

### Mục tiêu

Xóa hoàn toàn domain/component `device_capabilities` và thay bằng `device_schema` hỗ trợ tools + features.

### Checklist component

- [ ] Tạo `components/device_schema/CMakeLists.txt`.
- [ ] Tạo `include/device_schema.h`.
- [ ] Tạo `device_schema.c`.
- [ ] Tạo validation helpers.
- [ ] Port worker/queue logic cần thiết từ `device_capabilities`.
- [ ] Port global serializer nếu vẫn cần để serialize discovery.
- [ ] Đổi log tag thành `device_schema`.
- [ ] Đổi internal prefix `CAP_*` thành `SCHEMA_*`.

### Checklist data model

- [ ] Tạo `device_schema_tool_t`.
- [ ] Tạo `device_schema_feature_t`.
- [ ] Tạo `device_schema_info_t`.
- [ ] Tạo `DEVICE_SCHEMA_MAX_TOOLS`.
- [ ] Tạo `DEVICE_SCHEMA_MAX_FEATURES`.
- [ ] Xóa giant public snapshot API.
- [ ] Dùng indexed copy-out getter.

### Checklist discovery

- [ ] `capabilities_begin` start staging.
- [ ] Parse `total` thành expected tool count.
- [ ] Parse `feature_total` thành expected feature count.
- [ ] Store `snapshot_id`.
- [ ] Store revision.
- [ ] `capability_item` append vào `staging.tools`.
- [ ] `feature_item` append vào `staging.features`.
- [ ] Validate duplicate command.
- [ ] Validate duplicate feature_id.
- [ ] Validate sequence bounds.
- [ ] Validate feature type/schema/property.
- [ ] Resolve writable `feature_tool`.
- [ ] `capabilities_end` chỉ commit khi counts match.
- [ ] Commit atomic.
- [ ] Refresh fail giữ committed schema cũ.

### Checklist rename/integration

- [ ] `device_capabilities_init` -> `device_schema_init`.
- [ ] `device_capabilities_on_ready` -> `device_schema_on_ready`.
- [ ] `device_capabilities_on_disconnect` -> `device_schema_on_disconnect`.
- [ ] `device_capabilities_on_notify` -> `device_schema_on_notify`.
- [ ] `device_capabilities_refresh` -> `device_schema_refresh`.
- [ ] `device_capabilities_forget` -> `device_schema_forget`.
- [ ] Xóa mọi include `device_capabilities.h`.
- [ ] Xóa `components/device_capabilities` khỏi build graph.

### Checklist test

- [ ] 2 tools + 1 feature commit thành công.
- [ ] Missing tool item -> staging fail.
- [ ] Missing feature item -> staging fail.
- [ ] Duplicate command -> fail.
- [ ] Duplicate feature_id -> fail.
- [ ] Missing `feature_tool` target -> fail với writable feature.
- [ ] Refresh fail giữ schema trước đó.

### Exit criteria

- [ ] Không còn `device_capabilities` trong Gateway domain/build.
- [ ] Schema READY chứa cả tools và features.
- [ ] Auto discovery sau BLE READY hoạt động.
- [ ] Manual schema refresh hoạt động.

---

## PHASE V4-04 — New `dev_schema` persistence

### Mục tiêu

Persist schema mới và loại bỏ hoàn toàn capability-v3 cache.

### Checklist implementation

- [ ] Tạo namespace `dev_schema`.
- [ ] Tạo `DEVICE_SCHEMA_STORE_VERSION = 1`.
- [ ] Persist committed schema בלבד.
- [ ] Persist `tool_count`.
- [ ] Persist `feature_count`.
- [ ] Persist revision.
- [ ] Persist tools.
- [ ] Persist features.
- [ ] Dùng variable-length blob nếu phù hợp.
- [ ] Không persist staging.
- [ ] Không persist runtime feature state.
- [ ] Không load `dev_caps` vào schema mới.
- [ ] Không viết migration capability-v3 -> schema-v4.
- [ ] Optional cleanup erase `dev_caps`.
- [ ] Cleanup lỗi không block boot.

### Checklist test

- [ ] NVS sạch -> discovery bình thường.
- [ ] Reboot với valid `dev_schema` -> load được.
- [ ] Corrupt `dev_schema` -> ignore safely.
- [ ] Existing `dev_caps` -> không migrate.
- [ ] Existing `dev_caps` -> rediscover v4 thành công.
- [ ] Cleanup `dev_caps` không ảnh hưởng `dev_list`.

### Exit criteria

- [ ] Không còn code load/migrate v3 capability blob.
- [ ] Schema persistence độc lập hoạt động.

---

## PHASE V4-05 — Remove device type from Gateway Store/API

### Mục tiêu

Xóa device-level type khỏi Gateway nhưng giữ registered device identity.

### Checklist device_store

- [ ] Xóa `DEVICE_TYPE_MAX_LEN`.
- [ ] Xóa `device_entry_t.type`.
- [ ] `device_store_add(device_id, name, type)` -> `(device_id, name)`.
- [ ] `device_store_edit(device_id, name, type)` -> `(device_id, name)`.
- [ ] Bump `DEVICE_STORE_SCHEMA_VERSION` lên 3.
- [ ] Writer không ghi `type_N`.
- [ ] Loader schema 3 không cần type.
- [ ] Migration v2 -> v3 giữ id/name/BLE identity.
- [ ] Migration v2 -> v3 bỏ `type_N`.
- [ ] Erase obsolete `type_N` sau successful rewrite.

### Checklist dispatcher/API

- [ ] `add_device` không default `generic`.
- [ ] `add_device` không đọc `msg.device_type`.
- [ ] `edit_device` chỉ edit name.
- [ ] Error text không còn nhắc device_type.
- [ ] `list_devices` không emit `type`.
- [ ] Delete flow gọi `device_schema_forget`.
- [ ] POST `/api/devices` không nhận `type`.
- [ ] PUT `/api/devices` không nhận `type`.
- [ ] GET `/api/devices` không trả `type`.
- [ ] Frontend không còn device type selector/badge.

### Checklist test

- [ ] Existing `dev_list` v2 migrate được sang v3.
- [ ] `device_id` giữ nguyên.
- [ ] `name` giữ nguyên.
- [ ] BLE address giữ nguyên.
- [ ] BLE address type giữ nguyên.
- [ ] Device reconnect không cần add lại.

### Exit criteria

- [ ] Không còn device-level type trong Gateway domain model/API/UI.
- [ ] Existing registered devices vẫn dùng được.

---

## PHASE V4-06 — Runtime `device_state`

### Mục tiêu

Tạo feature/property runtime state độc lập với schema.

### Checklist implementation

- [ ] Tạo `components/device_state`.
- [ ] State key = `(device_id, feature_id, property_id)`.
- [ ] Hỗ trợ BOOL.
- [ ] Hỗ trợ INT.
- [ ] Có `valid` flag.
- [ ] Có update timestamp.
- [ ] Không persist NVS.
- [ ] Implement `device_state_on_notify`.
- [ ] Consume `feature_state` event.
- [ ] Route theo gateway connection `device_id`.
- [ ] Không dùng native model ID để route state.

### Checklist state seed

- [ ] Sau schema commit enumerate readable features.
- [ ] Gửi `read_feature_state`.
- [ ] Include `feature_id`.
- [ ] Include `property_id`.
- [ ] ACK bool update đúng state.
- [ ] ACK int update đúng state.
- [ ] Read failure không invalidate schema.

### Checklist test

- [ ] LED state seed đúng sau connect.
- [ ] Local action tạo `feature_state`.
- [ ] Gateway update state không cần rediscovery.
- [ ] Hai device cùng `feature_id=led_main` không cross-update.
- [ ] Disconnect/reconnect reseed đúng.

### Exit criteria

- [ ] Web/MCP đọc được semantic state từ Gateway cache.
- [ ] Semantic state không phụ thuộc legacy `get_state`.

---

## PHASE V4-07 — Device Template engine

### Mục tiêu

Map semantic feature sang presentation/control template.

### Checklist implementation

- [ ] Tạo `components/device_template`.
- [ ] Tạo static template registry.
- [ ] Lookup bằng `(feature_type, schema_version)`.
- [ ] Không lookup bằng device type.
- [ ] Không lookup bằng raw command name.
- [ ] Implement `on_off_light.v1`.
- [ ] Primary property = `GW_PROP_ON_OFF`.
- [ ] Semantic name = `light`.
- [ ] Write qua `feature.write_tool`.
- [ ] Không hardcode `set_led`.
- [ ] Unknown template trả unsupported, không crash.

### Checklist test

- [ ] `ON_OFF_LIGHT + schema 1` resolve đúng.
- [ ] `feature_tool=set_led` write đúng.
- [ ] `feature_tool=power` vẫn dùng same template.
- [ ] Unknown schema version degrade safe.
- [ ] Unknown feature type degrade safe.

### Exit criteria

- [ ] Semantic layer không phụ thuộc device category.
- [ ] Template registry không yêu cầu runtime JSON parser/heap.

---

## PHASE V4-08 — Web Device Schema API + Semantic UI

### Mục tiêu

Web render theo semantic features thay vì capability list/device type.

### Checklist API

- [ ] Remove/rename `list_device_capabilities`.
- [ ] Add `get_device_schema`.
- [ ] Add `refresh_device_schema` nếu cần.
- [ ] `web_capability_api.c` -> `web_device_schema_api.c`.
- [ ] API trả schema state.
- [ ] API trả revision.
- [ ] API trả `tools[]`.
- [ ] API trả `features[]`.
- [ ] API trả template id.
- [ ] API trả current state/state_valid.
- [ ] API không trả device type.

### Checklist frontend

- [ ] Device detail load schema API.
- [ ] Render feature cards.
- [ ] Implement `on_off_light.v1` toggle.
- [ ] Toggle write qua semantic binding.
- [ ] UI update sau ACK/event.
- [ ] Unknown template hiển thị unsupported.
- [ ] Có optional Advanced/Raw Tools section.
- [ ] Feature-bound raw tool không duplicate mặc định.

### Checklist test

- [ ] Reference LED hiển thị toggle.
- [ ] Toggle điều khiển đúng device.
- [ ] Local state change phản ánh lên UI.
- [ ] Multi-feature device render nhiều cards.
- [ ] Device list không còn type.

### Exit criteria

- [ ] Web UI dùng feature/template làm presentation source.
- [ ] Không còn UI branch theo device type.

---

## PHASE V4-09 — Semantic MCP + Xiaozhi MCP

### Mục tiêu

Expose semantic tools dựa trên schema/template, dùng chung cho local MCP và Xiaozhi.

### Checklist core catalog

- [ ] `mcp_tool_exposure` đọc `device_schema`.
- [ ] `mcp_tool_exposure` đọc `device_template`.
- [ ] Đọc `device_state` khi cần.
- [ ] Tạo semantic tool cho supported feature.
- [ ] Tool name ổn định theo device + feature.
- [ ] Semantic write route qua `feature_tool`.
- [ ] Không hardcode device command.

### Checklist duplicate policy

- [ ] Raw tool bound vào feature bị hide mặc định.
- [ ] Raw unbound tool vẫn expose.
- [ ] Advanced/debug mode có thể expose raw bound tools nếu cần.
- [ ] Default catalog không có 2 tools làm cùng action.

### Checklist Xiaozhi

- [ ] Xiaozhi dùng cùng semantic catalog.
- [ ] Không duplicate template mapping ở Xiaozhi layer.
- [ ] Parameter schema giống local MCP.
- [ ] State/read behavior giống local MCP.

### Checklist test

- [ ] `led_main` expose semantic light control.
- [ ] `set_led` không duplicate mặc định.
- [ ] Raw unbound command vẫn xuất hiện.
- [ ] Local MCP control được LED.
- [ ] Xiaozhi MCP control được cùng LED.

### Exit criteria

- [ ] Một semantic catalog phục vụ cả local MCP và Xiaozhi.
- [ ] MCP không phụ thuộc device type.

---

## PHASE V4-10 — Cleanup + Hardening

### Mục tiêu

Xóa toàn bộ legacy path và khóa kiến trúc v4.

### Checklist cleanup

- [ ] Xóa `components/device_capabilities`.
- [ ] Xóa mọi include `device_capabilities.h`.
- [ ] Xóa v1/v2/v3 protocol branches.
- [ ] Xóa v3 compatibility tests.
- [ ] Xóa `device_type` khỏi Gateway.
- [ ] Xóa `device_type` khỏi Device.
- [ ] Xóa `dev_caps` loading code.
- [ ] Xóa UI device type.
- [ ] Xóa MCP metadata dựa trên device type.
- [ ] Xóa semantic dependency vào legacy `get_state`.
- [ ] Update docs toàn repo thành v4-only.
- [ ] Update diagrams và component dependencies.

### Checklist hardening

- [ ] Validate string lengths.
- [ ] Validate counts trước copy/allocation.
- [ ] Validate duplicate schema items.
- [ ] Validate snapshot transaction IDs.
- [ ] Validate `feature_tool` reference.
- [ ] Unknown CBOR keys không crash.
- [ ] Unknown feature template không crash consumers.
- [ ] Queue overflow có metrics/log throttling.
- [ ] Schema refresh timeout recover được.
- [ ] Disconnect giữa discovery rollback staging đúng.
- [ ] Delete device xóa schema + state + MCP exposure.

### Exit criteria

- [ ] Source tree không còn legacy domain concepts.
- [ ] Full test suite pass.
- [ ] Hardware E2E pass.
- [ ] Memory/leak test pass.

---

# 6. Release-gate test matrix

| ID | Test | Expected |
|---|---|---|
| T01 | Protocol v1 | Reject |
| T02 | Protocol v2 | Reject |
| T03 | Protocol v3 | Reject |
| T04 | Protocol v4 | Accept |
| T05 | 2 tools + 1 feature discovery | Atomic commit READY |
| T06 | Missing tool item | Reject staging |
| T07 | Missing feature item | Reject staging |
| T08 | Duplicate command | Reject staging |
| T09 | Duplicate feature_id | Reject staging |
| T10 | Missing feature_tool target | Reject writable feature/schema |
| T11 | `read_feature_state` BOOL | State cache seeded |
| T12 | `feature_state` BOOL | Runtime cache updated |
| T13 | Two devices same native model | State isolated by gateway device_id |
| T14 | Multi-feature device | Multiple templates rendered |
| T15 | Unknown feature type/schema | Safe unsupported fallback |
| T16 | Existing `dev_caps` | No migration, rediscover v4 |
| T17 | Existing `dev_list` v2 | Preserve ID/name/BLE identity, drop type |
| T18 | Reboot with `dev_schema` | Schema restored |
| T19 | Corrupt schema NVS | Ignore safely / rediscover |
| T20 | Disconnect during discovery | Rollback staging |
| T21 | Refresh failure | Previous committed schema retained |
| T22 | Local device state change | Web/MCP state updates |
| T23 | Semantic MCP duplicate policy | No duplicate bound raw tool |
| T24 | 16 devices x 12 tools x 8 features | No stack overflow/leak |

---

# 7. Memory acceptance checklist

- [ ] Per-device records dùng PSRAM-preferred allocation.
- [ ] Staging schema dùng PSRAM-preferred allocation.
- [ ] Queue items không embed schema snapshot lớn.
- [ ] `gw_message_t` copies được bounded.
- [ ] HTTP handlers không đặt whole schema lên stack.
- [ ] MCP handlers không đặt whole schema lên stack.
- [ ] Đo worker stack high-water mark sau migration.
- [ ] So sánh heap trước/sau 100 refresh cycles.
- [ ] Không có monotonic PSRAM loss.
- [ ] Không có monotonic internal SRAM loss.
- [ ] Largest free internal block vẫn trong ngưỡng an toàn.

---

# 8. Reference LED expected schema

```json
{
  "device_id": "gateway-assigned-id",
  "revision": 2,
  "tools": [
    {
      "name": "set_led",
      "value_type": "boolean"
    },
    {
      "name": "get_state",
      "value_type": "none"
    }
  ],
  "features": [
    {
      "feature_id": "led_main",
      "feature_type": "on_off_light",
      "schema_version": 1,
      "property": "on_off",
      "write_tool": "set_led",
      "template": "on_off_light.v1"
    }
  ]
}
```

Không còn:

```json
"device_type": "light"
```

---

# 9. Exact rename map

| Old | New |
|---|---|
| `components/device_capabilities` | `components/device_schema` |
| `device_capabilities.h` | `device_schema.h` |
| `device_capability_t` | `device_schema_tool_t` |
| `device_capability_snapshot_t` | remove |
| `DEVICE_CAP_MAX_PER_DEVICE` | `DEVICE_SCHEMA_MAX_TOOLS` |
| `device_capabilities_init` | `device_schema_init` |
| `device_capabilities_on_ready` | `device_schema_on_ready` |
| `device_capabilities_on_disconnect` | `device_schema_on_disconnect` |
| `device_capabilities_on_notify` | `device_schema_on_notify` |
| `device_capabilities_refresh` | `device_schema_refresh` |
| `device_capabilities_forget` | `device_schema_forget` |
| `device_capabilities_get` | remove |
| `list_device_capabilities` | `get_device_schema` |
| `web_capability_api.c` | `web_device_schema_api.c` |
| `dev_caps` | `dev_schema` |
| `CAP_*` internal prefix | `SCHEMA_*` |
| `device_caps` log tag | `device_schema` |

---

# 10. Device-type removal search checklist

## Gateway

- [ ] `GW_MSG_DEVICE_TYPE_LEN`
- [ ] `GW_KEY_DEVICE_TYPE`
- [ ] `gw_message_t.device_type`
- [ ] `DEVICE_TYPE_MAX_LEN`
- [ ] `device_entry_t.type`
- [ ] `device_store_add(... type)`
- [ ] `device_store_edit(... type)`
- [ ] `type_N` NVS writer
- [ ] `"type"` trong `/api/devices`
- [ ] Device type form field
- [ ] Device type UI badge
- [ ] MCP metadata dựa trên device type
- [ ] Tests expecting `generic`

## Device

- [ ] `GW_MSG_DEVICE_TYPE_LEN`
- [ ] `GW_KEY_DEVICE_TYPE`
- [ ] `gw_message_t.device_type`
- [ ] `device_app_profile_t.device_type`
- [ ] `.device_type = ...`
- [ ] `"device_type"` trong product.json
- [ ] Device type logs
- [ ] Codec tests cho device_type

## Must remain

- [ ] `gw_message_t.type`
- [ ] `GW_MSG_TYPE_*`
- [ ] `feature_type`
- [ ] `gw_feature_type_t`

---

# 11. Definition of Done

- [ ] Gateway không còn component `device_capabilities`.
- [ ] Gateway domain model chính là `device_schema`.
- [ ] Gateway chỉ accept Protocol v4.
- [ ] BLE Device chỉ accept/emit Protocol v4.
- [ ] `device_type` bị xóa khỏi cả hai repository.
- [ ] `gw_message_t.type` vẫn hoạt động cho protocol routing.
- [ ] `feature_type` vẫn hoạt động cho template selection.
- [ ] `dev_caps` không được migrate.
- [ ] Device registry giữ BLE identity sau khi drop type.
- [ ] Schema discovery commit atomic tools + features.
- [ ] `read_feature_state` seed state đúng.
- [ ] `feature_state` update state đúng.
- [ ] Web UI render theo Device Template.
- [ ] MCP và Xiaozhi dùng cùng semantic catalog.
- [ ] Feature writes route qua `feature_tool`.
- [ ] Không hardcode command theo feature type.
- [ ] Unknown feature/template degrade safely.
- [ ] Schema records ưu tiên PSRAM.
- [ ] Không có giant snapshot copy lên task stack.
- [ ] Interop tests pass.
- [ ] Gateway unit tests pass.
- [ ] Device host tests pass.
- [ ] Hardware E2E pass.
- [ ] Memory/leak tests pass.

---

# 12. Kiến trúc cuối cùng

```text
Physical Device
    |
    | no device category
    |
    +-- Tool: set_led
    +-- Tool: get_state
    |
    +-- Feature: led_main
           |
           +-- type: ON_OFF_LIGHT
           +-- schema: 1
           +-- property: ON_OFF
           +-- write_tool: set_led
                  |
                  v
          Device Template
                  |
          +-------+-------+
          |       |       |
          v       v       v
         Web     MCP   Xiaozhi
```

Protocol v4 là BLE application protocol duy nhất được hỗ trợ.
