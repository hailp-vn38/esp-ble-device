# ESP BLE Device Settings v2 — Fix & Hardening Implementation Guide v1.1

**Status:** Implementation-ready for `hailp-vn38/esp-ble-device`  
**Device baseline reviewed:** `main` @ `d87bf1cc163575434b1f2157bbfbd2aa7cf265cc`  
**Target MCU:** ESP32-S3  
**Protocol target:** ESP-GATT Protocol v4 + additive Device Settings extension  
**Scope:** BLE Device firmware only. Gateway implementation is an external protocol consumer/provider.  
**Supersedes:** `ESP_BLE_DEVICE_SETTINGS_FIX_IMPLEMENTATION_GUIDE_v1.0.md` and its review.

---

# 1. Purpose

This document converts the v1.0 review into a single implementation specification for `esp-ble-device`.

The goal is not to redesign Device Settings again. The goal is to make the current implementation safe enough to build, test, integrate with the Gateway, and ship as a reusable device framework.

After this document is implemented, the following must be true:

1. Device Settings can be registered by any product without product-specific logic in generic components.
2. Persistent settings use an active/staging transactional model.
3. A multi-field update is atomic at NVS commit level.
4. `config_revision` is persisted atomically with the configuration blob.
5. Transaction commands are serialized and idempotent where retry is expected.
6. A disconnect before commit never changes persistent configuration.
7. A disconnect after successful commit never rolls back the new configuration.
8. Commit confirmation gives the Gateway a chance to receive the commit result before the device restarts.
9. BLE schema/value streaming uses bounded frames and bounded queues.
10. Settings protocol fields do not continue inflating the common `gw_message_t` structure.
11. Settings discovery has deterministic frame types, revision metadata, and item ordering.
12. `reference_device` and `demo_device` both build and use the same generic Settings subsystem.
13. Secret values are not exposed until the Gateway/Device secret action contract is explicitly verified.
14. Host unit tests, golden vectors, HIL fault tests, and soak tests all pass.

---

# 2. Source baselines and external Gateway contract

## 2.1 Device baseline

This guide is written against:

```text
repo:   hailp-vn38/esp-ble-device
branch: main
sha:    d87bf1cc163575434b1f2157bbfbd2aa7cf265cc
```

The reviewed baseline already contains:

```text
components/device_settings/
components/gateway_protocol/gateway_settings.c
components/gateway_protocol/include/gateway_settings.h
Settings handling inside device_command.c
reference_device Settings configuration
host Settings tests
```

Therefore v1.1 is primarily a correctness/hardening pass, not a green-field implementation.

## 2.2 Gateway visibility note

The Gateway is stated to have been updated to support Device Settings. At review time, the GitHub refs visible to this environment still expose only:

```text
main   -> 01f4b3367e8cd783bd0cd585916ff6cd5aab1926
dev-ws -> 4889c4d7d3578f236d5b632f6c2a7e3e6ef35111
```

Those visible refs do not expose the new Settings implementation.

**Implementation rule:** the local/current Gateway implementation used by the project is the authoritative integration peer. Before merging F3/F4 protocol changes, run the cross-repo contract gate in section 7. Do not invent alternate numeric CBOR keys in the Device repo when the Gateway already has an authoritative definition.

## 2.3 Protocol v4 policy

V1.1 keeps Protocol v4 provided the updated Gateway accepts the additive Settings extension.

Do not bump to v5 unless one of these is true:

- the Gateway Settings implementation uses incompatible meanings for existing numeric keys;
- old-v4 compatibility requires a breaking decoder change;
- Settings cannot be represented without changing an existing key's wire type or meaning.

Adding new message types or optional keys is not by itself sufficient reason to bump protocol version if both decoders tolerate additive data.

---

# 3. V1.1 locked architectural decisions

These decisions are normative.

## D1 — Internal lifecycle ownership

```text
Device Settings core = synchronous state/data layer
Device command worker = single owner of Settings transaction state transitions
BLE callback          = posts internal event only
FreeRTOS timer callback = posts internal event only
Device app            = composition/lifecycle/restart provider
```

No NVS write, transaction mutation, or restart scheduling is performed directly from a NimBLE callback or FreeRTOS timer callback.

## D2 — No dedicated Settings task

Reuse the existing `device_command` worker as the Settings state-machine owner.

Reason:

- avoids an additional task stack;
- provides deterministic serialization with incoming BLE commands;
- eliminates races between command processing, disconnect handling, and timeout handling.

## D3 — Active and staging are separate static product buffers

Generic Settings code does not heap-allocate the product config blobs.

Each product owns:

```text
static active config
static staging config
```

The generic registry stores only pointers.

## D4 — Core owns the persisted header

The product owns only config payload defaults and product-specific fields.

Core owns:

```c
format_version
config_revision
reserved/header invariant
```

A product defaults callback must not be responsible for constructing the generic persisted header.

## D5 — Read always reads active; SET always writes staging

A Settings descriptor has separate contexts:

```text
read_ctx  -> committed active config
stage_ctx -> transaction staging config
```

No schema read callback may expose staged values before commit.

## D6 — No revision in `int_value`

For Settings ACKs:

```text
bool_value = success
int_value  = settings_status only
```

Configuration revision is carried in the Settings revision field, not overloaded into `int_value`.

## D7 — Reuse existing Settings revision wire key

Do not introduce a new key solely to fix status/revision ambiguity.

Key 43 becomes the generic Settings config revision key:

```c
GW_KEY_SETTINGS_CONFIG_REVISION = 43
GW_KEY_SETTINGS_NEW_REVISION = GW_KEY_SETTINGS_CONFIG_REVISION /* compatibility alias */
```

The wire number remains unchanged.

Meaning by context:

- discovery/value stream: current committed `config_revision`;
- BEGIN ACK: current committed `config_revision`;
- COMMIT ACK: newly committed `config_revision`;
- CONFIRM: revision being confirmed.

## D8 — Schema revision is `uint16_t`

V1.1 resolves the previous width mismatch by matching the existing Device wire representation:

```c
typedef uint16_t gw_settings_schema_revision_t;
```

Use `uint16_t` consistently in:

```text
device_app_profile_t
capability advertisement
gateway_protocol.h
Settings begin/end frames
product profile constants
tests
```

Do not cast from `uint32_t` to `uint16_t` at the encoder call site.

If the authoritative updated Gateway explicitly uses `uint32_t`, change both repositories together during F3 before implementation proceeds. Never silently truncate.

## D9 — Config revision is `uint32_t`

```c
typedef uint32_t gw_settings_config_revision_t;
```

Revision zero is valid for factory/default state.

Do not wrap `UINT32_MAX` back to zero. Return `REVISION_EXHAUSTED`.

## D10 — Stream frames have real message types

The following are **message type values**, not command names:

```text
settings_begin
setting_item
setting_option_item
settings_end
settings_values_begin
setting_value
settings_values_end
```

The `command` field remains the originating operation:

```text
describe_settings
read_settings
```

Transaction requests remain normal `device_command` frames with Settings command names.

## D11 — Every Settings frame is routable

Every Device -> Gateway Settings stream frame contains at least:

```text
protocol_version
type
device_id
command
request_id
```

The Device must not rely only on the BLE connection object for routing identity.

## D12 — Settings command execution is strictly sequential

The Gateway may retry a request, but a single device has only one Settings transaction in progress.

The Device does not support parallel Settings transactions.

## D13 — Settings save always restarts in v1.1

After a successful commit/confirm flow the device restarts.

There is no per-setting `REQUIRES_REBOOT` behavior in v1.1.

## D14 — SECRET is not production-enabled in v1.1

The `SECRET` flag may remain reserved in generic definitions, but `reference_device` and `demo_device` must not expose a writable SECRET descriptor unless the Gateway contract has verified:

```text
KEEP
SET(value)
CLEAR
configured-only readback
no plaintext GET/WS/cache
```

---

# 4. Current blocker matrix

## P0 — must be fixed before HIL

| ID | Problem | Result if unfixed |
|---|---|---|
| P0-01 | `device_settings_init()` resets metadata configured before init | config size/version lost |
| P0-02 | active/staging buffers are not formally bound in firmware lifecycle | real firmware load can see NULL config |
| P0-03 | current read callbacks can point at staging | uncommitted data leaks through `read_settings` |
| P0-04 | transaction state touched from command worker, BLE callback, timer callback | race on dual-core ESP32-S3 |
| P0-05 | SET path does not enforce transaction ID at core boundary | wrong transaction can mutate staging |
| P0-06 | COMMIT confirm timer starts inside commit core before ACK enqueue | reboot race / ACK loss |
| P0-07 | CONFIRM currently mixes validation and restart scheduling | restart can race confirm ACK |
| P0-08 | stream encoder emits `type=device_command` for begin/item/end | Gateway cannot deterministically classify stream frames |
| P0-09 | Settings stream frames omit explicit `device_id` | routing contract incomplete |
| P0-10 | success ACK overloads `int_value` with revision while failures use status | revision/status ambiguity |
| P0-11 | schema revision width is inconsistent | truncation/cache mismatch |
| P0-12 | CMake component dependencies are incomplete | IDF build/link failure |
| P0-13 | `reference_config.c` is not guaranteed in product SRCS | Settings registration missing at link/build |
| P0-14 | disconnect while ACTIVE does not deterministically abort staging | stale transaction/busy state |
| P0-15 | Settings frame validation only checks 256-byte logical max, not ATT payload | notify can fail after MTU negotiation |

