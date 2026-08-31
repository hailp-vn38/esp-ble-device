# ESP BLE Device — LED Matter Template Development Plan

**Repository:** `hailp-vn38/esp-ble-device`  
**Target device:** `devices/reference_device`  
**Hardware:** LED GPIO8 + Button GPIO9  
**Matter semantic target:** `GW_FEATURE_ON_OFF_LIGHT`  
**Gateway protocol target:** v4  
**Related execution phase:** Phase 10 — Light  
**Document version:** v1.1  
**Status:** Reviewed implementation specification  
**Date:** 2026-08-31

---

# 1. Review summary

Bản v1.1 giữ mục tiêu của v1.0 nhưng sửa các điểm có thể làm implementation lệch kiến trúc.

## 1.1 Các thay đổi chính so với v1.0

1. **Phase 10 không tạo lại generic feature framework.**  
   `device_feature`, `register_features()` và protocol-v4 feature serialization phải được hoàn thành ở Phase 06/07. Phase 10 chỉ **consume** framework để migrate reference LED.

2. **Không dùng generic `get_state` làm semantic read binding.**  
   Một physical device có thể có nhiều feature; một command `get_state` không xác định được đang đọc LED, fan hay sensor nào.  
   Protocol v4 phải cung cấp reserved query:

   ```text
   read_feature_state(feature_id, property)
   ```

   `get_state` vẫn giữ để backward compatibility.

3. **Structured `device_event` vẫn phải có `command` discriminator.**  
   Protocol v3 codec hiện bắt buộc `command` không rỗng. Protocol v4 thêm `feature_id` và `property`, nhưng nên giữ một reserved command ổn định:

   ```text
   command = feature_state
   ```

   Semantic source of truth là:

   ```text
   feature_id + property
   ```

   không phải string `command`.

4. **Template config giữ `read_state` callback**, nhưng callback này phục vụ framework-level `read_feature_state`, không bind trực tiếp với legacy `get_state`.

5. **Validation được chia trách nhiệm.**  
   Device framework validate cấu trúc/local registration. Gateway là authoritative validator của mapping giữa advertised tools và feature semantic contract.

6. **On/Off Light chỉ phù hợp nếu LED là user-facing controllable light.**  
   Với `reference_device`, LED GPIO8 được coi là demo light actuator. Production status LED không được tự động expose như Matter Light.

---

# 2. Mục tiêu

Cập nhật `devices/reference_device` để LED hiện tại ngoài các command:

```text
set_led
get_state
```

còn quảng bá semantic feature:

```text
feature_id = led_main
feature_type = GW_FEATURE_ON_OFF_LIGHT
property = GW_PROP_ON_OFF
```

để gateway có thể map thành:

```text
Matter On/Off Light
```

Yêu cầu giữ nguyên:

```text
BLE compatibility
MCP compatibility
Web command compatibility
set_led
get_state
button_pressed
uptime telemetry
```

Matter metadata là additive.

---

# 3. Trạng thái code hiện tại

Source:

```text
devices/reference_device/main/reference_product.c
```

Hardware:

```text
REF_LED_GPIO = GPIO8
REF_BTN_GPIO = GPIO9
```

LED state:

```c
static bool s_led_state = false;
```

Product commands:

```text
set_led
get_state
```

Current `set_led`:

```text
decode BOOL/int
gpio_set_level()
s_led_state = new_state
ACK result
device_event_publish_state("led_state", ...)
```

Current `get_state`:

```text
return s_led_state through ACK int_value
```

Current profile:

```text
.device_type = "light"
.capability_revision = 1
.firmware_version = "0.1.0"
```

Important current framework detail:

```text
device_command_init()
```

already registers a built-in:

```text
get_state
```

and product `device_command_register_capability("get_state", ...)` overrides that entry with the product handler.

This behavior must remain compatible.

---

# 4. Matter semantic type

Current LED supports only:

```text
ON
OFF
```

It does not support:

