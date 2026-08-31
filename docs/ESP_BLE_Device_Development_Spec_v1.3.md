# ESP-BLE Device Development Specification

**Version:** 1.3  
**Date:** 2026-08-27 (ISO 8601)  
**Status:** IMPLEMENTATION-FROZEN  
**Repository:** `hailp-vn38/esp-ble-device`  
**Verified baseline:** `70cfa2d02376a3a52408e294a4186e97c7a22373` (commit hash for v1.3 implementation start)  
**Companion specification:** `docs/ESP_BLE_Gateway_Development_Spec_v1.3.md` (same repository)  
**Supersedes for Device scope:** `ESP_GATT_Capability_Cache_Message_Trace_Interop_Development_Spec_v1.2.md`

---

# Glossary

| Term | Definition |
|------|------------|
| **BLE** | Bluetooth Low Energy - giao thức wireless ngắn hạn năng lượng thấp |
| **GATT** | Generic Attribute Profile - cấu trúc data trong BLE, định nghĩa services và characteristics |
| **CCCD** | Client Characteristic Configuration Descriptor (UUID: 0x2902) - descriptor cho phép client subscribe notifications |
| **CBOR** | Concise Binary Object Representation - định dạng binary encoding siêu tiết kiệm, dùng trong protocol messages |
| **MTU** | Maximum Transmission Unit - kích thước packet tối đa có thể gửi qua BLE connection |
| **NVS** | Non-Volatile Storage - hệ thống lưu trữ persist data trên ESP32 |
| **NimBLE** | BLE stack open-source được dùng trong ESP-IDF |
| **UUID** | Universally Unique Identifier - định danh duy nhất cho services/characteristics |
| **ACK** | Acknowledgement - xác nhận đã nhận và xử lý command |
| **WRITE_NO_RSP** | Ghi dữ liệu không cần response từ server (fire-and-forget) |
| **NOTIFY** | Server chủ động gửi dữ liệu cho client khi có thay đổi |
| **Bond** | Thông tin pairing được lưu trữ để kết nối lại mà không cần pairing lại |
| **Pairing** | Quá trình thiết lập bảo mật giữa hai thiết bị BLE |
| **Secure Connections** | Chế độ bảo mật BLE nâng cao sử dụng ECDH key exchange |
| **MITM** | Man-In-The-Middle - loại tấn công trung gian, phòng chống bằng số/pairing |
| **Peripheral** | Thiết bị BLE đóng vai trò server (device trong spec này) |
| **Central** | Thiết bị BLE đóng vai trò client (gateway trong spec này) |

---

# 1. Purpose

Tài liệu này là specification triển khai chính thức cho repository `esp-ble-device` để device firmware map ổn định với Gateway ESP-GATT Protocol v3.

Mục tiêu của Device là:

1. cung cấp BLE Peripheral contract cố định để Gateway nhận biết, connect, secure, subscribe notify và gửi command;
2. decode `device_command`, route command tới product handler và trả ACK đúng correlation;
3. mô tả capability bằng flow `BEGIN -> ITEM* -> END -> ACK`;
4. coi capability refresh là request-driven: Device chỉ response khi nhận `describe_capabilities`, không tự push/discover policy;
5. giữ routing identity đúng theo request từ Gateway;
6. expose product command metadata nhất quán với behavior thực tế;
7. đảm bảo callback BLE bounded, không heap allocation trong GATT command-write callback;
8. phục hồi được repeat pairing khi một phía mất bond;
9. duy trì cross-repo protocol tests để tránh contract drift.

Tài liệu này độc lập đủ để AI coding agent phát triển Device mà không cần đọc toàn bộ internals của Gateway.

---

# 2. Scope

## 2.1 In scope

- `components/gateway_protocol`
- `components/ble_peripheral`
- `components/device_command`
- `components/device_app`
- `components/device_event`
- `devices/reference_device`
- host protocol tests
- cross-repo interoperability vectors/tests
- hardware tests với Gateway companion spec

## 2.2 Out of scope

- Gateway cache/NVS implementation
- Gateway Web UI
- Gateway MCP authorization
- Protocol v4
- BLE application fragmentation
- dynamic capability types beyond NONE/BOOL/INT
- cloud transport
- Device-side periodic capability push
- Device-side knowledge về refresh generation của Gateway

---

# 3. Current Device baseline

## 3.1 Repository structure

Repository hiện có:

```text
components/
├── ble_peripheral/
├── device_app/
├── device_command/
├── device_event/
└── gateway_protocol/

devices/
└── reference_device/
```

## 3.2 Current implementation status

Protocol foundation đã tốt, xem Section 4 để biết canonical contract.

Command pipeline hiện tại:

```text
ABF1 write
-> bounded BLE RX queue
-> device command worker
-> CBOR decode
-> command lookup/handler
-> ACK encode
-> ABF2 notify
```

Capability response hiện có:

```text
describe_capabilities
-> capabilities_begin
-> capability_item*
-> capabilities_end
-> device_ack
```

Reference product hiện public capability:

```text
set_led    BOOL
get_state  NONE
```

## 3.3 Known issues cần hoàn thiện

Các điểm cần sửa trước khi coi implementation complete:

| ID | Issue | Section ref |
|----|-------|-------------|
| I-001 | Normal ACK helper override request routing ID bằng configured `s_cmd.device_id` | §11 |
| I-002 | Built-in `ping/get_info/get_state` policy chưa freeze rõ | §16 |
| I-003 | GATT write callback hiện heap-allocate tạm thời | §9 |
| I-004 | Repeat pairing chỉ retry mà chưa explicit remove old bond | §19 |
| I-005 | Protocol comments/headers cần phân biệt Gateway routing ID và device-native identity | §4.3, §5 |
| I-006 | Cross-repo tests chỉ dựa vào mirrored Gateway codec implementation | §23 |

---

# 4. Shared ESP-GATT v3 contract

## 4.1 BLE service contract

```text
Primary service:     0xABF0
Command characteristic: 0xABF1
  direction: Gateway -> Device
  property: WRITE_NO_RSP

Status characteristic: 0xABF2
  direction: Device -> Gateway
  property: NOTIFY

CCCD: 0x2902
```

Không đổi UUID trong version này.

## 4.2 Protocol version

```text
GW_PROTOCOL_VERSION = 3
GW_MSG_MAX_LEN       = 256
```

New Device messages phải explicit version 3.

Decoder có thể tiếp tục backward compatibility đã thiết kế nếu tests giữ behavior đó.

## 4.3 Direct response routing identity

Gateway gán logical route ID, ví dụ:

```text
lamp-1
```

Device không cần biết ID này từ trước.

Request:

```text
device_id = lamp-1
```

Direct response phải exact echo:

```text
device_ack.device_id          = lamp-1
capabilities_begin.device_id  = lamp-1
capability_item.device_id     = lamp-1
capabilities_end.device_id    = lamp-1
```

Device model:

```text
esp32s3-ref
```

là native metadata, không phải Gateway routing identity.

## 4.4 Request/ACK correlation

ACK phải giữ:

```text
type       = device_ack
request_id = exact request.request_id
command    = exact request.command
device_id  = exact request.device_id
bool_value = handler success/failure
int_value  = result/state when command defines it
```

## 4.5 Capability response order

Device phải preserve exact sequence:

```text
BEGIN
ITEM sequence=0
ITEM sequence=1
...
END
ACK
```

Không gửi ACK trước END.

## 4.6 Capability limits

```text
DEVICE_COMMAND_MAX_CAPABILITIES <= 12
```

Gateway companion spec dựa trên bound này.

---

# 5. Mandatory Device decisions

## D1. Device không triển khai Gateway cache policy

Device không biết request là:

```text
initial discovery
manual refresh
```

Cả hai đều là:

```text
device_command command=describe_capabilities
```

Device chỉ response deterministic theo current capability registry.

Không tự push capability khi reconnect.

## D2. Direct response luôn echo request routing ID

Không dùng:

```c
s_cmd.device_id
```

để override direct response `device_id` nếu field đó chứa model/native identity.

Target ACK helper:

```text
response.device_id = request.device_id
```

Target capability helper đã phải giữ behavior tương tự.

## D3. `device_command_set_device_id()` không dùng cho Gateway routing identity

Nếu API này còn tồn tại vì backward compatibility, phải:

- deprecate hoặc rename semantics;
- không được gọi từ `device_app` bằng `profile.model` để ảnh hưởng ACK;
- không được override request-bound response identity.

Preferred implementation: ACK builder luôn explicit dùng `request->device_id`.

## D4. Device-native identity là concept riêng

Native identity có thể gồm:

```text
model
serial number
hardware revision
firmware version
BLE local name
```

Không overload vào Gateway route ID.

Spontaneous `device_event` không thể biết Gateway-assigned logical ID nếu chưa có provisioning contract riêng.

Gateway phải route spontaneous event theo BLE connection context; Device event field nếu có chỉ là device-native identity/metadata.

## D5. Capability registry là source of truth cho public command

Một command chỉ được coi là public/user-accessible khi được register bằng capability metadata.

Normal command có handler nhưng không advertised là internal/private đối với current Protocol v3 Gateway.

## D6. Built-in policy được freeze

Current built-ins:

```text
ping
get_info
get_state
```

Policy version này:

```text
ping      -> internal by default
get_info  -> internal by default
get_state -> internal built-in handler may be overridden/promoted by product
```

Reference product explicitly advertises/promotes `get_state`.

Không tự advertise `ping/get_info` vì current capability flags chưa có `internal/diagnostic` visibility semantics và UI có thể render chúng thành user controls.

Nếu future Gateway diagnostic path cần chúng, thiết kế riêng internal-command bypass/security policy thay vì làm capability contract mơ hồ.

## D7. Capability revision thuộc product/schema owner

`capability_revision` phải tăng khi persistent public capability schema thay đổi, gồm:

- add/remove public command;
- change value type;
- change flags;
- change min/max/step;
- change label/unit;
- change public command presentation order nếu order intentionally changed.

Không cần tăng khi runtime value thay đổi.

## D8. Capability order là deterministic presentation order

Device emit capability item theo registration order đã freeze trước worker start.

Gateway companion spec coi order này là semantic presentation order.

Do đó product registration order phải deterministic qua build/reboot.

## D9. Capability snapshot ID là runtime transaction ID

`snapshot_id`:

- nonzero;
- increment/wrap skipping 0;
- mới cho mỗi successful `describe_capabilities` response attempt;
- không phải persistent revision;
- không cần survive reboot.

## D10. GATT command-write callback không heap allocate

Callback phải bounded copy trực tiếp từ `os_mbuf` vào queue object.

Không `malloc/free` trong callback.

## D11. Notify batch ordering phải atomic ở producer contract

Capability batch phải reserve/enqueue đủ capacity để preserve:

```text
BEGIN -> ITEM* -> END -> ACK
```

Không cho producer khác xen frame vào giữa capability transaction nếu hiện architecture dùng shared notify queue.

## D12. Repeat pairing phải recover old bond mismatch

Nếu peer yêu cầu repeat pairing vì một phía mất bond, Device phải remove/replace stale local peer security material theo NimBLE-supported sequence rồi retry pairing.

Chỉ return `BLE_GAP_REPEAT_PAIRING_RETRY` mà không xử lý stale bond là chưa đủ acceptance.

---

# 6. Device application startup contract

Target startup sequence giữ modular architecture:

```text
1. logging
2. NVS
3. board/product prerequisites
4. product_init
5. build device profile/native metadata
6. device_command_init
7. set capability_revision
8. register product public capabilities
9. register product internal commands if any
10. freeze command registry / start command worker
11. device_event_init
12. register events
13. ble_peripheral_init
14. product_start
15. ble_peripheral_start / advertising
```

Important identity rule:

```text
profile.model may configure native metadata / event metadata
profile.model MUST NOT become request-bound Gateway routing ID
```

Do not add:

```c
device_command_set_device_id(s_app.device_id);
```

if `s_app.device_id` is model-derived.

---

# 7. BLE advertising and discovery

Primary advertisement must include ESP-GATT service UUID `0xABF0`.

Current design may keep complete local name in scan response because advertisement payload is bounded.

Device does not need to duplicate name into primary advertising packet solely to work around Gateway scan UX.

Companion Gateway spec owns scan-response merge improvement.

Recommended advertising properties:

```text
connectable
scannable
service UUID in primary ADV
complete name in SCAN_RSP
```

---

# 8. BLE connection readiness

Device ready condition remains:

```text
connected
AND security established
AND status notification CCCD enabled
```

State flow example:

```text
ADVERTISING
-> CONNECTED
-> SECURING
-> WAIT_CCCD
-> READY
```

MTU is tracked separately and used to validate outbound notify payload size.

Do not send application notifications before required ready gate is satisfied.

---

# 9. GATT write callback target

Current temporary heap copy must be removed.

Target callback:

```c
static int cmd_chr_write_cb(...)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    size_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > GW_MSG_MAX_LEN) {
        return 0;
    }

    rx_msg_t rx = {
        .len = (uint16_t)len,
    };

    if (os_mbuf_copydata(ctxt->om, 0, len, rx.data) != 0) {
        return 0;
    }

    if (xQueueSend(s_periph.rx_queue, &rx, 0) != pdTRUE) {
        /* bounded warning/counter */
    }
    return 0;
}
```

No:

```text
malloc
calloc
free
CBOR decode
command execution
long formatting
```

inside host callback.

---

# 10. Command RX pipeline

Target remains:

```text
ABF1 write callback
-> bounded raw queue
-> RX consumer / command queue
-> command worker
-> gw_message_decode
-> validate type=device_command
-> validate required fields
-> reserved command handling or handler lookup
-> execute
-> ACK
```

Malformed CBOR may be dropped/logged because reliable application-level error response cannot be correlated if required fields cannot be decoded.