## P1 — fix before release candidate

| ID | Problem |
|---|---|
| P1-01 | Settings-specific fields unnecessarily enlarge `gw_message_t` |
| P1-02 | callback ABI uses ambiguous `void *` layouts |
| P1-03 | descriptor metadata is not fully preflight-encoded at freeze |
| P1-04 | product defaults/header ownership is unclear |
| P1-05 | transaction timeout should be absolute from BEGIN |
| P1-06 | reverse dependency from `device_settings` to `device_app` |
| P1-07 | generic Settings stubs remain registered alongside custom dispatcher |
| P1-08 | factory reset does not reset Settings NVS/config cleanly |
| P1-09 | invalid NVS recovery policy is not explicit |
| P1-10 | revision overflow is not handled |

## P2 — architecture/maintenance

- duplicate protocol concepts exist between common and specialized codecs;
- command/message constant naming is mixed (`GW_MSG_TYPE_*` used as command strings);
- product Settings registration is not clearly defined as hardware-independent;
- restart lifecycle currently uses timer callback code that may block the timer service task;
- reference SECRET demo is ahead of the verified security contract.

---

# 5. Final component architecture

```text
components/
  gateway_protocol/
    include/gateway_protocol.h
    include/gateway_settings.h
    gateway_protocol.c
    gateway_settings.c

  device_settings/
    include/device_settings.h
    device_settings_registry.c
    device_settings_transaction.c
    device_settings_persistence.c

  device_command/
    include/device_command.h
    device_command.c
      - BLE frame worker
      - Settings protocol dispatcher
      - Settings timeout ownership
      - disconnect/timeout internal events
      - ACK sequencing

  device_app/
    include/device_app.h
    device_app.c
      - NVS init
      - Settings composition
      - BLE lifecycle
      - safe restart implementation

products:
  devices/reference_device/main/
    reference_product.c
    reference_config.h/.c

  devices/demo_device/main/
    demo_product.c
    demo_config.h/.c
    drivers/demo_sensor.h/.c
```

Dependency direction:

```text
product
  ↓
device_app ─────────────→ device_settings
  ↓                          ↓
device_command ───────────→  │
  ↓                          │
gateway_protocol          nvs_flash
  ↓
ble_peripheral
```

Forbidden dependency:

```text
device_settings -> device_app
```

The generic Settings core must not call `device_app_schedule_restart()` directly.

---

# 6. Final public Settings data model

## 6.1 Persisted header

```c
typedef struct {
    uint16_t format_version;
    uint16_t reserved;
    uint32_t config_revision;
} device_settings_blob_header_t;
```

Invariants:

```text
header is first bytes of every product settings blob
reserved == 0
format_version == profile/settings config version
config_revision == registry committed revision
```

## 6.2 Product config layout

Recommended pattern:

```c
typedef struct {
    bool sensor_enabled;
    int32_t sample_interval_s;
    int32_t startup_dryer_temp_c;
    uint8_t startup_fan_mode;
    char device_label[33];
    bool diagnostic_logging;
    char serial_number[17];
} demo_config_payload_t;

typedef struct {
    device_settings_blob_header_t header;
    demo_config_payload_t payload;
} demo_config_t;
```

The generic core operates on the full blob.

Product defaults/validation callbacks operate on payload only.

## 6.3 Product binding API

Replace independent mutable setters with one configuration path.

```c
typedef esp_err_t (*device_settings_defaults_fn)(
    void *payload,
    size_t payload_size);

typedef esp_err_t (*device_settings_validate_fn)(
    const void *payload,
    size_t payload_size);

typedef struct {
    size_t config_size;
    uint16_t format_version;
    void *active_config;
    void *staging_config;
    device_settings_defaults_fn defaults_fn;
    device_settings_validate_fn validate_fn;
} device_settings_storage_config_t;

esp_err_t device_settings_init(void);

esp_err_t device_settings_configure(
    const device_settings_storage_config_t *config);
```

Rules:

- `device_settings_init()` clears internal runtime state only.
- `device_settings_configure()` must be called after init and before registration/freeze.
- configure validates pointers, size, alignment, and minimum header size.
- there is only one configure call per boot.
- remove or make internal the loose setters:
  - `device_settings_set_config_size()`
  - `device_settings_set_format_version()`
  - `device_settings_set_active_config()`
  - `device_settings_set_staging_config()`

## 6.4 Descriptor ABI

Use typed callbacks to eliminate `void *` value ambiguity.

```c
typedef struct {
    uint8_t index;
} device_setting_enum_value_t;

typedef union {
    esp_err_t (*read_bool)(void *ctx, bool *out);
    esp_err_t (*read_int)(void *ctx, int32_t *out);
    esp_err_t (*read_string)(void *ctx, char *out, size_t out_cap);
    esp_err_t (*read_enum)(void *ctx, device_setting_enum_value_t *out);
} device_setting_read_cb_t;

typedef union {
    esp_err_t (*stage_bool)(void *ctx, bool value);
    esp_err_t (*stage_int)(void *ctx, int32_t value);
    esp_err_t (*stage_string)(void *ctx, const char *value, size_t value_len);
    esp_err_t (*stage_enum)(void *ctx, device_setting_enum_value_t value);
} device_setting_stage_cb_t;
```

Descriptor:

```c
typedef struct device_setting_descriptor {
    const char *id;
    const char *title;
    const char *group;
    const char *unit;

    device_setting_type_t type;
    uint16_t flags;

    int32_t min_value;
    int32_t max_value;
    int32_t step;

    uint16_t max_length;

    const device_setting_option_t *options;
    uint8_t option_count;

    device_setting_read_cb_t read;
    device_setting_stage_cb_t stage;

    void *read_ctx;
    void *stage_ctx;
} device_setting_descriptor_t;
```

### Read/stage invariant

```text
read_ctx  = pointer into active config
stage_ctx = pointer into staging config
```

For READONLY:

```text
stage callback must be NULL
```

## 6.5 Core result ABI vs wire status ABI

Keep the storage/transaction core independent from the BLE wire protocol.

`device_settings` defines its own stable result enum:

```c
typedef enum {
    DEVICE_SETTINGS_OK = 0,
    DEVICE_SETTINGS_ERR_INVALID_ARGUMENT,
    DEVICE_SETTINGS_ERR_UNSUPPORTED,
    DEVICE_SETTINGS_ERR_TYPE_MISMATCH,
    DEVICE_SETTINGS_ERR_OUT_OF_RANGE,
    DEVICE_SETTINGS_ERR_READONLY,
    DEVICE_SETTINGS_ERR_REVISION_CONFLICT,
    DEVICE_SETTINGS_ERR_TX_BUSY,
    DEVICE_SETTINGS_ERR_TX_NOT_ACTIVE,
    DEVICE_SETTINGS_ERR_TX_ID_MISMATCH,
    DEVICE_SETTINGS_ERR_VALIDATION_FAILED,
    DEVICE_SETTINGS_ERR_PERSIST_FAILED,
    DEVICE_SETTINGS_ERR_INTERNAL,
    DEVICE_SETTINGS_ERR_TIMEOUT,
    DEVICE_SETTINGS_ERR_REVISION_EXHAUSTED,
} device_settings_result_t;
```

`gateway_settings.h` defines the wire status enum used by Device and Gateway protocol tests:

```c
typedef enum {
    GW_SETTINGS_STATUS_OK = 0,
    GW_SETTINGS_STATUS_INVALID_ARGUMENT = 1,
    GW_SETTINGS_STATUS_UNSUPPORTED = 2,
    GW_SETTINGS_STATUS_TYPE_MISMATCH = 3,
    GW_SETTINGS_STATUS_OUT_OF_RANGE = 4,
    GW_SETTINGS_STATUS_READONLY = 5,
    GW_SETTINGS_STATUS_REVISION_CONFLICT = 6,
    GW_SETTINGS_STATUS_TX_BUSY = 7,
    GW_SETTINGS_STATUS_TX_NOT_ACTIVE = 8,
    GW_SETTINGS_STATUS_TX_ID_MISMATCH = 9,
    GW_SETTINGS_STATUS_VALIDATION_FAILED = 10,
    GW_SETTINGS_STATUS_PERSIST_FAILED = 11,
    GW_SETTINGS_STATUS_INTERNAL_ERROR = 12,
    GW_SETTINGS_STATUS_TIMEOUT = 13,
    GW_SETTINGS_STATUS_REVISION_EXHAUSTED = 14,
    GW_SETTINGS_STATUS_FRAME_TOO_LARGE = 15,
} gw_settings_status_t;
```

`device_command` owns an explicit mapping function:

```c
static gw_settings_status_t settings_result_to_wire(
    device_settings_result_t result);
```

Do not make `device_settings` depend on `gateway_protocol` just to reuse a wire enum, and do not expose raw ESP-IDF `esp_err_t` values over the wire.

