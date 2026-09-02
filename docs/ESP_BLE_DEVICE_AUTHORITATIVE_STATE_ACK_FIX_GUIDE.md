# ESP BLE Device — Authoritative Feature State ACK Fix Guide

**Repository:** `hailp-vn38/esp-ble-device`  
**Branch:** `main`  
**Reviewed commit:** `8f353cb793134374207c2cec5237a0e4ad4413d2`  
**Target:** ESP32-S3 BLE peripheral  
**Scope:** Chỉ thay đổi `esp-ble-device`. Không bao gồm gateway/Web UI implementation.

---

# 1. Mục tiêu

Fix trường hợp gateway/MCP gửi command như:

```text
set_led(true)
```

device ACK:

```text
result=ok
```

nhưng gateway không nhận được authoritative feature state trong trường hợp state thực tế không thay đổi.

Target contract:

```text
Command success
    |
    v
Device ACK
    |
    +--> success/failure
    |
    +--> authoritative feature state
          feature_id
          property_id
          feature value
```

Device-side command phải cho gateway biết state cuối cùng bất kể command có làm thay đổi physical state hay không.

---

# 2. Root cause hiện tại

File:

```text
devices/reference_device/main/reference_product.c
```

Logic hiện tại:

```c
static int ref_led_apply(bool new_state, bool publish)
{
    esp_err_t err = gpio_set_level(REF_LED_GPIO, new_state ? 1 : 0);
    if (err != ESP_OK) return -1;

    bool changed = s_led_state != new_state;
    s_led_state = new_state;

    if (publish && changed) {
        device_feature_publish_bool(
            "led_main",
            GW_PROP_ON_OFF,
            s_led_state
        );
    }

    return 0;
}
```

Điều này có nghĩa:

```text
LED=false
set_led(true)
 -> changed=true
 -> spontaneous feature_state event

LED=true
set_led(true)
 -> changed=false
 -> không có feature_state event
 -> command vẫn ACK success
```

Đây là behavior hợp lệ cho event delta, nhưng ACK hiện chưa luôn mang semantic state.

---

# 3. Không nên sửa bằng cách publish event mọi lần

Có thể sửa nhanh:

```c
if (publish) {
    device_feature_publish_bool(...);
}
```

nhưng không phải thiết kế khuyến nghị.

Lý do:

- idempotent command sẽ tạo event thừa;
- duplicate BLE traffic;
- event queue tăng tải;
- command response và spontaneous event vẫn bị trộn semantic;
- mọi product handler sau này vẫn dễ lặp lại lỗi.

Thiết kế tốt hơn:

```text
Command path
    -> ACK luôn trả authoritative state

Spontaneous/local path
    -> feature_state event khi state thay đổi
```

---

# 4. Protocol hiện tại đã hỗ trợ structured feature state trong ACK

Header:

```text
components/device_command/include/device_command.h
```

`device_cmd_response_t` hiện đã có:

```c
typedef struct {
    bool success;
    int int_value;
    bool long_running;

    bool has_feature_value_bool;
    bool feature_value_bool;
    uint8_t feature_property_id;
    char feature_id[GW_FEATURE_ID_LEN];
} device_cmd_response_t;
```

Do đó **không cần tạo message type mới** cho LED boolean.

ACK sender:

```text
components/device_command/device_command.c
```

đã có:

```c
if (response->has_feature_value_bool) {
    strlcpy(
        ack.feature_id,
        response->feature_id,
        sizeof(ack.feature_id)
    );

    ack.has_feature_id = 1;
    ack.property_id = response->feature_property_id;
    ack.has_property_id = 1;

    ack.feature_value_bool = response->feature_value_bool;
    ack.has_feature_value_bool = 1;
}
```

Vì vậy fix tối thiểu cho LED chỉ cần đảm bảo product command handler populate đúng các field này.

---

# 5. P0 — Sửa `cmd_set_led_handler()`

File:

```text
devices/reference_device/main/reference_product.c
```

## Hiện tại

```c
response->success = true;
response->int_value = new_state ? 1 : 0;

return DEVICE_CMD_OK;
```