Decoded unknown command should return negative ACK with exact correlation fields.

---

# 11. ACK builder semantics

Recommended direct helper:

```c
static void send_ack(const gw_message_t *request,
                     bool success,
                     int int_value)
{
    gw_message_t ack;
    gw_build_ack(&ack,
                 request,
                 request->device_id,
                 success,
                 int_value);
    ...
}
```

The request route ID is authoritative.

`gw_build_ack()` fallback behavior may remain for generic library use, but Device command pipeline should not depend on configured native ID.

## 11.1 Success semantics

```text
bool_value=true
```

means handler accepted/completed according to command contract.

`int_value` can contain resulting state or query value.

## 11.2 Negative ACK

Unknown command, validation failure after correlation is known, or handler failure:

```text
bool_value=false
int_value=0 unless command defines an error code contract
```

Do not mutate command/request ID/device ID.

## 11.3 Long-running command

Current contract may ACK acceptance immediately:

```text
bool_value=true
```

Completion later should be represented by event/future protocol extension; do not misuse same request ACK twice unless protocol is explicitly extended.

**Note:** Long-running command support is out of scope for v1.3. Current implementation should only return immediate ACK. If a handler needs to perform long-running operations, it should:

1. Return immediate ACK with `bool_value=true` to indicate acceptance;
2. Perform the operation asynchronously;
3. Use `device_event` with `state_changed` type to notify completion (if applicable);
4. Never send a second ACK for the same request_id.

Future protocol versions may introduce explicit command completion events with correlation to original request.

---

# 12. Capability registry API

Public capability registration must contain:

```text
command
handler
value_type
flags
label
unit
min/max/step for INT
```

Validation:

- command non-empty;
- allowed charset consistent with Gateway;
- no duplicate registry name;
- max public count 12;
- value type NONE/BOOL/INT only;
- INT `min <= max`;
- INT `step > 0`;
- label/unit fit wire field sizes.

Registration must complete before registry freeze.

After freeze, public schema/order is immutable for current boot.

---

# 13. Capability response contract

On valid Protocol v3 request:

```text
type=device_command
command=describe_capabilities
request_id != 0
device_id nonempty
```

Device generates runtime snapshot ID and constructs one ordered batch.

## 13.1 BEGIN

Required:

```text
type                = capabilities_begin
command             = describe_capabilities
device_id           = request.device_id
snapshot_id         = nonzero runtime ID
total               = advertised_count
capability_revision = product revision
```

## 13.2 ITEM

For each advertised entry in deterministic registration order:

```text
type        = capability_item
device_id   = request.device_id
snapshot_id = same snapshot
sequence    = 0..N-1
command     = public command
value_type
flags
label
unit
```

INT additionally:

```text
min_value
max_value
step
```

## 13.3 END

```text
type        = capabilities_end
device_id   = request.device_id
snapshot_id = same snapshot
total       = N
bool_value  = true
```

## 13.4 Final ACK

```text
type       = device_ack
request_id = request.request_id
command    = describe_capabilities
device_id  = request.device_id
bool_value = true
```

If batch cannot be produced/enqueued:

```text
send negative ACK when correlation fields are available
```

Do not send a success ACK after partial capability batch failure.

---

# 14. Capability notify batch

Current max batch:

```text
BEGIN + 12 ITEM + END + ACK = 15 frames
```

Notify queue capacity must support one complete maximum batch under chosen atomic producer strategy.

Before enqueue:

```text
check READY
check CCCD enabled
check every encoded frame <= gw_ble_max_tx_payload(negotiated_mtu)
check sufficient queue capacity
serialize producer access
```

Then enqueue full ordered batch.

No partial enqueue if later frame cannot fit or queue capacity is insufficient.

If implementation cannot atomically reserve queue capacity, redesign batch transport helper rather than accepting interleaving.

---

# 15. Product capability revision contract

Reference product owns a constant/revision value, for example:

```c
.capability_revision = 1
```

Increment revision whenever public capability persistent metadata changes.

Example:

Version 1:

```text
set_led BOOL
get_state NONE
```

Version 2 adds:

```text
set_brightness INT 0..100 step 1
```

Then:

```text
capability_revision: 1 -> 2
```

If label/order/range changes, bump revision too.

Gateway will still tolerate same-revision changed content but logs a firmware contract warning; Device tests should prevent that situation.

---

# 16. Built-in command policy

## 16.1 `ping`

Internal by default.

Do not advertise in reference product for current version.

## 16.2 `get_info`

Internal by default.

Current implementation may return protocol version in `int_value`.

Do not advertise until a product/use-case explicitly wants it public.

## 16.3 `get_state`

Generic built-in handler may exist, but reference product replaces/promotes it with actual LED state behavior and capability metadata.