---

# 7. Gateway contract gate — mandatory before F3 merge

This phase is not Gateway implementation work. It is a Device-side verification gate against the already-updated Gateway.

## 7.1 Contract items to diff

From the actual Gateway Settings implementation, verify byte-for-byte or symbol-for-symbol:

```text
Protocol version
BLE service/characteristic UUIDs
message type strings
command strings
numeric CBOR keys
setting type enum
setting flags
settings status enum
schema revision width
config revision width
transaction ID width
string length limits
request_id requirements
secret semantics
```

## 7.2 Current Device Settings numeric key baseline

V1.1 assumes the existing additive v4 key allocation is retained:

```text
32 SETTINGS_SUPPORTED
33 SETTINGS_SCHEMA_REVISION
34 SETTINGS_ID
35 SETTINGS_TITLE
36 SETTINGS_GROUP
37 SETTINGS_UNIT
38 SETTINGS_TYPE
39 SETTINGS_FLAGS
40 SETTINGS_VALUE
41 SETTINGS_TRANSACTION_ID
42 SETTINGS_EXPECTED_REVISION
43 SETTINGS_CONFIG_REVISION
44 SETTINGS_OPTION_INDEX
45 SETTINGS_MAX_LENGTH
46 SETTINGS_OPTION_COUNT
47 SETTINGS_SEQUENCE
```

Compatibility alias:

```c
GW_KEY_SETTINGS_NEW_REVISION = GW_KEY_SETTINGS_CONFIG_REVISION;
```

No new numeric key is introduced by v1.1 unless the actual Gateway contract requires it.

## 7.3 Command strings

Use command symbols for commands:

```c
#define GW_COMMAND_DESCRIBE_SETTINGS          "describe_settings"
#define GW_COMMAND_READ_SETTINGS              "read_settings"
#define GW_COMMAND_SETTINGS_TX_BEGIN          "settings_tx_begin"
#define GW_COMMAND_SETTINGS_TX_SET            "settings_tx_set"
#define GW_COMMAND_SETTINGS_TX_COMMIT         "settings_tx_commit"
#define GW_COMMAND_SETTINGS_TX_ABORT          "settings_tx_abort"
#define GW_COMMAND_SETTINGS_COMMIT_CONFIRM    "settings_commit_confirm"
```

Do not use `GW_MSG_TYPE_*` constants to identify incoming transaction commands.

## 7.4 Response message types

```c
#define GW_MSG_TYPE_SETTINGS_BEGIN         "settings_begin"
#define GW_MSG_TYPE_SETTINGS_ITEM          "setting_item"
#define GW_MSG_TYPE_SETTINGS_OPTION_ITEM   "setting_option_item"
#define GW_MSG_TYPE_SETTINGS_END           "settings_end"
#define GW_MSG_TYPE_SETTINGS_VALUES_BEGIN  "settings_values_begin"
#define GW_MSG_TYPE_SETTINGS_VALUE         "setting_value"
#define GW_MSG_TYPE_SETTINGS_VALUES_END    "settings_values_end"
```

## 7.5 Golden vector gate

At least the following vectors must be exported from or validated against the updated Gateway:

1. `describe_settings` request.
2. `settings_begin` response.
3. BOOL `setting_item`.
4. INT `setting_item` with min/max/step/unit.
5. ENUM `setting_item` with option count.
6. `setting_option_item`.
7. STRING `setting_item`.
8. `settings_end`.
9. `read_settings` request.
10. `settings_values_begin`.
11. BOOL value.
12. INT value.
13. ENUM value.
14. STRING value.
15. `settings_values_end`.
16. `settings_tx_begin`.
17. BEGIN ACK success.
18. BEGIN ACK revision conflict.
19. `settings_tx_set` for all four types.
20. SET ACK error.
21. `settings_tx_commit`.
22. COMMIT ACK containing status + config revision.
23. `settings_commit_confirm`.
24. CONFIRM ACK.

### Gate rule

```text
No F3/F4 protocol PR merge until Device vectors decode in Gateway
and Gateway vectors decode in Device.
```

---

# 8. Final Settings wire contract

This section is normative unless the authoritative updated Gateway has a different already-shipped contract. If it differs, update this section and the Device code together before merge.

## 8.1 Requests

All Gateway -> Device Settings requests use:

```text
type = device_command
```

Required envelope:

```text
protocol_version
type
device_id
command
request_id
int_value
bool_value
```

The legacy `int_value/bool_value` fields may remain zero/false for Settings commands if the common v4 decoder still requires them.

## 8.2 `describe_settings`

Request:

```text
type       = device_command
command    = describe_settings
device_id  = gateway routing ID
request_id = non-zero
```

Response stream:

```text
settings_begin
setting_item ...
setting_option_item ...
settings_end
ACK(describe_settings)
```

### `settings_begin`

Required:

```text
protocol_version
type = settings_begin
command = describe_settings
device_id
request_id
total
settings_schema_revision
settings_config_revision
```

### `setting_item`

Required:

```text
protocol_version
type = setting_item
command = describe_settings
device_id
request_id
settings_sequence
settings_id
settings_title
settings_type
settings_flags
settings_option_count
```

Optional by type:

```text
group
unit
max_length
min_value
max_value
step
```

### `setting_option_item`

Required:

```text
protocol_version
type = setting_option_item
command = describe_settings
device_id
request_id
settings_id
settings_sequence      # parent setting index
settings_option_index
settings_title         # option label
```

### `settings_end`

Required:

```text
protocol_version
type = settings_end
command = describe_settings
device_id
request_id
total
settings_schema_revision
settings_config_revision
```

## 8.3 `read_settings`

Response:

```text
settings_values_begin
setting_value ...
settings_values_end
ACK(read_settings)
```

### `settings_values_begin/end`

Carry:

```text
total
settings_config_revision
```

### `setting_value`

Carry:

```text
settings_sequence
settings_id
settings_type
settings_value
```

The value wire type must match the descriptor type.

## 8.4 Transaction BEGIN

Request fields:

```text
transaction_id: uint64
expected_revision: uint32
```

Success ACK:

```text
type=device_ack
command=settings_tx_begin
bool_value=true
int_value=GW_SETTINGS_STATUS_OK
settings_transaction_id=tx_id
settings_config_revision=current_revision
```

Conflict ACK:

```text
bool_value=false
int_value=GW_SETTINGS_STATUS_REVISION_CONFLICT
settings_transaction_id=tx_id
settings_config_revision=current_revision
```

## 8.5 Transaction SET

Request:

```text
transaction_id
settings_id
settings_type
settings_value
```

Device validates:

```text
active transaction
matching tx_id
known id
writable
matching type
range
step
string length
enum index
```

No NVS write occurs during SET.

## 8.6 COMMIT

Success ACK:

```text
bool_value=true
int_value=GW_SETTINGS_STATUS_OK
settings_transaction_id=tx_id
settings_config_revision=new_revision
```

COMMIT ACK is generated only after:

```text
cross-field validation
revision increment
NVS set blob
NVS commit success
active promotion
state -> COMMITTED_WAIT_CONFIRM
```

## 8.7 CONFIRM

Request includes:

```text
transaction_id
settings_config_revision
```

The Device validates both values but does not restart before the CONFIRM ACK has been queued.

---

# 9. MTU and frame-size contract

## 9.1 Logical max is not enough

Current protocol has:

```c
GW_MSG_MAX_LEN = 256
```

But a BLE notification can only carry:

```text
negotiated_mtu - 3
```

Therefore an encoded frame of 250 bytes is invalid on MTU 247 even though it is below `GW_MSG_MAX_LEN`.

## 9.2 V1.1 minimum Settings MTU

Target:

```c
#define GW_SETTINGS_MIN_MTU 247u
#define GW_SETTINGS_TARGET_ATT_PAYLOAD 244u
```

Before starting schema/value streaming:

```c
uint16_t max_payload = gw_ble_max_tx_payload(ble_peripheral_get_mtu());
if (max_payload < GW_SETTINGS_TARGET_ATT_PAYLOAD) {
    return GW_SETTINGS_STATUS_FRAME_TOO_LARGE; /* or MTU unsupported status */
}
```

If the actual Gateway negotiates a different required MTU, align this constant during the Gateway contract gate.

## 9.3 Descriptor encode preflight

The Settings core must stay transport-independent, so `device_settings_freeze()` validates descriptor metadata and callback invariants but does **not** call the BLE/CBOR encoder.

After `device_command_init()` and before BLE advertising, run:

```c
int device_command_preflight_settings_schema(uint16_t target_att_payload);
```

The preflight iterates the frozen registry and uses the specialized Settings encoder to:

- encode each descriptor;
- encode each enum option;
- verify each encoded frame <= target ATT payload;
- fail boot with a clear log if any descriptor cannot be represented.

Do not allow firmware to advertise successfully and discover the problem only when the Gateway opens Settings UI.

---

# 10. Single-owner concurrency model

## 10.1 Existing problem

Settings state currently can be touched by:

```text
device command worker
BLE connection-state callback
confirm timeout callback
```

ESP32-S3 is dual-core. A plain static `s_tx` structure is not safe under concurrent mutation.