```text
brightness
PWM
RGB
color temperature
hue/saturation
```

Therefore:

```text
GW_FEATURE_ON_OFF_LIGHT
```

is correct.

Expected gateway mapping:

```text
GW_FEATURE_ON_OFF_LIGHT
    ->
Matter On/Off Light device type
    ->
On/Off server behavior
```

Do not use:

```text
GW_FEATURE_DIMMABLE_LIGHT
```

until real brightness control exists.

---

# 5. Semantic correctness rule

`GW_FEATURE_ON_OFF_LIGHT` means:

```text
a user-facing controllable light function
```

It does **not** mean:

```text
any GPIO connected to any LED
```

Therefore:

```text
reference_device GPIO8 = demo/user-controllable light
```

is acceptable.

Production LEDs used only for:

```text
status
error indicator
Wi-Fi indicator
power indicator
```

must not use `GW_FEATURE_ON_OFF_LIGHT` unless product behavior explicitly intends them to appear as controllable Matter lights.

---

# 6. Prerequisites — mandatory

Phase 10 must not implement foundation architecture.

Before this document is executed, the integration branch must already contain Phase 06/07 changes.

Required:

```text
GW_PROTOCOL_VERSION = 4
components/device_feature/
device_app_profile.register_features
feature registry freeze
feature discovery serialization
structured feature event transport
reserved feature-state read command
```

Required protocol semantic IDs:

```text
GW_FEATURE_ON_OFF_LIGHT
GW_PROP_ON_OFF
```

If any required foundation is missing:

```text
STATUS = BLOCKED
STOP Phase 10
```

Do not recreate framework locally in `reference_device`.

---

# 7. Protocol v4 reserved contracts

Phase 10 consumes the following protocol-v4 contracts.

Exact numeric keys are owned by Phase 06 and must not be invented here.

## 7.1 Structured feature event

Reserved event discriminator:

```text
command = "feature_state"
```

Recommended constant:

```c
#define GW_EVENT_FEATURE_STATE "feature_state"
```

Required semantic fields:

```text
feature_id
property_id
typed value
```

Example:

```text
protocol_version = 4
type             = device_event
command          = feature_state
device_id        = esp32s3-ref
feature_id       = led_main
property_id      = GW_PROP_ON_OFF
bool_value       = true
```

`command` remains a transport discriminator.

Gateway semantic mapping must use:

```text
feature_id + property_id
```

not `command`.

## 7.2 Generic feature-state read

Reserved command:

```text
read_feature_state
```

Recommended constant:

```c
#define GW_COMMAND_READ_FEATURE_STATE "read_feature_state"
```

Request semantic fields:

```text
feature_id
property_id
request_id
```

Example:

```text
type        = device_command
command     = read_feature_state
feature_id  = led_main
property_id = GW_PROP_ON_OFF
request_id  = ...
```

Device feature framework resolves:

```text
led_main
+
GW_PROP_ON_OFF
    ->
registered read callback
```

ACK returns the typed state according to the v4 ACK contract.

This command is framework-level, not product-specific.

---

# 8. Why `get_state` is not the Matter read binding

A future physical device may expose:

```text
led_main
fan_main
temperature_room
humidity_room
```

A single:

```text
get_state
```

cannot identify which feature/property to read.

Therefore the semantic design is:

```text
Matter/gateway state seed:
    read_feature_state(feature_id, property)

Legacy/MCP compatibility:
    get_state
```

For the current reference device:

```text
get_state
```

still returns LED state because that is existing product behavior.

But Matter template must not depend on that legacy assumption.

---

# 9. Target reference-device model

```text
Physical Device: esp32s3-ref
    |
    +-- legacy/product tools
    |     set_led
    |     get_state
    |
    +-- semantic feature
          feature_id = led_main
          type = GW_FEATURE_ON_OFF_LIGHT
          schema = 1

          property:
              GW_PROP_ON_OFF : BOOL

          write binding:
              set_led

          read provider:
              ref_led_read_state()
              exposed through read_feature_state
```