Public capability:

```text
command: get_state
value_type: NONE
idempotent: true
```

ACK returns current LED state in `int_value`.

---

# 17. Reference product contract

Reference firmware profile:

```text
model:              esp32s3-ref
product type:       light
BLE name:           GW-REF
protocol version:   3
capability revision: product-defined
```

Hardware baseline:

```text
LED GPIO:    8
Button GPIO: 9
```

Public capabilities required:

## 17.1 `set_led`

```text
value_type = BOOL
label      = LED power
idempotent = true
```

Protocol v3 input:

```text
request.bool_value
```

ACK:

```text
success=true
int_value=current LED state
```

## 17.2 `get_state`

```text
value_type = NONE
label      = LED state
idempotent = true
```

ACK:

```text
success=true
int_value=current LED state
```

The Gateway companion spec must receive this result rather than collapsing it.

---

# 18. Device events

Current event examples may include:

```text
button_pressed
state_changed
telemetry
```

Spontaneous event routing rule:

- Device cannot assume Gateway logical route ID equals model;
- event native identity may remain product/model-derived if current wire schema requires a `device_id` field;
- Gateway connection context is authoritative for routing;
- do not change direct response identity semantics to match event semantics.

If future protocol requires strict event identity, introduce an explicit provisioning/assigned-ID mechanism rather than reusing model implicitly.

Event worker should remain decoupled from NimBLE callbacks and use bounded queues/mutexes as currently designed.

---

# 19. Repeat pairing recovery

Current behavior that only returns retry must be strengthened.

Target repeat-pairing flow conceptually:

```text
BLE_GAP_EVENT_REPEAT_PAIRING
-> identify peer security record
-> delete stale local bond for that peer using NimBLE store API
-> return BLE_GAP_REPEAT_PAIRING_RETRY
```

Exact NimBLE API must match ESP-IDF version in repository.

Requirements:

- do not erase all bonds unnecessarily;
- only target peer involved when API permits;
- log recovery action;
- repeated failure must not enter tight loop;
- hardware test one-side Gateway factory/bond reset;
- hardware test one-side Device bond reset.

If ESP-IDF API behavior differs, document the actual supported peer-delete sequence in code comments/tests.

---

# 20. Security and bonding

Target security posture remains:

```text
Secure Connections enabled
MITM disabled for NO_IO reference device
bonding enabled according to config
```

Device should persist bonding according to NimBLE/NVS configuration.

Hardware verification must cover:

```text
pair
reboot Device
reconnect
reboot Gateway
reconnect
one-side bond loss
re-pair
```

Do not assume sdkconfig alone proves runtime bond recovery correctness.

---

# 21. MTU and frame size

Preferred MTU may remain 256.

Actual outbound limit:

```text
min(negotiated_mtu - 3, GW_MSG_MAX_LEN)
```

Before notify:

```text
encoded_len <= allowed payload
```

Capability batch helper must reject entire batch if any frame cannot be sent under current MTU.

No application-level fragmentation in this version.

---

# 22. Gateway protocol codec requirements

`gateway_protocol` remains the source of Device CBOR encoding/decoding.

Required behavior:

- strict required-field validation;
- accepted protocol versions per existing backward-compatibility policy;
- explicit v3 on new outbound messages;
- unknown keys tolerated if contract currently permits;
- no trailing garbage accepted;
- request_id 0 invalid when request ID present/required;
- field sizes identical to Gateway companion codec.

Do not change numeric CBOR key assignments independently.

---

# 23. Cross-repository interoperability strategy

## 23.1 Shared golden vectors

Maintain canonical encoded vectors for:

```text
device_command set_led BOOL
device_command get_state NONE
device_ack success + int_value
capabilities_begin
capability_item BOOL
capability_item NONE
capabilities_end
negative ACK
```

Each vector should record:

```text
semantic message
hex bytes
protocol version
expected decoded fields
```

## 23.2 Actual Gateway codec check

Existing mirrored "gateway-style" host codec test is useful smoke coverage but can drift.

At least one CI/check path must either:

- compile actual Gateway codec against Device vectors; or
- generate vectors from actual Gateway source and verify Device decoder; or
- use a shared contract fixture consumed by both repos.

## 23.3 Drift rule

Any change to:

```text
UUID
protocol version
CBOR key
field length
value type
capability ordering contract
ACK correlation
```

requires companion Gateway spec/tests updated together.

---

# 24. Device logging requirements

Log enough to diagnose:

```text
BLE connect/disconnect/security/CCCD
command decode failure
unknown command
handler failure
capability batch start/failure
notify failure
repeat pairing recovery
RX queue full
notify queue full
```

Do not dump raw sensitive payloads by default.