## 10.2 Final owner

Only the `device_command` worker calls transaction mutating APIs.

BLE and timers enqueue internal events.

## 10.3 Command queue event type

Extend the existing worker queue item:

```c
typedef enum {
    DEVICE_CMD_EVENT_RX_FRAME = 0,
    DEVICE_CMD_EVENT_SETTINGS_DISCONNECT,
    DEVICE_CMD_EVENT_SETTINGS_TIMEOUT,
} device_cmd_event_type_t;

typedef struct {
    uint8_t type;
    uint16_t len;
    uint32_t generation;
    uint8_t data[GW_MSG_MAX_LEN];
} cmd_rx_msg_t;
```

The queue remains bounded.

No large new Settings object is placed in the queue.

## 10.4 Disconnect bridge

`device_app` BLE state callback:

```text
CONNECTED/SECURING/WAIT_CCCD/READY
   -> ADVERTISING
```

posts:

```c
device_command_post_internal_event(
    DEVICE_CMD_EVENT_SETTINGS_DISCONNECT,
    0);
```

It does **not** call `device_settings_tx_on_disconnect()` directly.

## 10.5 Timer callback

A one-shot Settings timer callback posts:

```text
DEVICE_CMD_EVENT_SETTINGS_TIMEOUT
```

with an operation generation/epoch.

The worker ignores stale timeout events whose generation no longer matches the active timer generation.

## 10.6 No Settings mutex needed

If the single-owner rule is respected, no long-held mutex is needed around NVS commit or callbacks.

Use assertions in debug builds to detect illegal cross-context calls if practical.

---

# 11. Transaction state machine

## 11.1 States

```c
typedef enum {
    DEVICE_SETTINGS_TX_IDLE = 0,
    DEVICE_SETTINGS_TX_ACTIVE,
    DEVICE_SETTINGS_TX_COMMITTED_WAIT_CONFIRM,
    DEVICE_SETTINGS_TX_RESTART_PENDING,
} device_settings_tx_state_t;
```

## 11.2 Bounded runtime state

```c
typedef struct {
    device_settings_tx_state_t state;
    uint64_t transaction_id;
    uint32_t expected_revision;

    uint64_t last_committed_tx_id;
    uint32_t last_committed_revision;

    uint32_t generation;
} device_settings_tx_runtime_t;
```

No history list is required.

## 11.3 BEGIN

Pseudo-flow:

```text
if state == ACTIVE:
    if same tx_id and same expected_revision:
        return OK                 # retry
    else:
        return TX_BUSY

if state != IDLE:
    return TX_BUSY

if expected_revision != current_revision:
    return REVISION_CONFLICT

active -> staging copy
state = ACTIVE
transaction_id = tx_id
expected_revision = expected
new generation
arm absolute ACTIVE timeout
return OK
```

A transaction ID of zero is invalid.

## 11.4 SET

API must include tx ID:

```c
device_settings_result_t device_settings_tx_set(
    uint64_t transaction_id,
    const char *setting_id,
    const device_setting_value_t *value);
```

Validation order:

```text
state ACTIVE
transaction id match
setting exists
not readonly
type match
range/step/length/enum
stage callback
```

SET is assignment-only and performs no NVS operation.

The absolute transaction timeout is **not refreshed by SET**.

## 11.5 COMMIT

```text
state ACTIVE
verify tx_id
validate full staging payload
if current revision == UINT32_MAX -> REVISION_EXHAUSTED
new_revision = current + 1
write new revision into staging header
nvs_set_blob(staging)
nvs_commit()
copy staging -> active
registry revision = new_revision
state = COMMITTED_WAIT_CONFIRM
remember last committed tx/revision
cancel ACTIVE timeout
return OK + new_revision
```

### Duplicate COMMIT

If:

```text
state == COMMITTED_WAIT_CONFIRM or RESTART_PENDING
and tx_id == last_committed_tx_id
```

return the same committed revision without writing NVS again.

## 11.6 ABORT

If ACTIVE and tx ID matches:

```text
active -> staging copy
state = IDLE
cancel timer
clear active tx fields
```

If IDLE, ABORT may be treated as idempotent OK.

Wrong active tx ID returns `TX_ID_MISMATCH`.

## 11.7 ACTIVE timeout

Absolute timeout starts at successful BEGIN.

Default target:

```c
#define DEVICE_SETTING_TX_TIMEOUT_MS 10000u
```

On timeout event while ACTIVE:

```text
abort staging
state -> IDLE
```

No NVS write, no reboot.

## 11.8 Disconnect

### ACTIVE

```text
discard staging
state -> IDLE
```

### COMMITTED_WAIT_CONFIRM

Persistent config has already changed.

```text
state -> RESTART_PENDING
schedule safe restart
```

No rollback.

### IDLE

No Settings action.

---

# 12. COMMIT ACK / CONFIRM / restart sequencing

## 12.1 COMMIT success path

Correct order:

```text
Gateway -> COMMIT

Device worker:
  validate transaction
  validate staging
  persist blob + new revision
  promote active
  core state = COMMITTED_WAIT_CONFIRM
  encode COMMIT ACK
  enqueue COMMIT ACK

  if ACK enqueue succeeds:
      arm confirm timeout
  else:
      state = RESTART_PENDING
      schedule fallback restart
```

The confirm timeout **must not start inside** `device_settings_tx_commit()`.

## 12.2 CONFIRM path

Correct order:

```text
Gateway -> COMMIT_CONFIRM(tx_id, revision)

Device worker:
  validate state
  validate tx_id
  validate revision
  encode CONFIRM ACK
  enqueue CONFIRM ACK

  if ACK enqueue succeeds or fails:
      state = RESTART_PENDING
      cancel confirm timeout
      schedule restart with transport grace
```

The key invariant is that restart scheduling occurs after the ACK enqueue attempt, not before it.

## 12.3 Confirm timeout

Target:

```c
#define DEVICE_SETTINGS_CONFIRM_TIMEOUT_MS 5000u
```

On timeout:

```text
if state == COMMITTED_WAIT_CONFIRM and generation matches:
    state = RESTART_PENDING
    schedule restart
```

## 12.4 Restart callback injection

`device_settings` core never calls `device_app_schedule_restart()`.

The command/orchestration layer owns a callback:

```c
typedef int (*device_cmd_restart_fn)(uint32_t delay_ms);

void device_command_set_restart_fn(device_cmd_restart_fn fn);
```

`device_app_start()` injects:

```c
device_command_set_restart_fn(device_app_schedule_restart);
```

## 12.5 Restart implementation

Do not sleep/block inside the FreeRTOS timer service callback.

Preferred implementation:

```text
device_app_schedule_restart()
    -> set restart pending
    -> create/start one-shot timer

timer callback
    -> notify/release a safe app context OR directly call esp_restart()
       only if no blocking cleanup is needed
```

If product stop/flush requires blocking work, run it from a normal task context, not timer service callback.

---

# 13. Protocol codec architecture

## 13.1 Keep common message lean

The current common `gw_message_t` contains many Settings fields.

V1.1 keeps only the Settings fields needed by common capability discovery:

```c
int has_settings_supported;
bool settings_supported;
gw_settings_schema_revision_t settings_schema_revision;
int has_settings_schema_revision;
```

Remove from common `gw_message_t`:

```text
setting_id
setting_title
setting_group
setting_unit
setting_type
setting_flags
setting_max_length
setting_option_count
setting_option_index
settings_transaction_id
settings_expected_revision
settings_new/config_revision
```

These belong in `gw_settings_command_t` / specialized Settings codec structures.

## 13.2 Raw Settings decoder

Change:

```c
gw_settings_decode_command(const gw_message_t *msg, ...)
```

into:

```c
int gw_settings_decode_command(
    const uint8_t *buf,
    size_t len,
    gw_settings_command_t *out_cmd);
```

The command worker still runs common decode first to obtain envelope routing fields.

Flow:

```text
raw frame
  ↓
gw_message_decode()             # protocol/type/device_id/command/request_id
  ↓
command identifies Settings?
  ├─ no  -> normal command handler
  └─ yes -> gw_settings_decode_command(raw,len,...)
```

Unknown additive Settings keys remain safely skipped by the common decoder.

## 13.3 Specialized Settings ACK encoder

Because transaction ID and config revision are removed from common `gw_message_t`, Settings ACKs must not use the generic `gw_build_ack()` + `gw_message_encode()` path when Settings metadata is required.

Add:

```c
int gw_settings_encode_ack(
    uint8_t *out,
    size_t out_cap,
    const char *device_id,
    const char *command,
    uint32_t request_id,
    bool success,
    gw_settings_status_t status,
    bool has_transaction_id,
    uint64_t transaction_id,
    bool has_config_revision,
    gw_settings_config_revision_t config_revision);
```

Wire shape:

```text
type = device_ack
command = exact Settings request command
device_id = exact routing ID
request_id = exact request ID
bool_value = success
int_value = gw_settings_status_t
optional settings_transaction_id
optional settings_config_revision
```

Normal non-Settings commands continue using the generic ACK builder.

## 13.4 Settings stream encoder API

All Device -> Gateway stream functions accept routing identity explicitly.