Device remains Matter-independent.

---

# 10. Git workflow

Prerequisites:

```text
Phase 06 merged
Phase 07 merged
feature/matter-integration clean
```

Branch:

```text
matter/phase-10-light
```

Start:

```bash
git checkout feature/matter-integration
git pull --ff-only
git status --short
git checkout -b matter/phase-10-light
```

If working tree is not clean:

```text
STOP
```

Recommended commits:

```text
matter(phase-10): migrate reference LED to on-off light feature
matter(phase-10): add reference LED semantic state tests
```

Final checkpoint:

```text
matter(phase-10): complete reference LED Matter template
```

No direct merge to `main`.

---

# 11. Scope of Phase 10

## Allowed

```text
devices/reference_device/main/reference_product.c
devices/reference_device/main/reference_product.h
devices/reference_device/main/CMakeLists.txt

reference-device tests
light-specific tests
documentation
```

Small generic framework fixes are allowed only if they fix a verified defect found while consuming Phase 07.

Such a fix must be:

```text
minimal
tested
documented as Phase-07 framework defect
```

## Not allowed

Do not implement from scratch in Phase 10:

```text
device_feature registry
protocol v4 keys
register_features lifecycle
feature discovery transport
read_feature_state framework command
generic feature event queue format
```

Those belong to Phase 06/07.

---

# 12. Expected Phase-07 template API

Phase 10 expects a generic light template already available.

Conceptual API:

```c
typedef struct {
    const char *feature_id;

    const char *set_command;

    bool (*read_on_off)(void *context);
    void *context;
} device_feature_on_off_light_config_t;
```

Registration:

```c
int device_feature_register_on_off_light(
    const device_feature_on_off_light_config_t *config);
```

The exact API may differ if Phase 07 implementation chose a generic property-provider model.

Phase 10 must adapt to the already-reviewed framework instead of redesigning it.

---

# 13. Reference LED feature configuration

Stable ID:

```text
led_main
```

Reference callback:

```c
static bool ref_led_read_state(void *context)
{
    (void)context;
    return s_led_state;
}
```

Conceptual feature registration:

```c
static int ref_register_features(void)
{
    const device_feature_on_off_light_config_t config = {
        .feature_id = "led_main",
        .set_command = "set_led",
        .read_on_off = ref_led_read_state,
        .context = NULL,
    };

    return device_feature_register_on_off_light(&config);
}
```

There is intentionally no:

```text
.get_command = "get_state"
```

Matter semantic read uses framework-level `read_feature_state`.

---

# 14. `get_state` compatibility

Keep product `get_state`.

Reasons:

```text
legacy gateway
MCP
Web
existing tests
diagnostics
backward compatibility
```

Current device command registry contains built-in `get_state`, and product registration overrides it with the product capability handler.

Do not remove that behavior in this phase.

`get_state` remains a product tool, not a feature semantic primitive.

---

# 15. Single LED state mutation path

Refactor LED state mutation into one helper.

Recommended:

```c
static int ref_led_apply(bool new_state, bool publish)
{
    esp_err_t err = gpio_set_level(
        REF_LED_GPIO,
        new_state ? 1 : 0);

    if (err != ESP_OK) {
        return -1;
    }

    bool changed = (s_led_state != new_state);
    s_led_state = new_state;

    if (publish && changed) {
        int rc = device_feature_publish_bool(
            "led_main",
            GW_PROP_ON_OFF,
            s_led_state);

        if (rc != 0) {
            ESP_LOGW(TAG, "LED state publish failed: %d", rc);
        }
    }

    return 0;
}
```

Important behavior:

```text
hardware update success
    !=
event delivery success
```

Do not roll back physical LED state only because notification queue is full.

State publish failure should be observable through log/metrics, while future `read_feature_state` can recover the authoritative state.

---

# 16. `set_led` target behavior