Gateway companion spec owns full frame trace; Device logs only need local operational diagnostics for this phase.

---

# 25. Device file-by-file implementation plan

## 25.1 `components/device_command/device_command.c`

Required:

- make direct ACK always use `request->device_id`;
- ensure capability response already uses request route ID consistently;
- remove/neutralize configured native-ID override from request-bound ACK path;
- preserve final capability ACK after END;
- preserve int response payload;
- add built-in visibility comments/policy;
- keep registry deterministic;
- add revision/order tests.

## 25.2 `components/device_command/include/device_command.h`

Required:

- document route-ID semantics;
- deprecate/clarify `device_command_set_device_id()` if retained;
- document public capability registry contract;
- document max capability count and freeze semantics.

## 25.3 `components/device_app/device_app.c`

Required:

- do not set command routing ID from `profile.model`;
- keep model/native identity for profile/event purposes only;
- set product capability revision before freeze;
- preserve startup ordering.

## 25.4 `components/ble_peripheral/ble_peripheral.c`

Required:

- remove malloc/free from GATT write callback;
- direct `os_mbuf_copydata` to bounded queue object;
- preserve ready gate;
- preserve notify batch ordering/capacity check;
- implement peer-specific repeat-pairing bond cleanup/retry;
- add queue/drop diagnostics.

## 25.5 `components/gateway_protocol/*`

Required:

- keep Protocol v3 constants/keys aligned with Gateway;
- clarify routing identity comments;
- add/maintain golden-vector tests;
- no independent wire-format changes.

## 25.6 `components/device_event/*`

Required:

- document event identity as native metadata, not Gateway route assignment;
- preserve worker/queue thread-safety;
- do not force direct-response identity model onto spontaneous events.

## 25.7 `devices/reference_device/main/reference_product.c`

Required:

- public `set_led` BOOL;
- public `get_state` NONE;
- deterministic registration order;
- capability revision increment when public metadata changes;
- `get_state` ACK int result;
- no accidental public `ping/get_info`.

## 25.8 Host tests

Strengthen:

```text
test_gateway_protocol.c
test_gateway_interop.c
run_gateway_protocol_tests.sh
run_gateway_interop_check.sh
```

with exact identity/revision/order vectors.

---

# 26. Required Device tests

## 26.1 Protocol

### DEV-PROTO-001 — Gateway command decode

Decode actual/golden Gateway v3 `set_led` and preserve all required fields.

### DEV-PROTO-002 — Device ACK encode

Gateway decoder/golden expectation sees exact:

```text
request_id
command
device_id
bool_value
int_value
```

### DEV-PROTO-003 — malformed request rejection

Bad request ID/type/required fields rejected deterministically.

## 26.2 Identity

### DEV-ID-001 — ACK exact route echo

Input:

```text
request.device_id=lamp-1
native model=esp32s3-ref
```

Output:

```text
ack.device_id=lamp-1
```

### DEV-ID-002 — capability exact route echo

All BEGIN/ITEM/END direct responses use `lamp-1`.

### DEV-ID-003 — device_app does not override route ID with model

Static/unit verification as appropriate.

## 26.3 Capability

### DEV-CAP-001 — batch order

```text
BEGIN -> ITEM[0..N-1] -> END -> ACK
```

### DEV-CAP-002 — max count

Registry refuses/handles public count beyond 12.

### DEV-CAP-003 — revision on BEGIN

Exact configured product revision emitted.

### DEV-CAP-004 — deterministic order

Repeated boot/registration emits same sequence.

### DEV-CAP-005 — BOOL metadata

`set_led` fields valid.

### DEV-CAP-006 — NONE metadata

`get_state` fields valid.

### DEV-CAP-007 — INT validation

If test capability used, invalid range/step rejected.

### DEV-CAP-008 — public built-in policy

`ping/get_info` not advertised by default; reference `get_state` advertised.

### DEV-CAP-009 — revision discipline fixture

Test fixture/doc check asserts revision bump when reference public metadata fixture intentionally changes.

## 26.4 Command

### DEV-CMD-001 — set_led v3 BOOL

Handler consumes bool field and ACK reflects resulting state.

### DEV-CMD-002 — get_state result

ACK int value equals current LED state.

### DEV-CMD-003 — unknown command

Negative ACK exact correlation.

### DEV-CMD-004 — long-running acceptance

One immediate success ACK; no duplicate request ACK.

## 26.5 BLE

### DEV-BLE-001 — callback no heap allocation

Code-level/unit guard as practical; callback contains no malloc/calloc/free.

### DEV-BLE-002 — RX queue bounded

Oversize/queue-full behavior deterministic.

### DEV-BLE-003 — notify batch order

No interleave inside capability batch.