## Target

```c
response->success = true;
response->int_value = s_led_state ? 1 : 0;

response->has_feature_value_bool = true;
response->feature_value_bool = s_led_state;
response->feature_property_id = GW_PROP_ON_OFF;

strlcpy(
    response->feature_id,
    "led_main",
    sizeof(response->feature_id)
);

return DEVICE_CMD_OK;
```

## Quan trọng

Dùng:

```c
s_led_state
```

không dùng trực tiếp:

```c
new_state
```

vì ACK phải trả **actual state after apply**.

Nếu sau này hardware driver normalize/clamp/reject state thì gateway vẫn nhận state thực.

---

# 6. Full suggested handler

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

    ESP_LOGI(
        TAG,
        "LED -> %s",
        s_led_state ? "ON" : "OFF"
    );

    response->success = true;
    response->int_value = s_led_state ? 1 : 0;

    response->has_feature_value_bool = true;
    response->feature_value_bool = s_led_state;
    response->feature_property_id = GW_PROP_ON_OFF;

    strlcpy(
        response->feature_id,
        "led_main",
        sizeof(response->feature_id)
    );

    return DEVICE_CMD_OK;
}
```

---

# 7. Expected ACK semantics

Sau fix, `set_led(true)` phải tạo ACK semantic tương đương:

```text
type=device_ack
device_id=<gateway routing id>
command=set_led
request_id=<same request id>

bool_value=true
    # command success

int_value=1
    # legacy/result compatibility

feature_id=led_main
has_feature_id=true

property_id=GW_PROP_ON_OFF
has_property_id=true

feature_value_bool=true
has_feature_value_bool=true
```

---

# 8. Phân biệt hai boolean

Đây là rule bắt buộc.

## `bool_value`

ACK command result:

```text
true = command accepted/succeeded
false = command rejected/failed
```

## `feature_value_bool`

Runtime feature value:

```text
true = LED ON
false = LED OFF
```

Không được dùng:

```c
ack.bool_value
```

làm LED state.

Ví dụ LED OFF nhưng command thành công:

```text
bool_value=true
feature_value_bool=false
```

Đây là trạng thái hoàn toàn hợp lệ.

---

# 9. Giữ spontaneous event change-only

Không cần sửa:

```c
bool changed = s_led_state != new_state;

if (publish && changed) {
    device_feature_publish_bool(...);
}
```

Behavior mong muốn:

## Command thay đổi state

```text
OFF -> ON

ACK:
    authoritative ON

Event:
    feature_state ON
```

Có thể có duplicate semantic information, nhưng gateway có thể xử lý.

## Idempotent command

```text
ON -> ON

ACK:
    authoritative ON

Event:
    không cần
```

## Local physical change

Ví dụ button/hardware làm:

```text
ON -> OFF
```

không có command ACK.

Phải có:

```text
feature_state OFF
```

---

# 10. Không thay đổi capability revision

Không bump:

```c
.capability_revision = 2
```

chỉ vì ACK bây giờ populate state field.

Lý do:

- command capability schema không đổi;
- feature schema không đổi;
- runtime response enrichment sử dụng field protocol đã tồn tại.

Chỉ bump capability revision nếu:

```text
tool metadata đổi
feature metadata đổi
property semantics đổi
public capability structure đổi
```

---

# 11. P1 — Cập nhật ACK contract comment

File:

```text
components/device_command/include/device_command.h
```

Current documentation nên được update để tránh developer hiểu:

```text
int_value = state
```

là authoritative mechanism duy nhất.

Suggested comment:

```c
/*
 * ACK contract:
 *   type = device_ack
 *   request_id = exact echo
 *   command = exact echo
 *   device_id = exact request->device_id
 *
 *   bool_value:
 *       command success/failure
 *
 *   int_value:
 *       legacy/general result value
 *
 *   structured feature state, when command mutates a semantic feature:
 *       feature_id
 *       property_id
 *       feature_value_bool / feature_value_int
 *
 * Writable semantic feature handlers SHOULD include the actual
 * post-command feature state in the ACK.
 */