Keep existing command name and argument compatibility.

Target flow:

```text
set_led request
    ->
decode desired state
    ->
ref_led_apply()
    ->
ACK current result
```

Conceptual:

```c
static device_cmd_result_t cmd_set_led_handler(
    const gw_message_t *request,
    device_cmd_response_t *response)
{
    bool new_state = request->protocol_version >= 3
                         ? request->bool_value != 0
                         : request->int_value != 0;

    if (ref_led_apply(new_state, true) != 0) {
        response->success = false;
        response->int_value = s_led_state ? 1 : 0;
        return DEVICE_CMD_ERR_HANDLER;
    }

    response->success = true;
    response->int_value = s_led_state ? 1 : 0;
    return DEVICE_CMD_OK;
}
```

Do not remove v2 compatibility unless a separate protocol deprecation decision has been made.

---

# 17. Idempotent writes

Current command is advertised:

```text
DEVICE_CMD_FLAG_IDEMPOTENT
```

Target event policy:

```text
OFF -> ON
    publish ON event

ON -> ON
    ACK success
    no duplicate state event by default
```

Reason:

```text
reported state did not change
```

Gateway Matter path is allowed to update desired/optimistic state on command acceptance and can recover reported state through `read_feature_state`.

---

# 18. Structured publish path

Replace LED semantic state publication:

```c
device_event_publish_state("led_state", value);
```

with:

```c
device_feature_publish_bool(
    "led_main",
    GW_PROP_ON_OFF,
    s_led_state);
```

Expected v4 wire semantic:

```text
type        = device_event
command     = feature_state
feature_id  = led_main
property_id = GW_PROP_ON_OFF
bool_value  = current state
```

No gateway code may infer LED semantic from:

```text
"led_state"
"set_led"
"light"
```

---

# 19. Legacy `"led_state"` event

Before removing:

```text
device_event_publish_state("led_state", ...)
```

search gateway consumers.

Decision rule:

```text
if no required consumer:
    remove legacy LED state event for v4 reference device

if existing UI/API depends on it:
    retain temporarily behind explicit compatibility policy
```

Avoid permanent dual-publish because it causes:

```text
duplicate notifications
extra queue pressure
duplicate state processing risk
```

If dual-publish is temporarily needed, document removal milestone.

---

# 20. Initial/reconnect state synchronization

Correct source of truth:

```text
s_led_state / physical output
```

After feature discovery or BLE reconnect, gateway must call:

```text
read_feature_state(
    feature_id = led_main,
    property = GW_PROP_ON_OFF)
```

Device feature framework calls:

```text
ref_led_read_state()
```

Gateway seeds:

```text
device_state[led_main][GW_PROP_ON_OFF]
```

Do not depend on:

```text
boot default OFF
legacy get_state
an unsolicited event before CCCD is ready
```

---

# 21. Boot state

Current product init:

```text
GPIO8 output
GPIO8 LOW
```

Therefore reboot behavior remains:

```text
LED OFF after reboot
```

Phase 10 does not add persistent light state.

After reconnect:

```text
read_feature_state -> OFF
```

is authoritative.

---

# 22. Feature discovery contract

The generic Phase-07 serializer should emit a compact v4 feature record equivalent to:

```json
{
  "feature_id": "led_main",
  "feature_type": "on_off_light",
  "schema_version": 1,
  "properties": [
    {
      "id": "on_off",
      "type": "bool",
      "writable": true,
      "readable": true
    }
  ],
  "write_bindings": {
    "on_off": "set_led"
  }
}
```

This JSON is conceptual only.

BLE wire remains compact numeric CBOR.

The read path does not need a product-specific command binding because v4 has:

```text
read_feature_state
```

---

# 23. Feature discovery ownership

Current v3 tool discovery is implemented inside:

```text
components/device_command/device_command.c
```

through:

```text
describe_capabilities
send_capabilities()
```

Phase 06/07 must already have integrated feature records into the v4 discovery contract.