### DEV-BLE-004 — MTU reject

Entire batch rejected when one frame exceeds payload.

### DEV-BLE-005 — READY gate

Notify fails/refuses before security+CCCD ready.

### DEV-BLE-006 — repeat pairing recovery

Peer-specific stale bond removed before retry according to NimBLE contract.

---

# 27. Hardware verification matrix

Use companion Gateway with logical ID `lamp-1`.

## HW-DEV-001 First advertising/discovery

Gateway sees service UUID and can connect.

Name may arrive via scan response.

## HW-DEV-002 Security + CCCD

Device reaches READY only after security and notify subscription.

## HW-DEV-003 Initial capability exchange

Observed wire order:

```text
BEGIN
set_led ITEM
get_state ITEM
END
ACK
```

All direct response IDs equal `lamp-1` even though model is `esp32s3-ref`.

## HW-DEV-004 set_led

Gateway sends BOOL; physical LED changes; ACK success.

## HW-DEV-005 get_state

Gateway receives correct int result.

## HW-DEV-006 Device reboot

Bond/reconnect works; Device does not autonomously push capability.

## HW-DEV-007 Gateway reboot

Reconnect works with persisted security as expected.

## HW-DEV-008 Manual capability refresh

Second `describe_capabilities` returns deterministic metadata and new runtime snapshot ID.

## HW-DEV-009 One-side Gateway bond reset

Device repeat-pairing recovery allows re-pair.

## HW-DEV-010 One-side Device bond reset

Gateway/device can recover and re-pair.

## HW-DEV-011 Stress

Repeated set/get + capability requests do not overflow queues under normal negotiated MTU.

---

# 28. Implementation phases

| Phase | Scope | Est. effort | Exit criteria | Dependencies |
|-------|-------|-------------|---------------|--------------|
| **A** | Routing identity correctness | 1-2 days | DEV-ID suite passes | None |
| **B** | BLE callback and pairing robustness | 2-3 days | DEV-BLE suite passes | None |
| **C** | Capability contract freeze | 1-2 days | DEV-CAP suite passes | Phase A |
| **D** | Protocol/cross-repo verification | 1-2 days | DEV-PROTO + cross-repo checks pass | Phase A, C |
| **E** | Reference hardware end-to-end | 2-3 days | HW-DEV matrix complete | Phase A, B, C, D |

**Total estimated effort:** 7-12 days (single developer, assuming hardware available)

## Phase A — Routing identity correctness

- ACK exact request route ID;
- clarify/deprecate native ID override;
- device_app identity cleanup;
- identity tests.

Exit: DEV-ID suite passes.

## Phase B — BLE callback and pairing robustness

- remove callback heap;
- queue behavior;
- repeat pairing peer cleanup;
- security hardware checks.

Exit: DEV-BLE suite passes.

## Phase C — Capability contract freeze

- deterministic registry order;
- built-in visibility policy;
- revision rules;
- capability batch tests.

Exit: DEV-CAP suite passes.

## Phase D — Protocol/cross-repo verification

- golden vectors;
- actual Gateway codec interoperability path;
- request/ACK field drift checks.

Exit: DEV-PROTO + cross-repo checks pass.

## Phase E — Reference hardware end-to-end

- set_led;
- get_state;
- reboot/reconnect;
- manual capability request;
- one-side bond resets;
- stress.

Exit: HW-DEV matrix complete.

---

# 29. Build and verification workflow

Reference firmware typical build:

```bash
cd devices/reference_device
idf.py set-target esp32s3
idf.py build
```

Host checks should run before hardware flash where possible.

Recommended order:

```text
1. gateway_protocol host tests
2. device_command/capability host tests
3. BLE component tests/mocks where available
4. cross-repo vectors
5. reference firmware build
6. flash Device
7. hardware matrix with Gateway
```

A compile-success alone is not completion.

---

# 30. Device acceptance criteria