Example:

```c
int gw_settings_encode_begin(
    uint8_t *out,
    size_t out_cap,
    const char *device_id,
    uint32_t request_id,
    uint16_t total,
    gw_settings_schema_revision_t schema_revision,
    gw_settings_config_revision_t config_revision);
```

Item:

```c
int gw_settings_encode_item(
    uint8_t *out,
    size_t out_cap,
    const char *device_id,
    uint32_t request_id,
    uint16_t item_index,
    uint16_t total,
    const device_setting_descriptor_t *desc);
```

Values begin:

```c
int gw_settings_encode_values_begin(
    uint8_t *out,
    size_t out_cap,
    const char *device_id,
    uint32_t request_id,
    uint16_t total,
    gw_settings_config_revision_t config_revision);
```

## 13.5 Message type helper

Replace the current helper that forces `type=device_command` with:

```c
static int gw_settings_put_envelope(
    gw_writer_t *w,
    const char *type,
    const char *command,
    const char *device_id,
    uint32_t request_id);
```

Schema item call:

```text
type    = GW_MSG_TYPE_SETTINGS_ITEM
command = GW_COMMAND_DESCRIBE_SETTINGS
```

## 13.6 Required legacy fields

If the updated Gateway's common v4 envelope decoder requires `int_value` and `bool_value` on all frames, continue emitting:

```text
int_value = 0
bool_value = false
```

for non-ACK Settings stream frames.

Do not remove them until cross-repo golden vectors confirm they are no longer required.

---

# 14. Persistence and defaults

## 14.1 Boot load algorithm

```text
open NVS readonly
  ↓
key exists?
  ├─ no:
  │    core initializes header(format_version, revision=0)
  │    product defaults(payload)
  │    validate defaults
  │    active -> staging copy
  │    load result = DEFAULTS_NOT_FOUND
  │
  └─ yes:
       size correct?
       format correct?
       reserved/header valid?
       product validation passes?
          ├─ yes: load active, copy active -> staging
          └─ no: recover defaults, load result = DEFAULTS_RECOVERED
```

Do not leave zero-filled payload as implicit defaults.

## 14.2 Load result

Recommended API:

```c
typedef enum {
    DEVICE_SETTINGS_LOAD_OK = 0,
    DEVICE_SETTINGS_LOAD_DEFAULTS_NOT_FOUND,
    DEVICE_SETTINGS_LOAD_DEFAULTS_RECOVERED,
    DEVICE_SETTINGS_LOAD_FATAL,
} device_settings_load_result_t;

esp_err_t device_settings_load(device_settings_load_result_t *out_result);
```

`DEFAULTS_NOT_FOUND` and `DEFAULTS_RECOVERED` are not necessarily boot-fatal.

## 14.3 Persist defaults policy

V1.1 does not automatically write NVS just because the key is absent.

The first successful Settings commit persists the blob.

This avoids unnecessary flash writes at every factory-reset/default boot.

## 14.4 NVS commit invariant

Exactly one NVS commit per Settings transaction COMMIT.

Forbidden:

```text
NVS write during BEGIN
NVS write during SET
multiple per-field writes
revision write separate from config blob
```

---

# 15. Device app lifecycle

## 15.1 Profile

Use:

```c
typedef struct {
    ...
    int (*register_settings)(void);
    gw_settings_schema_revision_t settings_schema_revision;
    uint16_t settings_format_version;
    size_t settings_config_size;
} device_app_profile_t;
```

## 15.2 Boot order

Final order:

```text
1.  logging
2.  NVS init
3.  device_settings_init (if profile supports settings)
4.  device_settings_configure
5.  product register_settings       # data-only, no hardware dependency
6.  device_settings_freeze/preflight
7.  device_settings_load
8.  product_init                    # may consume committed config
9.  device_feature_init
10. device_command_init
11. inject Settings metadata + restart callback
12. register product commands
13. register features
14. freeze command/features
15. preflight frozen Settings schema against target ATT payload
16. device_event init/register
17. ble_peripheral init
18. create/ensure internal command Settings timer ready
19. product_start
20. BLE start/advertising
```

The Settings timeout infrastructure must be ready before advertising is exposed.

## 15.3 `register_settings()` restriction

Allowed:

```text
bind static active/staging buffers
register static descriptors
register defaults/validation callbacks
```

Forbidden:

```text
GPIO init
start sensor task
create hardware timers
NimBLE calls
NVS direct access
```

---

# 16. Capability advertisement

Settings support is advertised in normal capability discovery.

## 16.1 Supported semantic

`settings_supported=true` means:

```text
Settings subsystem configured
registry freeze succeeded
storage buffers valid
load/default recovery completed
protocol handlers available
```

It does **not** mean merely `setting_count > 0`.

## 16.2 Fields

`capabilities_begin` and preferably `capabilities_end` carry:

```text
settings_supported = true
settings_schema_revision = profile value
```

Old Gateway behavior:

- unknown optional keys must be ignored;
- control/features continue to work.

New Gateway behavior:

- if field absent/false -> Settings unsupported;
- if true -> may issue `describe_settings`.

---

# 17. Factory reset

## 17.1 Generic API

Add:

```c
esp_err_t device_settings_factory_reset(void);
```

Behavior:

```text
reject if committed restart is pending unless product policy says otherwise
abort ACTIVE transaction
remove NVS config key
rebuild active defaults
copy active -> staging
revision = 0
```

## 17.2 Device app policy

`device_app_factory_reset()` first checks:

```text
profile.supports_factory_reset
```

Then:

```text
clear bonds
reset Settings
invoke product-specific reset callback if defined
schedule reboot
```

Add optional profile callback:

```c
int (*product_factory_reset)(void);
```

for product NVS state outside generic Settings.

---

# 18. SECRET policy

## 18.1 V1.1 default

Do not register a writable SECRET setting in production/demo profiles.

The existing `reference_device` `admin_token` setting must either:

1. be removed from registration in v1.1; or
2. be compiled only under an explicit experimental option after the Gateway secret action contract passes integration tests.

## 18.2 Future secret contract

Required before enablement:

```text
READ -> configured boolean only
WRITE -> explicit action:
         KEEP
         SET(value)
         CLEAR
```

Blank string is not CLEAR.

Secret plaintext must never be emitted through:

```text
read_settings
normal ACK
BLE event
WebSocket
Gateway persistent cache
logs
```

---

# 19. Demo device Settings target

## 19.1 New files

```text
devices/demo_device/main/demo_config.h
devices/demo_device/main/demo_config.c
```

Modify:

```text
devices/demo_device/main/demo_product.c
devices/demo_device/main/CMakeLists.txt
devices/demo_device/main/drivers/demo_sensor.h
devices/demo_device/main/drivers/demo_sensor.c
```

## 19.2 Demo config payload

```c
typedef enum {
    DEMO_FAN_MODE_OFF = 0,
    DEMO_FAN_MODE_MANUAL = 1,
    DEMO_FAN_MODE_AUTO = 2,
} demo_fan_mode_t;

typedef struct {
    bool sensor_enabled;
    int32_t sample_interval_s;
    int32_t startup_dryer_temp_c;
    uint8_t startup_fan_mode;
    int32_t startup_fan_percent;
    char device_label[33];
    bool diagnostic_logging;
    char serial_number[17];
} demo_config_payload_t;

typedef struct {
    device_settings_blob_header_t header;
    demo_config_payload_t payload;
} demo_config_t;
```

## 19.3 Demo setting descriptors

| ID | Type | Range/options | Flags |
|---|---|---|---|
| `sensor_enabled` | BOOL | true/false | — |
| `sample_interval_s` | INT | 1..60 step 1 | — |
| `startup_dryer_temp_c` | INT | 30..100 step 1 | — |
| `startup_fan_mode` | ENUM | Off/Manual/Auto | — |
| `startup_fan_percent` | INT | 0..100 step 1 | — |
| `device_label` | STRING | max 32 | — |
| `diagnostic_logging` | BOOL | true/false | ADVANCED |
| `serial_number` | STRING | max 16 | READONLY |

Eight settings remain below `DEVICE_SETTING_MAX_COUNT=12`.

## 19.4 Defaults

Recommended defaults:

```text
sensor_enabled       = true
sample_interval_s    = 3
startup_dryer_temp_c = 60
startup_fan_mode     = AUTO
startup_fan_percent  = 50
device_label         = "Demo Device"
diagnostic_logging   = false
serial_number        = "DEMO-0001"
```

## 19.5 Product initialization apply

After Settings load and when `product_init()` runs:

```text
s_dryer_temperature = startup_dryer_temp_c * 10
```

because the existing runtime feature uses one decimal digit.

Fan initial behavior:

```text
OFF    -> 0%
MANUAL -> startup_fan_percent
AUTO   -> defined demo policy (e.g. 30% initial; future control may adjust)
```

The exact AUTO simulation may be simple, but it must be deterministic.

## 19.6 Sensor interval

Remove the hard-coded `3000 ms` loop period from `demo_sensor.c`.

Recommended interface:

```c
typedef uint32_t (*demo_sensor_interval_fn)(void *ctx);
typedef bool (*demo_sensor_enabled_fn)(void *ctx);

int demo_sensor_start(
    demo_sensor_cb_t sample_cb,
    void *sample_ctx,
    demo_sensor_interval_fn interval_fn,
    demo_sensor_enabled_fn enabled_fn,
    void *config_ctx);
```

The task reads committed active config each iteration.

Since every Settings save reboots, there is no need for live staging application.

## 19.7 Demo cross-field validation

Example:

```text
startup_fan_mode must be 0..2
startup_fan_percent 0..100
sample_interval 1..60
startup_dryer_temp 30..100
```

Avoid artificial constraints that make the demo confusing unless they explicitly test cross-field validation.

A useful cross-field demo rule:

```text
if startup_fan_mode == MANUAL:
    startup_fan_percent must be >= 10
```

This is visible and easy to test.

---

# 20. Reference device migration

## 20.1 CMake

Ensure:

```text
reference_config.c
```

is included in `devices/reference_device/main/CMakeLists.txt`.

Add explicit dependency on `device_settings` if the product directly includes its header.

## 20.2 Active/staging descriptor contexts

Migrate each descriptor from one `.ctx` to:

```text
.read_ctx  = active payload/field
.stage_ctx = staging payload/field
```

## 20.3 Remove SECRET from normal registration

Do not register `admin_token` in the normal v1.1 reference profile until the secret contract gate passes.

It may remain in the config structure temporarily for format compatibility if necessary.

If removing the field changes persisted blob size, either:

- bump `settings_format_version`; or
- retain reserved padding/field layout until migration is implemented.

Do not silently reinterpret a persisted blob with the same format version.

---

# 21. File-by-file implementation map

## 21.1 `components/device_settings/include/device_settings.h`

Change:

- typed callbacks;
- `read_ctx` / `stage_ctx`;
- storage config object;
- Settings status return types or internal status mapping;
- tx SET includes transaction ID;
- split confirm validation from restart;
- factory reset API;
- load result enum;
- remove public loose internal setters.

## 21.2 `device_settings_registry.c`

Change:

- `init()` only initializes state;
- `configure()` binds buffers/config;
- validate descriptors against length/type/flags;
- validate read/stage callback presence;
- freeze performs core metadata/callback validation only;
- wire/ATT encode preflight is performed by `device_command` after Settings registration and before BLE advertising;
- committed revision derives from active header after load;
- no Settings task/timer logic.

## 21.3 `device_settings_transaction.c`

Change:

- remove FreeRTOS timer ownership;
- remove `extern device_app_schedule_restart`;
- enforce tx ID on SET/COMMIT/ABORT/CONFIRM;
- implement duplicate BEGIN/COMMIT semantics;
- detect revision exhaustion;
- return `device_settings_result_t`;
- map core results to wire `gw_settings_status_t` in `device_command`;
- absolute timeout handled by owner event, not internal timer refresh.

## 21.4 `device_settings_persistence.c`

Change:

- defaults callback recovery;
- copy active -> staging after successful load/default recovery;
- validate format/size/header;
- explicit load result;
- factory reset key erase helper;
- exactly one commit per successful transaction.

## 21.5 Remove/retire `device_settings_restart.c`

Preferred:

- delete it after reverse-dependency removal; or
- reduce it to pure state helpers with no device_app dependency.

Restart orchestration belongs to `device_command` + `device_app`.

## 21.6 `components/gateway_protocol/include/gateway_protocol.h`

Change:

- normalize Settings key 43 name to CONFIG_REVISION;
- keep compatibility alias;
- normalize schema revision width;
- keep only capability-level Settings fields in common message;
- no product-specific data.

## 21.7 `gateway_protocol.c`

Change:

- common encoder/decoder handles settings_supported/schema_revision only;
- unknown Settings data remains skipped;
- ensure old/new v4 compatibility tests.

## 21.8 `include/gateway_settings.h`

Change:

- command constants separated from message types;
- shared status enum;
- config/schema revision typedefs;
- raw decoder signature;
- routing identity arguments for response encoder;
- updated begin/end revision arguments.

## 21.9 `gateway_settings.c`

Change:

- emit actual Settings response type;
- include device_id;
- encode config/schema revisions;
- encode option_count;
- raw CBOR decoder;
- specialized Settings ACK encoder carrying status/tx/config revision;
- validate bounded strings;
- check map pair counts exactly;
- no heap allocation.

## 21.10 `components/device_command/device_command.c`

Change:

- internal event kind in worker queue;
- timer owned here or orchestration helper here;
- raw Settings dispatcher;
- remove stub Settings handlers;
- explicit core-result -> wire-status mapping;
- specialized Settings ACK status + transaction/config revision semantics;
- commit ACK enqueue then confirm timer arm;
- confirm ACK enqueue then restart schedule;
- disconnect event serialized through worker;
- frame length <= negotiated ATT payload check;
- preflight frozen Settings schema through the specialized encoder before advertising.

## 21.11 `device_command.h`

Add:

```c
int device_command_post_settings_disconnect(void);
void device_command_set_restart_fn(device_cmd_restart_fn fn);
```

and any test-only fault injection hooks behind build guards.

## 21.12 `components/device_app/device_app.c`

Change boot order per section 15.

BLE state callback posts an event only.

Safe restart implementation must not block timer service callback.

## 21.13 CMake

### `components/device_app/CMakeLists.txt`

Must require:

```text
device_settings
```

if it directly calls Settings APIs.

### `components/device_command/CMakeLists.txt`

Must require:

```text
device_settings
gateway_protocol
ble_peripheral
device_feature
```

### product CMake

Reference:

```text
reference_product.c
reference_config.c
```

Demo:

```text
demo_product.c
demo_config.c
```

Both add direct `device_settings` requirement when including its public header.

---

# 22. Implementation phases

The phase order is fixed.

```text
F0  Build graph + storage binding
F1  Active/staging + typed descriptor ABI + defaults
F2  Single-owner transaction state machine
F3  Gateway wire-contract alignment
F4  Specialized codec + common message RAM cleanup
F5  ACK/confirm/restart sequencing
F6  Command routing + capability advertisement
F7  Factory reset + recovery
F8  reference_device + demo_device integration
F9  Unit/golden/HIL/fault/soak release gate
```

Do not reorder F3/F4 after Gateway integration testing has started without regenerating golden vectors.

---

# 23. Phase F0 — Build graph and storage binding

## Goal

Get the real IDF firmware build using a valid Settings storage binding before changing transaction/protocol behavior.

## Files

```text
components/device_settings/include/device_settings.h
components/device_settings/device_settings_registry.c
components/device_app/device_app.c
components/device_app/CMakeLists.txt
components/device_command/CMakeLists.txt
devices/reference_device/main/CMakeLists.txt
devices/demo_device/main/CMakeLists.txt
```

## Tasks

- [ ] Add `device_settings_configure()` object API.
- [ ] Ensure configure occurs after init.
- [ ] Bind non-NULL active/staging buffers.
- [ ] Validate `config_size >= sizeof(device_settings_blob_header_t)`.
- [ ] Add missing CMake dependencies.
- [ ] Add `reference_config.c` to reference product SRCS.
- [ ] Add `demo_config.c` placeholder/source when F8 work begins.
- [ ] Remove loose public setters from product-facing use.

## Tests

### Host

- configure before init -> invalid state or documented failure;
- init -> configure -> success;
- double configure -> reject;
- NULL active -> reject;
- NULL staging -> reject;
- same pointer for active/staging -> reject;
- too-small blob -> reject.

### Build

```bash
idf.py -C devices/reference_device fullclean build
idf.py -C devices/demo_device fullclean build
```

## Exit gate

Both products compile and Settings load no longer fails because active/staging pointers are NULL.

---

# 24. Phase F1 — Active/staging, descriptor ABI and defaults

## Goal

Guarantee committed reads and staged writes are isolated.

## Tasks

- [ ] Replace `.ctx` with `.read_ctx/.stage_ctx`.
- [ ] Introduce typed read/stage callback union.
- [ ] Migrate all reference descriptors.
- [ ] Migrate all tests.
- [ ] Core owns persisted header.
- [ ] Add product defaults callback.
- [ ] Add explicit invalid-NVS recovery policy.
- [ ] Copy active -> staging after every load/default recovery.
- [ ] Freeze validates descriptor metadata.

## Required tests

### Isolation

```text
active target_temp = 60
BEGIN
SET target_temp = 80
read_settings -> 60
ABORT
read_settings -> 60
```

### Commit

```text
BEGIN
SET target_temp = 80
COMMIT
read_settings -> 80
```

### Defaults

- no NVS key -> product defaults;
- wrong blob size -> product defaults + recovered result;
- wrong format -> product defaults + recovered result;
- corrupt payload fails validation -> product defaults;
- default payload itself invalid -> boot Settings subsystem fails.

## Exit gate

No read path can observe staging before commit.

---

# 25. Phase F2 — Single-owner transaction state machine

## Goal

Make transaction behavior deterministic across BLE commands, disconnects, and timeouts.

## Tasks