Phase 10 must **not** create a second competing discovery transaction just for LED.

The reference LED only registers data into the existing feature registry.

---

# 24. Validation responsibility

## Device-side

Must reject obvious local errors:

```text
NULL config
empty feature_id
feature_id too long
duplicate feature_id
unsupported feature type/schema
missing read provider when template requires readable state
invalid property type
```

If Phase-07 framework exposes command-registry introspection, it may also validate:

```text
set_led exists
set_led value type = BOOL
```

## Gateway-side — authoritative

Gateway must validate the committed snapshot:

```text
feature type supported
schema supported
GW_PROP_ON_OFF exists
property type = BOOL
set binding references advertised set_led tool
set_led argument contract = BOOL
duplicate semantics absent
```

This prevents malformed peripheral metadata from creating invalid Matter endpoints.

Do not add a new command-registry introspection API in Phase 10 solely for validation.

---

# 25. Feature registration failure

For the reference v4 product:

```text
register_features failure
    ->
device_app_start failure
```

Reason:

A firmware that declares protocol-v4 semantic support but fails to register its expected light feature is inconsistent.

Do not silently advertise a partially configured v4 reference product.

---

# 26. Capability revision

Adding `led_main` changes the advertised capability/feature snapshot.

Current:

```text
capability_revision = 1
```

Phase 10 must increment the product revision according to repository policy.

Expected example:

```text
capability_revision = 2
```

Do not reuse revision `1`.

---

# 27. Firmware version

Current:

```text
0.1.0
```

Recommended:

```text
0.2.0
```

or the repository's current semantic-version policy.

Do not change:

```text
hardware_version
```

unless hardware changed.

---

# 28. Button behavior

Current button:

```text
button_pressed
```

It does not toggle LED.

Keep unchanged.

If a later product requirement adds local toggle:

```text
button
    ->
ref_led_apply(!s_led_state, true)
```

must be used so Matter/device_state receives the same semantic state event.

Do not implement button-as-Matter-Switch in this phase.

---

# 29. Heartbeat

Keep:

```text
uptime telemetry
```

unchanged.

Do not bind:

```text
uptime
button_pressed
```

to the On/Off Light feature.

---

# 30. File-level Phase-10 changes

## Expected modifications

```text
devices/reference_device/main/reference_product.c
devices/reference_device/main/CMakeLists.txt
reference-device tests
```

Optional:

```text
reference_product.h
product metadata/version files
```

## Expected existing framework files — consume only

```text
components/device_feature/include/device_feature.h
components/device_feature/include/device_feature_templates.h
components/device_feature/templates/on_off_light.c
components/device_app/include/device_app.h
components/device_app/device_app.c
components/gateway_protocol/*
components/device_event/*
```

If these are missing, report:

```text
BLOCKED: prerequisite Phase 06/07 not merged
```

instead of implementing them ad hoc in Phase 10.

---

# 31. Expected `reference_product.c` structure

```c
static bool s_led_state;

static int ref_led_apply(bool new_state, bool publish);
static bool ref_led_read_state(void *context);

static device_cmd_result_t cmd_set_led_handler(...);
static device_cmd_result_t cmd_get_state_handler(...);

static int ref_register_commands(void);
static int ref_register_features(void);
static int ref_register_events(void);
```

Profile:

```c
.register_commands = ref_register_commands,
.register_features = ref_register_features,
.register_events = ref_register_events,
```

---

# 32. Device remains Matter-independent

Forbidden dependencies:

```text
esp_matter
ConnectedHomeIP / CHIP
Matter endpoint APIs
Matter cluster IDs
Matter fabric state
Matter commissioning
```

Device contract uses only:

```text
feature type
property type
tool binding
state provider
structured event
```

Gateway owns Matter translation.

---

# 33. Unit tests

## 33.1 Feature registration

Expected descriptor:

```text
feature_id = led_main
type = GW_FEATURE_ON_OFF_LIGHT
schema = 1
GW_PROP_ON_OFF BOOL readable+writable
write binding = set_led
```

Reject:

```text
duplicate led_main
empty ID
unsupported schema
missing read provider
```

## 33.2 Command

Test:

```text
set_led(false)
set_led(true)
```

Verify:

```text
physical/GPIO abstraction
s_led_state
ACK
structured state event on actual transition
```

## 33.3 Idempotent

```text
ON -> ON
```

Expected:

```text
ACK success
no duplicate state event
```

## 33.4 Legacy query

```text
get_state
```

Expected:

```text
returns current LED state
does not publish state event
```

## 33.5 Reserved feature read

```text
read_feature_state(
    led_main,
    GW_PROP_ON_OFF)
```

Expected:

```text
framework resolves feature/property
calls ref_led_read_state()
returns typed current state
```

---

# 34. Protocol tests

Required v4 event:

```text
type=device_event
command=feature_state
feature_id=led_main
property=GW_PROP_ON_OFF
bool_value=<state>
```

Verify:

```text
command is non-empty
new numeric keys do not renumber v1-v3 keys
unknown v4 fields are safely ignored by older tolerant decoders where applicable
encoded message <= GW_MSG_MAX_LEN
```

Reserved read request must include:

```text
feature_id
property
request_id
```

---

# 35. Backward compatibility

Required matrix:

```text
v3 gateway <-> v3 device
    unchanged

v4 gateway <-> v3 reference device
    set_led/get_state work
    no semantic feature -> no Matter endpoint

v4 gateway <-> v4 reference device
    tools work
    led_main discovered
    Matter On/Off Light exposed
```

A v3 gateway is not required to understand v4 semantic records unless Phase 06 explicitly defines downgrade behavior.

---

# 36. Hardware tests

## Boot

Expected:

```text
LED OFF
BLE advertising
no boot loop
```

## ON

```text
set_led(true)
```

Expected:

```text
GPIO8 HIGH
ACK success
feature_state event
led_main
GW_PROP_ON_OFF=true
```

## OFF

```text
set_led(false)
```

Expected:

```text
GPIO8 LOW
ACK success
feature_state event false
```

## Idempotent

```text
set_led(false)
set_led(false)
```

Expected:

```text
both ACK success
second write produces no duplicate semantic event
```

## Reconnect

1. Set LED ON.
2. Disconnect gateway.
3. Reconnect.
4. Gateway invokes reserved `read_feature_state`.

Expected:

```text
ON
```

No dependency on legacy `get_state`.

## Reboot

Expected:

```text
LED OFF
read_feature_state -> OFF
```

---

# 37. Matter end-to-end

Requires gateway Phase-10 Light adapter.

## Controller ON

```text
Matter OnOff=true
    ->
gateway semantic adapter
    ->
set_led(true)
    ->
device GPIO8 HIGH
```

## Controller OFF

```text
Matter OnOff=false
    ->
set_led(false)
    ->
GPIO8 LOW
```

## MCP changes same light

```text
MCP set_led(true)
    ->
device feature_state event
    ->
gateway device_state
    ->
Matter subscription update
```

## BLE reconnect

```text
read_feature_state
    ->
reported state recovery
```

## Device offline

Matter gateway must reject/enqueue-fail quickly according to gateway policy and must not block CHIP thread waiting for BLE.

---

# 38. Event delivery failure

If:

```text
GPIO updated
but
device_feature_publish_bool() fails
```

device physical state remains updated.

Do not revert GPIO.

Gateway eventually recovers through:

```text
read_feature_state
reconnect sync
manual refresh
```

Tests should include event queue pressure if framework test infrastructure supports it.

---

# 39. Memory constraints

Phase 10 should add near-zero runtime overhead beyond the already-existing Phase-07 registry entry.

Avoid:

```text
heap allocation per LED transition
dynamic feature ID allocation per event
duplicate copied strings in hot path
Matter data structures on peripheral
```