| ID | Criterion | Test ref | Status |
|----|-----------|----------|--------|
| AC-001 | Service/characteristic UUID contract remains ABF0/ABF1/ABF2 | DEV-PROTO-001 | [ ] |
| AC-002 | Protocol v3 remains wire-compatible with Gateway | DEV-PROTO-001, DEV-PROTO-002 | [ ] |
| AC-003 | Direct ACK exact-echoes request ID, command and request device ID | DEV-ID-001 | [ ] |
| AC-004 | Device model/native identity never overrides request-bound route ID | DEV-ID-003 | [ ] |
| AC-005 | Capability BEGIN/ITEM/END exact-echo request device ID | DEV-ID-002 | [ ] |
| AC-006 | Capability order is BEGIN -> ordered ITEM* -> END -> ACK | DEV-CAP-001 | [ ] |
| AC-007 | Public capability count is bounded at 12 | DEV-CAP-002 | [ ] |
| AC-008 | Capability registration order is deterministic and treated as presentation order | DEV-CAP-004 | [ ] |
| AC-009 | Product revision is emitted and updated for public metadata changes | DEV-CAP-003, DEV-CAP-009 | [ ] |
| AC-010 | `ping/get_info` remain internal by default | DEV-CAP-008 | [ ] |
| AC-011 | Reference `set_led` and `get_state` are advertised public capabilities | DEV-CAP-005, DEV-CAP-006 | [ ] |
| AC-012 | `get_state` ACK carries current state in int value | DEV-CMD-002 | [ ] |
| AC-013 | Unknown command returns correlated negative ACK | DEV-CMD-003 | [ ] |
| AC-014 | GATT command-write callback performs no heap allocation | DEV-BLE-001 | [ ] |
| AC-015 | Notify batch cannot partially enqueue/interleave under normal helper contract | DEV-BLE-003 | [ ] |
| AC-016 | MTU is validated before actual notify send | DEV-BLE-004 | [ ] |
| AC-017 | Application notifications require READY gate | DEV-BLE-005 | [ ] |
| AC-018 | Repeat pairing removes stale peer bond and retries safely | DEV-BLE-006 | [ ] |
| AC-019 | Device reboot/reconnect works | HW-DEV-006 | [ ] |
| AC-020 | Gateway reboot/reconnect works | HW-DEV-007 | [ ] |
| AC-021 | One-side bond reset is recoverable in both directions | HW-DEV-009, HW-DEV-010 | [ ] |
| AC-022 | Shared/cross-repo vectors pass | §23 | [ ] |
| AC-023 | Reference hardware set/get/capability flow passes end-to-end | HW-DEV-003, HW-DEV-004, HW-DEV-005 | [ ] |

---

# 31. Rules for AI coding agent

1. Không đổi UUID/CBOR key/Protocol v3 independently.
2. Không implement Gateway cache policy trong Device.
3. Không tự push capability trên reconnect.
4. Không set model `esp32s3-ref` làm Gateway routing ID.
5. Direct ACK/capability response phải dùng exact `request.device_id`.
6. Không để `device_command_set_device_id()` override request-bound ACK bằng native ID.
7. Không advertise built-in command nếu product chưa explicit promote thành capability.
8. Giữ `ping/get_info` internal trong reference firmware version này.
9. Giữ `get_state` public thông qua reference product registration.
10. Bump capability revision khi public persistent metadata/order thay đổi.
11. Không đổi capability response order.
12. Không gửi partial capability batch rồi success ACK.
13. Không malloc/free trong GATT command-write callback.
14. Không decode CBOR/execute command trong NimBLE host callback.
15. Không notify application frame trước security+CCCD READY.
16. Không bỏ MTU payload validation.
17. Repeat pairing phải xử lý stale local bond, không chỉ retry mù.
18. Không dùng spontaneous event native identity để suy ra request routing semantics.
19. Cross-repo protocol change phải update companion Gateway spec/tests.
20. Mỗi phase cần tests trước khi coi hoàn tất.

---

# 32. Companion Gateway expectations

Device implementation giả định Gateway companion spec thực hiện:

```text
cache-first capability management
no rediscovery when cache exists
manual refresh by describe_capabilities request
exact request/ACK matcher
Gateway route ID supplied in request
get_state ACK int payload propagated to caller
BLE runtime ready before manual refresh
static MCP allowlist independent from capability
full Gateway-side message trace
failure-safe delete/rebind capability invalidation
```

Device không được thêm workaround làm lệch wire protocol nếu Gateway feature chưa implement xong.

---

# 33. Change summary from combined v1.2

Device v1.3 split-spec incorporates all Device-relevant content from combined v1.2 and the subsequent freeze review:

- separates Device responsibilities from Gateway cache/REST internals;
- freezes request-bound routing identity as exact request echo;
- reserves `esp32s3-ref` for native model examples and uses `lamp-1` for Gateway route examples;
- prevents `device_app` model-derived ID from overriding ACK routing;
- freezes built-in visibility policy: `ping/get_info` internal, reference `get_state` public;
- defines deterministic capability order as presentation semantics;
- strengthens capability revision ownership rules;
- requires no heap allocation in GATT command-write callback;
- requires real repeat-pairing stale-bond cleanup before retry;
- keeps capability behavior purely request-driven and independent of Gateway cache policy;
- adds exact identity, order, revision, pairing and MTU tests;
- strengthens cross-repo testing so mirrored codecs cannot silently drift.

This file is the authoritative implementation specification for `esp-ble-device` v1.3 scope.