- [ ] Add internal command worker event types.
- [ ] BLE callback posts disconnect event only.
- [ ] timer callback posts timeout event only.
- [ ] remove Settings core timer ownership.
- [ ] SET requires tx ID.
- [ ] duplicate BEGIN same tx is deterministic.
- [ ] duplicate COMMIT same tx does not write NVS twice.
- [ ] wrong tx ID maps to `TX_ID_MISMATCH`.
- [ ] active timeout absolute from BEGIN.
- [ ] revision overflow handled.

## Concurrency tests

Inject ordering combinations:

1. SET queued, disconnect queued immediately after.
2. disconnect queued, SET queued after.
3. COMMIT processing while disconnect event arrives.
4. timeout event generated then COMMIT completes with newer generation.
5. stale timeout event after state transition.

Expected behavior must depend only on queue order/generation, never on CPU race timing.

## Exit gate

Thread sanitizer is not available on ESP32, so acceptance is deterministic queue-order unit/fault tests plus 100 HIL disconnect cycles without invalid state/deadlock.

---

# 26. Phase F3 — Gateway wire contract alignment

## Goal

Lock Device protocol to the already-updated Gateway contract.

## Tasks

- [ ] Compare key table.
- [ ] Compare command strings.
- [ ] Compare response message types.
- [ ] Compare status enum.
- [ ] Compare revision widths.
- [ ] Confirm transaction ID `uint64_t`.
- [ ] Confirm Settings type encoding.
- [ ] Confirm max string lengths.
- [ ] Confirm MTU expectation.
- [ ] Export golden vectors.
- [ ] Keep v4 or formally bump only if the Gateway requires it.

## Device protocol decisions if Gateway matches expected v1.1

```text
schema_revision: uint16
config_revision: uint32
transaction_id: uint64
status: int_value
config revision key: 43
no new numeric keys
```

## Exit gate

Cross-repo golden vector test passes in both directions.

---

# 27. Phase F4 — Specialized codec and RAM cleanup

## Goal

Make Settings framing correct and prevent shared-message growth.

## Tasks

- [ ] Raw Settings decoder accepts `buf,len`.
- [ ] common message retains only supported/schema fields.
- [ ] schema response types are real message types.
- [ ] every stream frame includes device_id/request_id.
- [ ] begin/end include revisions.
- [ ] item includes option_count.
- [ ] option includes setting ID.
- [ ] runtime frame length checked against ATT payload.
- [ ] freeze preflight encodes all descriptors/options.
- [ ] no heap allocation in codec.

## RAM measurement

Record:

```c
sizeof(gw_message_t)
sizeof(gw_settings_command_t)
sizeof(cmd_rx_msg_t)
```

before and after.

Acceptance:

- common `gw_message_t` must not grow versus current baseline;
- ideally it shrinks materially after removing Settings strings/tx fields.

## Codec tests

- every message type;
- malformed type;
- missing request_id;
- zero request_id;
- oversized ID/title/group/unit/string;
- truncated CBOR;
- duplicate key behavior defined/rejected;
- unknown additive key skipped;
- frame > ATT target rejected by preflight/runtime check.

## Exit gate

All specialized codec tests and shared Gateway vectors pass.

---

# 28. Phase F5 — ACK, confirm and restart lifecycle

## Goal

Prevent reboot-before-ACK and recover correctly when ACK/confirm is lost.

## Tasks

- [ ] COMMIT core no longer starts confirm timer.
- [ ] COMMIT ACK contains status + config revision.
- [ ] confirm timer armed only after ACK enqueue success.
- [ ] CONFIRM core only validates transaction/revision.
- [ ] CONFIRM ACK enqueue attempted before restart scheduling.
- [ ] timeout/disconnect-after-commit schedules restart.
- [ ] remove reverse dependency to device_app.
- [ ] restart callback injected.
- [ ] no blocking delay in timer service callback.

## Fault tests

### Drop COMMIT ACK

Persist succeeds; Gateway receives no ACK.

Expected:

```text
device eventually restarts
new revision persists
Gateway reconnect/read reconciles outcome
```

### Drop CONFIRM

Expected:

```text
confirm timeout -> restart
new revision persists
```

### Drop CONFIRM ACK

Expected:

```text
restart still happens
Gateway sees reconnect and verifies revision
```

### Power cycle after NVS commit, before confirm

Expected:

```text
new config + new revision
never mixed old/new
```

## Exit gate

Every fault ends in only one of:

```text
old config + old revision
new config + new revision
```

Never mixed.

---

# 29. Phase F6 — Command routing and capability advertisement

## Goal

Remove duplicate/stub routes and expose Settings support cleanly.

## Tasks

- [ ] remove built-in stub handlers for `describe_settings/read_settings` if custom dispatcher owns them;
- [ ] one Settings dispatcher only;
- [ ] use `GW_COMMAND_SETTINGS_*` symbols;
- [ ] capability begin includes `settings_supported/schema_revision`;
- [ ] unsupported product does not register Settings commands/metadata as supported;
- [ ] normal command/feature paths unchanged.

## Tests

- old device profile without Settings -> no support advertisement;
- Settings profile -> supported true;
- schema revision exact;
- old Gateway ignores additive fields;
- normal feature discovery unaffected.

## Exit gate

One route per Settings command and no Settings stub remains reachable.

---

# 30. Phase F7 — Factory reset and recovery

## Goal

Make persistent reset deterministic.

## Tasks

- [ ] implement `device_settings_factory_reset()`;
- [ ] erase only Settings NVS key/namespace intended by design;
- [ ] reset active defaults/revision 0;
- [ ] copy active -> staging;
- [ ] respect profile `supports_factory_reset`;
- [ ] optional product reset callback;
- [ ] reboot after reset.

## Tests

1. Save revision 5.
2. Factory reset.
3. Reboot.
4. `read_settings` returns defaults revision 0.
5. BLE bonds reset according to app policy.

## Exit gate

Factory reset returns the device to documented default Settings state without stale transaction data.

---

# 31. Phase F8 — reference_device and demo_device integration

## Goal

Prove generic product integration with two products.

## Reference checklist

- [ ] CMake includes config source.
- [ ] active/staging contexts separated.
- [ ] format version correct.
- [ ] schema revision correct.
- [ ] SECRET descriptor disabled unless verified.
- [ ] all existing LED feature behavior remains functional.

## Demo checklist

- [ ] `demo_config.h/.c` added.
- [ ] 8 demo settings registered.
- [ ] Settings metadata titles/groups/units render clearly.
- [ ] sensor interval uses config, not hard-coded 3000 ms.
- [ ] sensor_enabled controls sampling behavior.
- [ ] startup dryer temperature applied at boot.
- [ ] startup fan mode/percent applied at boot.
- [ ] diagnostic logging flag applied.
- [ ] serial number readonly.
- [ ] no SECRET in default demo.
- [ ] runtime features remain writable separately from persistent settings.

## Important feature/settings separation

Example:

```text
Feature: dryer_temperature
    runtime control
    can change while device is running

Setting: startup_dryer_temp_c
    persistent startup value
    applied after restart
```

Do not make the persistent setting and live feature share the same staging/active variable unintentionally.

## Exit gate

Gateway can discover/save/reboot/re-read Settings on both products while existing features continue working.

---

# 32. Phase F9 — Release test gate

## Goal

Close all correctness, interoperability, and resource risks.

## 32.1 Host tests

Run all existing Settings suites after migration:

```bash
test/host/run_device_settings_tests.sh
test/host/run_gateway_settings_tests.sh
test/host/run_settings_stream_tests.sh
test/host/run_settings_tx_tests.sh
test/host/run_settings_confirm_tests.sh
test/host/run_fault_injection_tests.sh
test/host/run_reference_config_tests.sh
test/host/run_soak_tests.sh
```

Update tests rather than deleting assertions that expose changed behavior.

## 32.2 IDF builds

```bash
idf.py -C devices/reference_device fullclean build
idf.py -C devices/demo_device fullclean build
```

## 32.3 HIL matrix

### Discovery

- connect/bond/CCCD ready;
- capability advertisement says Settings supported;
- describe schema;
- validate begin/item/options/end order;
- read values;
- revisions consistent.

### Save

- change one BOOL;
- change one INT;
- change one ENUM;
- change one STRING;
- multi-field transaction;
- commit;
- confirm;
- restart;
- reconnect;
- read revision and values.

### Validation

- stale expected revision;
- out-of-range INT;
- invalid step;
- enum index out of range;
- too-long string;
- readonly write;
- cross-field validation failure.

### Fault injection

- disconnect after BEGIN;
- disconnect after SET;
- disconnect during schema stream;
- disconnect during values stream;
- disconnect after NVS commit;
- drop COMMIT ACK;
- drop CONFIRM;
- drop CONFIRM ACK;
- stale timeout event;
- NVS commit fail injection;
- MTU smaller than supported target.

## 32.4 Soak

Minimum:

```text
100 schema discoveries
100 value reads
100 save/reboot/reconnect cycles
100 disconnect-before-commit cycles
```

Record:

```text
free heap before/after
minimum free heap
largest free block
stack high-water marks
NVS revision
reboot count
BLE reconnect success
```

## 32.5 Compatibility

If old Gateway compatibility remains required:

```text
old GW + new device -> control/features still work
new GW + old device -> Settings shown unsupported
new GW + new device -> Settings full path works
```

---

# 33. Detailed unit test matrix

## Registry

- valid BOOL/INT/STRING/ENUM;
- duplicate ID;
- max count;
- register after freeze;
- invalid flags combination;
- enum missing options;
- enum > max options;
- INT min > max;
- INT step 0;
- title/id/group/unit over limit;
- readonly with stage callback policy;
- writable without stage callback reject;
- missing read callback reject;
- protocol-layer encode preflight too large reject.

## Persistence

- not found defaults;
- valid load;
- wrong size;
- wrong format;
- corrupt reserved/header;
- invalid payload validation;
- NVS set failure;
- NVS commit failure;
- one commit per transaction;
- revision persisted in same blob.

## Transaction

- BEGIN correct revision;
- BEGIN stale revision;
- BEGIN tx id zero;
- duplicate BEGIN same tx;
- second BEGIN different tx;
- SET before BEGIN;
- SET wrong tx id;
- type mismatch;
- range;
- step;
- enum;
- string max;
- readonly;
- ABORT;
- COMMIT validation failure;
- COMMIT persistence failure;
- duplicate COMMIT;
- revision overflow;
- timeout;
- disconnect ACTIVE;
- disconnect WAIT_CONFIRM;
- wrong confirm tx;
- wrong confirm revision;
- duplicate confirm.

## Protocol

- frame type exact;
- command exact;
- device ID echo;
- request ID echo;
- settings status exact;
- config revision exact;
- schema revision exact;
- option count exact;
- sequence exact;
- malformed/truncated CBOR;
- unknown additive key;
- ATT payload limit.

---

# 34. Memory and resource policy

ESP32-S3 device has limited internal SRAM and BLE already consumes meaningful memory.

## 34.1 Must remain static/bounded

```text
setting descriptors
active config
staging config
transaction runtime
worker queue depth
codec buffer
string limits
enum option limits
```

## 34.2 No heap-per-setting

Descriptors are:

```c
static const
```

Registry stores pointers only.

## 34.3 No discovery batch allocation

Forbidden:

```c
uint8_t frames[count][GW_MSG_MAX_LEN];
calloc(count, ...);
```

Use one bounded encode buffer and ordered send.

## 34.4 No dedicated Settings task

Do not add a permanent stack solely for Settings.

## 34.5 Common message size

Add a host/build assertion report:

```c
printf("sizeof(gw_message_t)=%zu\n", sizeof(gw_message_t));
```

The v1.1 refactor must not leave large Setting title/group/string buffers in every generic message copied through queues.

---

# 35. Error/logging policy

Use stable Settings status on wire and ESP-IDF error details only in logs.

Good:

```text
W settings: tx begin revision conflict expected=7 current=8
W settings: set target_temp out of range value=150 max=100
E settings: NVS commit failed err=ESP_ERR_NVS_...
```

Do not log secret plaintext.

Do not log full string setting values by default.

Transaction logs may include:

```text
request_id
transaction_id
revision
setting id
status
```

---

# 36. PR decomposition

Recommended PR sequence:

## PR 1 — Core storage/lifecycle correctness

Includes F0/F1.

Do not change wire protocol yet.

Acceptance:

- both products build;
- active/staging tests pass;
- persistence/default tests pass.

## PR 2 — Transaction single-owner hardening

Includes F2.

Acceptance:

- concurrency/fault host tests pass;
- no direct BLE/timer mutation of Settings core.

## PR 3 — Gateway contract + codec

Includes F3/F4.

Acceptance:

- golden vectors pass against actual Gateway;
- common message memory does not grow.

## PR 4 — Commit confirm/restart lifecycle

Includes F5/F6.

Acceptance:

- drop ACK/confirm HIL tests pass.

## PR 5 — Reset and product integration

Includes F7/F8.

Acceptance:

- reference and demo Settings visible and writable from Gateway.

## PR 6 — Soak/release

Includes F9 only plus fixes from testing.

Do not combine broad feature work into this PR.

---

# 37. Per-PR review checklist

For every PR touching Device Settings:

## Protocol

- [ ] no numeric key renumbered;
- [ ] no command/message string changed accidentally;
- [ ] protocol version intentional;
- [ ] Device/Gateway golden vectors updated if wire changed.

## Transaction

- [ ] no NVS in SET;
- [ ] tx ID verified;
- [ ] active/staging isolation preserved;
- [ ] revision increments once;
- [ ] duplicate commit no second write.

## Concurrency

- [ ] no Settings state mutation from NimBLE callback;
- [ ] no Settings state mutation from FreeRTOS timer callback;
- [ ] internal event is bounded;
- [ ] stale timer generation handled.

## Memory

- [ ] no per-setting heap allocation;
- [ ] no batch frame allocation;
- [ ] no unnecessary task;
- [ ] common message not enlarged.

## Security

- [ ] no secret plaintext exposure;
- [ ] readonly enforced device-side;
- [ ] validation performed device-side, not trusted to UI/Gateway only.

---

# 38. Final release checklist

## Build

- [ ] reference fullclean build passes.
- [ ] demo fullclean build passes.
- [ ] no unresolved component dependency.
- [ ] `git diff --check` clean.

## Storage

- [ ] active/staging bound.
- [ ] defaults deterministic.
- [ ] header core-owned.
- [ ] revision persisted atomically.
- [ ] invalid NVS recovery tested.

## Transactions

- [ ] one active transaction.
- [ ] tx ID checked everywhere.
- [ ] stale revision conflict.
- [ ] absolute timeout.
- [ ] disconnect ACTIVE abort.
- [ ] duplicate commit idempotent.
- [ ] revision overflow safe.

## Protocol

- [ ] Gateway contract gate signed off.
- [ ] response message types correct.
- [ ] device_id/request_id on stream frames.
- [ ] status not confused with revision.
- [ ] schema/config revision widths exact.
- [ ] item option_count emitted.
- [ ] MTU/ATT payload check.
- [ ] golden vectors committed.

## Restart

- [ ] commit ACK enqueue before confirm timer.
- [ ] confirm ACK enqueue before restart scheduling.
- [ ] timeout fallback.
- [ ] disconnect-after-commit fallback.
- [ ] no blocking timer-service callback.

## Products

- [ ] reference Settings works.
- [ ] demo Settings works.
- [ ] existing features unchanged.
- [ ] demo sensor interval configurable.
- [ ] startup values applied after reboot.
- [ ] SECRET disabled unless verified.

## Tests

- [ ] all host tests.
- [ ] all HIL basic tests.
- [ ] fault injection.
- [ ] 100-cycle soak.
- [ ] no revision mismatch.
- [ ] no heap/stack regression requiring rollback.

---

# 39. Definition of Done

Device Settings v1.1 is complete only when all of the following are demonstrated on real hardware with the updated Gateway:

```text
Gateway connects to demo device
        ↓
capability discovery reports Settings support
        ↓
Gateway describes Settings schema
        ↓
Gateway reads committed values
        ↓
user changes multiple settings
        ↓
Gateway begins transaction with expected revision
        ↓
Device stages every SET without changing active state/NVS
        ↓
Device validates full staging config
        ↓
Device commits one versioned NVS blob
        ↓
config_revision increments exactly once
        ↓
Device sends COMMIT ACK with status + new revision
        ↓
Gateway sends COMMIT_CONFIRM
        ↓
Device attempts CONFIRM ACK
        ↓
Device restarts safely
        ↓
Gateway reconnects
        ↓
Gateway reads Settings
        ↓
new revision and values match
```

And the failure cases also hold:

```text
disconnect before commit -> old config remains
invalid SET -> no partial persistence
validation failure -> no persistence
NVS failure -> no revision promotion
lost COMMIT ACK -> new config reconciled after reconnect
lost CONFIRM -> timeout restart + new config
stale UI revision -> device returns revision conflict
```

At that point the generic Device Settings subsystem is safe for additional products without adding product IDs or product-specific business logic to the protocol, command, or Settings core components.

---

# 40. Immediate implementation order

For the current Device codebase, execute in this exact order:

```text
1. Fix CMake/build graph.
2. Introduce configure/bind API.
3. Separate read_ctx/stage_ctx and typed callback ABI.
4. Fix defaults/load/recovery.
5. Make device_command worker the sole transaction owner.
6. Fix tx-id/idempotency/timeout/disconnect.
7. Run Gateway contract gate against the actually updated Gateway checkout.
8. Refactor specialized codec + shrink common gw_message_t.
9. Fix stream types/device_id/revisions/option_count.
10. Fix COMMIT ACK -> timer and CONFIRM ACK -> restart sequencing.
11. Remove Settings stubs and finalize capability advertisement.
12. Implement factory reset.
13. Migrate reference_device.
14. Add demo_config and demo settings.
15. Run host tests.
16. Run reference/demo IDF builds.
17. Run cross-repo golden vectors.
18. Run HIL fault injection.
19. Run 100-cycle soak.
20. Merge only after release checklist is complete.
```

This order deliberately fixes local correctness first and freezes the Gateway wire contract before protocol refactoring, minimizing the chance of debugging storage, concurrency, and wire incompatibility at the same time.