Feature registration is boot-time bounded metadata.

---

# 40. Acceptance gate

Phase PASS only when:

```text
[ ] Phase 06/07 prerequisites are present

[ ] reference device builds

[ ] set_led backward compatibility passes

[ ] get_state backward compatibility passes

[ ] led_main registered

[ ] type = GW_FEATURE_ON_OFF_LIGHT

[ ] schema = 1

[ ] GW_PROP_ON_OFF = BOOL

[ ] write binding = set_led

[ ] framework read provider resolves through read_feature_state

[ ] semantic event uses command=feature_state

[ ] semantic event carries feature_id + property

[ ] no semantic parsing of "led_state"

[ ] no dependency on profile.device_type for Matter

[ ] duplicate feature registration rejected

[ ] reconnect state recovery uses read_feature_state

[ ] no ESP-Matter/CHIP dependency on device

[ ] capability revision incremented

[ ] unit tests PASS

[ ] protocol tests PASS

[ ] hardware tests PASS

[ ] Matter E2E PASS when gateway adapter is available

[ ] working tree clean

[ ] final checkpoint commit created
```

---

# 41. Required commits

Minimum recommended:

```text
matter(phase-10): migrate reference LED to on-off light feature
matter(phase-10): add reference LED semantic state tests
```

Final checkpoint:

```text
matter(phase-10): complete reference LED Matter template
```

If only one implementation commit is needed, it may also be the final checkpoint provided tests and documentation are included.

---

# 42. Agent handoff

```text
PHASE:
10 — Light / esp-ble-device

STATUS:
PASS / FAIL / BLOCKED

BRANCH:
matter/phase-10-light

COMMIT:
<sha>

PREREQUISITES:
phase-06: PASS
phase-07: PASS

PROTOCOL:
v4

FEATURE:
led_main

TYPE:
GW_FEATURE_ON_OFF_LIGHT

PROPERTY:
GW_PROP_ON_OFF

WRITE TOOL:
set_led

LEGACY TOOL:
get_state

SEMANTIC READ:
read_feature_state(led_main, GW_PROP_ON_OFF)

EVENT:
feature_state

BUILD:
...

UNIT TESTS:
...

PROTOCOL TESTS:
...

HARDWARE TEST:
...

GATEWAY E2E:
...

BACKWARD COMPATIBILITY:
...

KNOWN ISSUES:
...

NEXT STEP ALLOWED:
YES / NO
```

---

# 43. Out of scope

Do not add:

```text
PWM brightness
RGB
color temperature
persistent light state
button as Matter Switch
generic protocol redesign
new BLE service UUID
Matter SDK on device
endpoint/cluster logic on device
```

---

# 44. Future Dimmable Light migration

When hardware has actual brightness control:

```text
GW_FEATURE_ON_OFF_LIGHT
    ->
GW_FEATURE_DIMMABLE_LIGHT
```

Additional property:

```text
GW_PROP_LEVEL
```

Canonical gateway/device range:

```text
0..100
```

A future template may bind:

```text
set_brightness
```

and provide local read callback for level.

This is a separate migration because changing Matter device type/endpoint behavior can affect persisted bridge topology.

---

# 45. Final target

After Phase 10:

```text
esp32s3-ref
    |
    +-- legacy/product interface
    |     set_led
    |     get_state
    |
    +-- semantic feature
          led_main
          GW_FEATURE_ON_OFF_LIGHT
          schema 1
          |
          +-- GW_PROP_ON_OFF
                writable -> set_led
                readable -> read callback
                            exposed via read_feature_state
```

State update:

```text
local/product LED mutation
    ->
device_feature_publish_bool
    ->
device_event
      command=feature_state
      feature_id=led_main
      property=GW_PROP_ON_OFF
    ->
gateway
```

The peripheral remains independent of Matter.

The gateway converts the semantic feature into the actual Matter bridged endpoint/device type/cluster model.