```

---

# 12. P1 — Tạo helper để tránh product handler quên populate ACK

Hiện tại mỗi product phải tự viết:

```c
response->has_feature_value_bool = true;
response->feature_value_bool = value;
response->feature_property_id = property_id;
strlcpy(response->feature_id, feature_id, ...);
```

Dễ quên.

Khuyến nghị thêm helper generic.

File:

```text
components/device_command/include/device_command.h
components/device_command/device_command.c
```

## API đề xuất

```c
int device_command_response_set_feature_bool(
    device_cmd_response_t *response,
    const char *feature_id,
    uint8_t property_id,
    bool value
);
```

Implementation:

```c
int device_command_response_set_feature_bool(
    device_cmd_response_t *response,
    const char *feature_id,
    uint8_t property_id,
    bool value)
{
    if (response == NULL ||
        feature_id == NULL ||
        feature_id[0] == '\0' ||
        strnlen(feature_id, GW_FEATURE_ID_LEN) >= GW_FEATURE_ID_LEN) {
        return -1;
    }

    response->has_feature_value_bool = true;
    response->feature_value_bool = value;
    response->feature_property_id = property_id;

    strlcpy(
        response->feature_id,
        feature_id,
        sizeof(response->feature_id)
    );

    return 0;
}
```

Product code sau đó:

```c
response->success = true;
response->int_value = s_led_state ? 1 : 0;

device_command_response_set_feature_bool(
    response,
    "led_main",
    GW_PROP_ON_OFF,
    s_led_state
);
```

Ưu điểm:

- giảm duplicated code;
- giảm lỗi field mismatch;
- dễ mở rộng future devices;
- product module sạch hơn.

---

# 13. P1 — Chuẩn bị INT feature support

Hiện `device_cmd_response_t` chỉ có structured bool response.

Nếu gateway/device roadmap có:

```text
brightness
temperature setpoint
fan level
position
volume
```

nên bổ sung structured INT ACK ngay trong cùng architecture.

Suggested struct:

```c
typedef struct {
    bool success;
    int int_value;
    bool long_running;

    bool has_feature_value_bool;
    bool feature_value_bool;

    bool has_feature_value_int;
    int32_t feature_value_int;

    uint8_t feature_property_id;
    char feature_id[GW_FEATURE_ID_LEN];
} device_cmd_response_t;
```

`send_ack()`:

```c
if (response->has_feature_value_bool ||
    response->has_feature_value_int) {

    strlcpy(
        ack.feature_id,
        response->feature_id,
        sizeof(ack.feature_id)
    );

    ack.has_feature_id = 1;
    ack.property_id = response->feature_property_id;
    ack.has_property_id = 1;
}

if (response->has_feature_value_bool) {
    ack.feature_value_bool = response->feature_value_bool;
    ack.has_feature_value_bool = 1;
}

if (response->has_feature_value_int) {
    ack.feature_value_int = response->feature_value_int;
    ack.has_feature_value_int = 1;
}
```

Helper:

```c
int device_command_response_set_feature_int(
    device_cmd_response_t *response,
    const char *feature_id,
    uint8_t property_id,
    int32_t value
);
```

## Priority

Đối với bug LED hiện tại:

```text
BOOL support = P0
INT support = P1 / future-proofing
```

Không cần block LED fix để chờ INT implementation.

---

# 14. Error behavior

Nếu hardware apply fail:

```c
if (ref_led_apply(new_state, true) != 0)
```

khuyến nghị:

```c
response->success = false;
response->int_value = s_led_state ? 1 : 0;
return DEVICE_CMD_ERR_HANDLER;
```

Không set:

```c
has_feature_value_bool=true
```

trừ khi protocol contract xác định rõ failed ACK vẫn chứa authoritative current state.

## Khuyến nghị hiện tại

Giữ đơn giản:

```text
success=false
structured feature state absent
```

Gateway không mutate cache từ failed command.

Nếu sau này muốn failed ACK cũng report state, cần document riêng:

```text
state_valid_even_on_failure
```

Không implicit.

---

# 15. Long-running command rule

Nếu command:

```c
response.long_running = true;
```

worker hiện gửi ACK accepted sớm.

Trong trường hợp này không được báo final feature state ngay nếu state chưa apply.

Target:

```text
initial ACK
    -> accepted only

operation completes later
    -> spontaneous feature_state event
hoặc completion protocol riêng
```

Rule:

> Chỉ attach authoritative feature state vào ACK nếu state đó đã thực sự được apply trước khi ACK được gửi.

---

# 16. Device-side architecture rule

Mọi writable semantic feature handler nên theo pattern:

```text
validate
    |
    v
apply physical/runtime state
    |
    v
read actual state
    |
    v
response.success = true
    |
    v
attach structured feature state
    |
    v
return DEVICE_CMD_OK
```

Không theo pattern:

```text
copy requested value
    |
    v
ACK it as state
    |
    v
apply hardware later
```

trừ khi command được định nghĩa long-running.

---

# 17. Product-level helper pattern

Đối với future features:

```c
static device_cmd_result_t cmd_set_x_handler(
    const gw_message_t *request,
    device_cmd_response_t *response)
{
    /* 1. validate / normalize requested value */

    /* 2. apply hardware */

    /* 3. read actual value */

    response->success = true;

    /* 4. populate authoritative semantic state */

    return DEVICE_CMD_OK;
}
```

Examples:

## Boolean

```c
device_command_response_set_feature_bool(
    response,
    "relay_main",
    GW_PROP_ON_OFF,
    actual_state
);
```

## Integer

```c
device_command_response_set_feature_int(
    response,
    "light_main",
    GW_PROP_LEVEL,
    actual_level
);
```

---

# 18. Tests cần thêm

## T01 — OFF → ON

Initial:

```text
s_led_state=false
```

Command:

```text
set_led(true)
```

Assert decoded ACK:

```text
success=true
feature_id=led_main
property_id=GW_PROP_ON_OFF
has_feature_value_bool=true
feature_value_bool=true
```

---

# 19. T02 — ON → OFF

Initial:

```text
s_led_state=true
```

Command:

```text
set_led(false)
```

Assert:

```text
success=true
feature_value_bool=false
```

Đặc biệt verify:

```text
ACK bool_value=true
feature_value_bool=false
```

để bắt lỗi nhầm hai boolean.

---

# 20. T03 — Idempotent ON → ON

Initial:

```text
s_led_state=true
```

Command:

```text
set_led(true)
```

Expected:

```text
command success
structured ACK state=true
```

Spontaneous `feature_state` event không bắt buộc.

Đây là test quan trọng nhất cho bug hiện tại.

---

# 21. T04 — Idempotent OFF → OFF

Expected:

```text
ACK bool_value=true
ACK feature_value_bool=false
```

Test này phải fail nếu code vô tình lấy `bool_value` làm feature state.

---

# 22. T05 — Hardware apply failure

Inject/stub:

```text
gpio_set_level -> error
```

Expected:

```text
success=false
no authoritative feature state flag
```

---

# 23. T06 — ACK CBOR roundtrip

Build response:

```text
success=true
feature_id=led_main
property_id=1
feature_value_bool=true
```

Encode:

```text
send_ack / gw_message_encode
```

Decode lại và assert tất cả:

```text
has_feature_id
feature_id
has_property_id
property_id
has_feature_value_bool
feature_value_bool
```

---

# 24. T07 — Success OFF state semantic separation

Input:

```text
successful command produces actual OFF
```

Expected decoded ACK:

```text
bool_value == true
feature_value_bool == false
```

Đây là regression test bắt buộc.

---

# 25. T08 — Change-only event vẫn hoạt động

OFF → ON:

```text
ref_led_apply(true, true)
```

Expected:

```text
one feature_state event
```

ON → ON:

```text
ref_led_apply(true, true)
```

Expected:

```text
no additional spontaneous feature_state event
```

ACK correctness được test độc lập.

---

# 26. T09 — Local state change

Nếu product có local action thay LED:

```text
button / physical input / autonomous logic
```

Expected:

```text
feature_state event emitted
```

vì local change không có command ACK.

---

# 27. T10 — Repeated MCP-equivalent commands

Device-level simulation:

```text
set_led(true)
set_led(true)
set_led(false)
set_led(false)
```

Expected ACK states:

```text
true
true
false
false
```

Không phụ thuộc event count.

---

# 28. Test files dự kiến

Review/update:

```text
test/host/test_device_protocol.c
test/host/test_gateway_protocol.c
test/host/test_device_feature.c
test/host/test_gateway_interop.c
```

Nếu reference product handler hiện khó unit-test trực tiếp, nên thêm product-specific host test hoặc extract LED state command logic sang testable function.

---

# 29. Hardware test cần update

File hiện có:

```text
test/hardware/test_hw_dev.py
```

Thêm case:

```text
1. set LED ON
2. set LED ON lần nữa
3. decode ACK lần 2
4. verify feature state vẫn ON
```

Sau đó:

```text
5. set LED OFF
6. set LED OFF lần nữa
7. verify ACK structured state OFF
```

Hardware test phải phân biệt:

```text
ACK command success
```

và:

```text
ACK semantic feature value
```

---

# 30. Logging đề xuất

Không cần INFO log cho từng structured field trong production.

Có thể dùng DEBUG:

```c
ESP_LOGD(
    TAG,
    "[STATE_ACK] feature=%s prop=%u value=%s",
    response->feature_id,
    response->feature_property_id,
    response->feature_value_bool ? "true" : "false"
);
```

INFO giữ:

```text
LED -> ON
LED -> OFF
```

---

# 31. Memory requirements

Fix này không cần:

- heap allocation mới;
- FreeRTOS task mới;
- queue mới;
- timer mới;
- cJSON;
- dynamic feature-state object.

Structured state nằm trong:

```c
device_cmd_response_t
```

trên command worker stack hiện tại.

ACK sử dụng:

```c
gw_message_t
uint8_t buf[GW_MSG_MAX_LEN]
```

đã tồn tại.

Memory impact rất nhỏ.

---

# 32. BLE traffic impact

## Before

Changed command:

```text
ACK + feature event
```

Idempotent command:

```text
ACK only
```

## After recommended fix

Changed command:

```text
structured ACK + feature event
```

Idempotent command:

```text
structured ACK only
```

Không tăng number of BLE notifications cho idempotent command.

Chỉ tăng vài CBOR fields trong ACK payload.

---

# 33. Compatibility

Legacy gateway:

```text
ignores unknown optional structured ACK fields
continues using ACK success/int_value
```

New gateway:

```text
uses structured feature state
```

Do đó đây là backward-compatible protocol enrichment nếu decoder hiện đã hỗ trợ các field này.

---

# 34. Không cần sửa `device_feature_publish_bool()`

File:

```text
components/device_feature/feature_registry.c
```

Hàm:

```c
device_feature_publish_bool()
```

đang làm đúng nhiệm vụ:

```text
semantic feature -> spontaneous device_event
```

Không biến function này thành ACK helper.

Giữ separation:

```text
device_feature_publish_bool()
    = asynchronous state event

device_command_response_set_feature_bool()
    = synchronous command response state
```

---

# 35. Không cần gọi `get_state` sau `set_led`

Không sửa device theo flow:

```text
set_led
 -> ACK
 -> gateway phải tự gửi get_state
```

Vì:

- thêm BLE round-trip;
- tăng latency;
- tăng dispatcher contention;
- không cần thiết khi command handler đã biết actual state;
- khó scale với nhiều feature.

Authoritative value nên nằm ngay trong ACK của write command.

---

# 36. Không cần thay đổi `get_state` cho bug P0

`get_state` hiện có thể giữ:

```c
response->success = true;
response->int_value = s_led_state ? 1 : 0;
```

Tuy nhiên P1 nên cân nhắc cho nó trả structured semantic state giống:

```c
device_command_response_set_feature_bool(
    response,
    "led_main",
    GW_PROP_ON_OFF,
    s_led_state
);
```

Ưu điểm:

```text
mọi state-reading command trả cùng semantic contract
```

Khuyến nghị làm nếu không ảnh hưởng compatibility tests.

---

# 37. Suggested `get_state` target

```c
static device_cmd_result_t cmd_get_state_handler(
    const gw_message_t *request,
    device_cmd_response_t *response)
{
    (void)request;

    response->success = true;
    response->int_value = s_led_state ? 1 : 0;

    device_command_response_set_feature_bool(
        response,
        "led_main",
        GW_PROP_ON_OFF,
        s_led_state
    );

    return DEVICE_CMD_OK;
}
```

Priority:

```text
set_led structured state = P0
get_state structured state = P1
```

---

# 38. Recommended implementation phases

## Phase D01 — Minimal bug fix

Files:

```text
devices/reference_device/main/reference_product.c
```

Tasks:

- [ ] `set_led` ACK includes `feature_id`.
- [ ] includes property ID.
- [ ] includes `feature_value_bool`.
- [ ] uses actual `s_led_state`.
- [ ] no capability revision bump.

Gate:

```text
ON->ON returns state=true
OFF->OFF returns state=false
```

---

## Phase D02 — Regression tests

Files:

```text
test/host/*
test/hardware/test_hw_dev.py
```

Tasks:

- [ ] OFF→ON.
- [ ] ON→OFF.
- [ ] ON→ON.
- [ ] OFF→OFF.
- [ ] success=true + feature=false distinction.
- [ ] CBOR roundtrip.

---

## Phase D03 — Generic response helpers

Files:

```text
components/device_command/include/device_command.h
components/device_command/device_command.c
```

Tasks:

- [ ] add `device_command_response_set_feature_bool()`.
- [ ] migrate reference product.
- [ ] add helper unit tests.

---

## Phase D04 — INT future-proofing

Optional for current LED bug.

Tasks:

- [ ] `has_feature_value_int`.
- [ ] `feature_value_int`.
- [ ] ACK serialization.
- [ ] helper.
- [ ] tests.

---

# 39. Suggested commit sequence

Minimal:

```text
fix(device): include authoritative LED state in set_led ack
```

Then:

```text
test(device): cover idempotent feature command ack state
```

Optional cleanup:

```text
refactor(device): add semantic feature response helpers
```

Future:

```text
feat(protocol): support integer feature state in command ack
```

---

# 40. Definition of Done

## P0 behavior

- [ ] `set_led(true)` when OFF returns semantic ON state.
- [ ] `set_led(true)` when already ON still returns semantic ON state.
- [ ] `set_led(false)` when ON returns semantic OFF state.
- [ ] `set_led(false)` when already OFF still returns semantic OFF state.

## Protocol correctness

- [ ] ACK success remains in `bool_value`.
- [ ] Feature runtime value is in `feature_value_bool`.
- [ ] `feature_id=led_main`.
- [ ] `property_id=GW_PROP_ON_OFF`.
- [ ] request ID exact echo.
- [ ] routing device ID exact echo.

## Event behavior

- [ ] Real state transition can still emit spontaneous `feature_state`.
- [ ] Idempotent command does not need spontaneous event.
- [ ] Local change still has event path.

## Reliability

- [ ] No additional task.
- [ ] No additional queue.
- [ ] No dynamic allocation in command handler.
- [ ] ACK CBOR remains within `GW_MSG_MAX_LEN`.

## Tests

- [ ] Host tests pass.
- [ ] Protocol roundtrip tests pass.
- [ ] Hardware repeated-command test passes.
- [ ] No capability discovery regression.

---

# 41. Final device-side contract

Sau fix, `esp-ble-device` chịu trách nhiệm:

```text
WRITE COMMAND
    |
    v
apply actual state
    |
    v
ACK
    |
    +--> command success
    |
    +--> actual semantic feature state
```

và độc lập:

```text
LOCAL / SPONTANEOUS CHANGE
    |
    v
feature_state event
```

Rule cuối:

> Một writable semantic feature command thành công phải trả actual post-command feature state trong ACK. Device event chỉ cần mô tả asynchronous/spontaneous state change; không được là cơ chế duy nhất giúp gateway biết kết quả runtime state của một command.
