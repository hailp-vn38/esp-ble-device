# ESP-BLE-Gateway → ESP-BLE-Device Integration Contract

**Document type:** Gateway integration contract / AI Agent context  
**Version:** 1.0  
**Date:** 26/08/2026  
**Gateway repository:** `hailp-vn38/esp-ble-gateway`  
**Gateway branch reviewed:** `main`  
**Gateway snapshot reviewed:** `e049ec880c674551bb0a5645f94e396437580012`  
**Target consumer:** AI Agent / developer implementing `esp-ble-device`  
**Current wire protocol:** v2  
**Current BLE architecture:** Gateway = Central / GATT Client; Device = Peripheral / GATT Server  

---

# 0. READ THIS FIRST — mục đích của tài liệu

Tài liệu này trả lời một câu hỏi duy nhất:

> **Một firmware trong `esp-ble-device` phải hoạt động như thế nào để Gateway hiện tại có thể nhận biết, kết nối, bảo mật, discover GATT, gửi command, nhận ACK/event và tự reconnect ổn định?**

Đây không phải tài liệu mô tả toàn bộ nội bộ Gateway.

Đây là **contract phía Gateway nhìn sang Device**.

AI Agent xây dựng `esp-ble-device` phải coi tài liệu này là compatibility boundary.

---

# 1. Quy ước mức độ yêu cầu

Tài liệu dùng bốn nhãn:

```text
[CURRENT]
```

Hành vi đang tồn tại trong source code Gateway hiện tại.

```text
[DEVICE MUST]
```

Điều Device bắt buộc phải làm để tương thích với Gateway hiện tại.

```text
[DEVICE SHOULD]
```

Khuyến nghị để integration ổn định, tránh race/drop/reconnect lỗi.

```text
[FUTURE]
```

Kiến trúc hoặc cải tiến được định hướng nhưng không được coi là contract hiện tại.

AI Agent không được biến `[FUTURE]` thành dependency bắt buộc khi implementation Phase hiện tại.

---

# 2. Source of truth hiện tại

Tại snapshot được review, các file Gateway sau là source of truth chính cho Device integration:

```text
components/ble_central/README.md
components/ble_central/ble_central_internal.h
components/ble_central/ble_central_scan.c
components/ble_central/ble_central_gap.c
components/ble_central/ble_central_gatt.c

components/cbor_codec/include/cbor_codec.h
components/cbor_codec/cbor_codec.c

components/command_dispatcher/README.md
components/command_dispatcher/command_dispatcher.c
components/command_dispatcher/gateway_commands.c

components/device_store/include/device_store.h

sdkconfig.defaults
```

Nếu tài liệu này khác với source code Gateway ở commit mới hơn, source code mới hơn phải được review và contract phải được cập nhật.

---

# 3. Mission của `esp-ble-device`

`esp-ble-device` phải cung cấp một framework để nhiều product firmware có thể trở thành peripheral tương thích với Gateway.

Ví dụ:

```text
reference_device
relay_1ch
relay_4ch
temperature_sensor
motion_sensor
motor_controller
```

Mọi product dùng chung transport contract:

```text
BLE Peripheral
        +
GATT Server
        +
ESP-GATT Protocol v2
```

Product logic có thể khác nhau.

Wire contract không được fork theo product.

---

# 4. System architecture

```text
                    LAN / Web / MCP
                           │
                           ▼
                 ┌──────────────────┐
                 │ ESP BLE Gateway  │
                 │                  │
                 │ BLE Central      │
                 │ GATT Client      │
                 └────────┬─────────┘
                          │
                          │ BLE
                          │
                 ┌────────▼─────────┐
                 │ ESP BLE Device   │
                 │                  │
                 │ BLE Peripheral   │
                 │ GATT Server      │
                 └────────┬─────────┘
                          │
                          ▼
                    Product logic
```

Direction:

```text
Gateway → Device
device_command
```

```text
Device → Gateway
device_ack
device_event
```

---

# 5. Non-negotiable compatibility summary

AI Agent phải implement các điểm sau trước bất kỳ product feature nào:

```text
[DEVICE MUST] advertise service UUID 0xABF0

[DEVICE MUST] expose primary service 0xABF0

[DEVICE MUST] expose characteristic 0xABF1
              with WRITE WITHOUT RESPONSE

[DEVICE MUST] expose characteristic 0xABF2
              with NOTIFY

[DEVICE MUST] expose CCCD 0x2902 for 0xABF2

[DEVICE MUST] support Gateway-initiated security

[DEVICE MUST] support bonding-compatible operation

[DEVICE MUST] implement protocol v2 for command/ACK integration

[DEVICE MUST] decode device_command from 0xABF1

[DEVICE MUST] notify device_ack through 0xABF2

[DEVICE MUST] echo request_id in ACK

[DEVICE MUST] echo command in ACK

[DEVICE MUST] include device_id in ACK

[DEVICE MUST] keep encoded message <= 256 bytes

[DEVICE MUST] also keep Notify <= negotiated MTU - 3

[DEVICE MUST] restart advertising after disconnect

[DEVICE SHOULD] use a stable BLE identity across reboot
```

---

# 6. Gateway BLE role

[CURRENT]

Gateway NimBLE configuration:

```text
Central      enabled
Observer     enabled
Peripheral   disabled
Broadcaster  disabled
GATT Client  enabled
```

Gateway owns the connection.

Device must be connectable Peripheral.

---

# 7. Gateway capacity

[CURRENT]

Gateway has:

```text
CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 9
DEVICE_STORE_MAX_DEVICES         = 16
```

Meaning:

```text
up to 16 configured logical devices
up to 9 BLE connection slots
```

Device must not assume it is the only device connected to Gateway.

---

# 8. Device connection model

[DEVICE SHOULD]

Normal Device requires:

```text
CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 1
```

Phase hiện tại không yêu cầu một Device phục vụ nhiều Gateway đồng thời.

---

# 9. Gateway discovery filter

[CURRENT]

Gateway scanner parses advertising fields.

Một scan result chỉ được forward lên application khi advertising fields chứa:

```text
16-bit Service UUID = 0xABF0
```

Gateway không dùng BLE name để quyết định protocol compatibility.

---

# 10. Advertising requirement

[DEVICE MUST]

Device phải advertise:

```text
0xABF0
```

trong 16-bit Service UUID list.

Recommended:

```text
Flags
Complete 16-bit Service UUIDs:
    0xABF0
Local Name:
    optional/product-friendly name
```

---

# 11. Primary advertising packet

[DEVICE SHOULD]

Đặt:

```text
0xABF0
```

trong primary advertising packet, không phụ thuộc hoàn toàn vào scan response.

Lý do:

Gateway compatibility filter trực tiếp parse advertising report và tìm UUID.

BLE name có thể đặt trong ADV hoặc scan response tùy giới hạn packet.

Service UUID quan trọng hơn name.

---

# 12. BLE name

[CURRENT]

Gateway scan result có thể copy local name nếu có.

Nhưng name chỉ là metadata/display information.

[DEVICE MUST NOT]

Không thiết kế compatibility dựa trên:

```text
GW-Relay
GW-Sensor
ESP32_xxx
```

[DEVICE SHOULD]

Tên có thể dùng format:

```text
GW-Relay-A1
GW-Temp-B2
```

nhưng không phải identity contract.

---

# 13. Logical identity và BLE identity

Gateway phân biệt hai khái niệm.

## Logical identity

```text
device_id
```

Ví dụ:

```text
relay_01
sensor_bedroom
pump_2
```

## Transport identity

```text
BLE address
BLE address type
```

Hai identity không phải một.

---

# 14. Device Store model

[CURRENT]

Gateway persistent entry chứa:

```c
typedef struct {
    char device_id[32];
    char name[32];
    char type[16];

    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    bool has_ble_identity;
} device_entry_t;
```

Gateway không parse `device_id` thành BLE address.

---

# 15. Device identity requirement

[DEVICE SHOULD]

BLE identity phải ổn định qua reboot.

Recommended:

```text
public identity
```

hoặc:

```text
random static identity persisted
```

Tránh:

```text
random non-resolvable identity thay đổi mỗi boot
```

nếu không có onboarding/identity-resolution flow phù hợp.

---

# 16. Vì sao stable identity quan trọng

[CURRENT]

Gateway lưu BLE identity để:

```text
connect
reconnect
forget bond
map logical device → peer
```

Sau connect Gateway đọc canonical:

```text
peer_id_addr
```

và persist lại identity thông qua worker ngoài NimBLE callback.

Nếu Device đổi identity tùy tiện sau reboot, auto reconnect có thể thất bại.

---

# 17. Onboarding model hiện tại

Flow khái niệm:

```text
Device
  │
  │ advertise 0xABF0
  ▼
Gateway scan
  │
  ▼
scan result:
  BLE addr
  addr type
  RSSI
  name
  │
  ▼
add_device
  │
  ├── persist logical device
  ├── persist BLE identity if supplied
  └── best-effort connect
```

---

# 18. add_device semantics

[CURRENT]

`add_device`:

```text
device_id required

name:
  optional
  default = device_id

device_type:
  optional
  default = generic

BLE identity:
  optional
```

Nếu BLE identity được truyền vào và persist thành công, Gateway thực hiện connect best-effort.

Persistence success mới quyết định `add_device` success.

Connection request chỉ là side effect.

---

# 19. Duplicate BLE identity

[CURRENT]

Gateway Device Store không cho một canonical BLE identity thuộc hai `device_id`.

Device development/test không nên reuse cùng một stable BLE identity cho nhiều board.

---

# 20. GATT contract

[CURRENT]

```text
Primary Service 0xABF0
│
├── Characteristic 0xABF1
│   Name: COMMAND
│   Direction: Gateway → Device
│   Property required:
│       WRITE WITHOUT RESPONSE
│
└── Characteristic 0xABF2
    Name: STATUS
    Direction: Device → Gateway
    Property required:
        NOTIFY
    Descriptor:
        CCCD 0x2902
```

---

# 21. Service UUID

[DEVICE MUST]

GATT Server phải expose:

```text
Primary Service UUID 0xABF0
```

[DEVICE SHOULD]

Chỉ expose một service `0xABF0` để tránh ambiguity trong discovery.

---

# 22. COMMAND characteristic

[DEVICE MUST]

UUID:

```text
0xABF1
```

Property:

```text
WRITE WITHOUT RESPONSE
```

Gateway discovery kiểm tra bit:

```text
BLE_GATT_CHR_F_WRITE_NO_RSP
```

Nếu thiếu property này, Gateway terminate connection.

---

# 23. STATUS characteristic

[DEVICE MUST]

UUID:

```text
0xABF2
```

Property:

```text
NOTIFY
```

Gateway discovery kiểm tra bit:

```text
BLE_GATT_CHR_F_NOTIFY
```

Nếu thiếu, Gateway terminate connection.

---

# 24. CCCD

[DEVICE MUST]

STATUS phải có CCCD chuẩn:

```text
UUID 0x2902
```

Gateway sau discovery sẽ ghi:

```text
0x01 0x00
```

vào CCCD để enable notifications.

Nếu không tìm thấy CCCD:

```text
discovery failed
connection terminated
```

---

# 25. READY definition

Đây là điểm rất quan trọng.

[CURRENT]

Gateway không coi:

```text
ACL connected
```

là đủ.

Gateway chỉ coi Device connected/usable khi connection state đạt:

```text
READY
```

Flow:

```text
CONNECTING
   ↓
SECURING
   ↓
DISCOVERING
   ↓
CCCD WRITE SUCCESS
   ↓
READY
```

---

# 26. `is_connected()` semantics

[CURRENT]

`ble_central_is_connected(device_id)` nghĩa là:

```text
GATT session READY
```

không chỉ physical BLE link.

Device command chỉ nên được kỳ vọng sau READY.

---

# 27. Device-side READY semantics

[DEVICE SHOULD]

Device cũng nên có state tương ứng:

```text
ADVERTISING
CONNECTED
SECURING
WAIT_SUBSCRIBE
READY
```

Device-side `READY` nên chỉ true khi:

```text
link exists
security established
STATUS notifications enabled
```

---

# 28. Gateway connection sequence

[CURRENT]

Sau BLE connect success Gateway:

```text
1. resolves canonical peer identity
2. schedules identity persistence
3. starts MTU exchange
4. initiates link security
5. waits encryption change success
6. starts GATT service discovery
7. discovers characteristics
8. validates properties
9. discovers STATUS CCCD
10. writes CCCD = notify enabled
11. marks connection READY
```

---

# 29. Device implementation sequence

[DEVICE MUST]

Device phải sẵn sàng để Gateway thực hiện sequence trên ngay sau connect.

Do đó Device không được:

```text
advertise trước khi GATT table ready
advertise trước khi command subsystem ready
advertise trước khi protocol RX queue ready
```

---

# 30. Security contract

[CURRENT]

Gateway security baseline:

```text
Security enabled
Bonding
LE Secure Connections
Legacy pairing also enabled
NVS bond persistence
IO capability: NO_IO
```

Gateway chủ động gọi security initiate ngay sau connect.

---

# 31. Device security requirement

[DEVICE MUST]

Device phải cấu hình Security Manager tương thích với:

```text
bonding
LE Secure Connections
NO_IO flow
```

[DEVICE SHOULD]

Giữ bond persistence qua reboot.

---

# 32. Repeat pairing

[CURRENT]

Nếu Gateway nhận:

```text
BLE_GAP_EVENT_REPEAT_PAIRING
```

Gateway xóa peer bond đang lưu và trả:

```text
BLE_GAP_REPEAT_PAIRING_RETRY
```

Device phải xử lý pairing retry bình thường.

---

# 33. Factory reset và bond

[DEVICE SHOULD]

Factory reset của Device phải có khả năng clear:

```text
bond database
ownership/claim state
resettable settings
```

nhưng không xóa factory calibration nếu product cần giữ.

---

# 34. Security timeout

[CURRENT]

Gateway timeout:

```text
BLE_SECURITY_TIMEOUT_MS = 10000
```

Nếu security không hoàn tất trong khoảng này, supervisor có thể terminate link.

[DEVICE MUST]

Không để pairing/security process treo lâu hơn contract này trong normal operation.

---

# 35. GATT discovery timeout

[CURRENT]

```text
BLE_GATT_DISCOVERY_TIMEOUT_MS = 10000
```

[DEVICE MUST]

GATT Server phải respond discovery bình thường và không phụ thuộc product initialization dài sau connect.

---

# 36. Connect timeout

[CURRENT]

```text
BLE_CONNECT_TIMEOUT_MS = 10000
```

Device advertising/connectability phải ổn định.

---

# 37. Reconnect behavior

[CURRENT]

Gateway reconnect supervisor tick:

```text
1000 ms
```

Backoff:

```text
2s
4s
8s
16s
30s
30s...
```

Backoff reset khi connection đạt READY.

---

# 38. Device disconnect behavior

[DEVICE MUST]

Sau disconnect Device phải quay lại advertising, trừ trường hợp explicit shutdown/factory workflow.

Recommended:

```text
READY
  ↓ disconnect
ADVERTISING
```

---

# 39. Không reset business state chỉ vì disconnect

[DEVICE SHOULD]

BLE disconnect không nên tự động:

```text
turn relay off
erase settings
reset sensor config
```

trừ khi product safety specification yêu cầu.

BLE availability và product state là hai domain khác nhau.

---

# 40. Connection interval

[CURRENT]

Gateway chọn connection interval theo số active links:

```text
< 3 links:
    15 ms

< 6 links:
    30 ms

>= 6 links:
    50 ms
```

Connection latency:

```text
0
```

Supervision timeout requested:

```text
2 seconds
```

---

# 41. Device interval rule

[DEVICE MUST]

Không hard-code logic yêu cầu Gateway luôn dùng một connection interval cố định.

[DEVICE SHOULD]

Chấp nhận connection parameter update hợp lệ trong khoảng trên nếu chip/product cho phép.

---

# 42. MTU

[CURRENT]

Gateway preferred ATT MTU:

```text
256
```

Gateway bắt đầu MTU exchange sau connect.

Nhưng negotiated MTU có thể nhỏ hơn.

---

# 43. Message size rules

Hai giới hạn cùng tồn tại:

```text
Protocol max:
GW_MSG_MAX_LEN = 256
```

và:

```text
ATT payload:
negotiated_mtu - 3
```

---

# 44. Gateway TX size handling

[CURRENT]

Gateway encode command rồi reject nếu:

```text
encoded_len > MTU - 3
```

---

# 45. Gateway RX size handling

[CURRENT]

STATUS notification bị reject trước queue nếu:

```text
packet_len == 0
```

hoặc:

```text
packet_len > 256
```

---

# 46. Device TX requirement

[DEVICE MUST]

Khi notify:

```text
encoded_len <= 256
```

và:

```text
encoded_len <= negotiated_mtu - 3
```

Nên dùng:

```text
max_tx_payload = min(256, mtu - 3)
```

---

# 47. Không hard-code 253

[DEVICE MUST NOT]

Không assume:

```text
MTU = 256
payload = 253
```

ở runtime.

Phải đọc negotiated MTU.

---

# 48. Wire format

[CURRENT]

Wire payload là:

```text
CBOR map
```

với numeric keys.

Không phải JSON trên BLE.

JSON chỉ dùng ở upper layers/conversion.

---

# 49. Protocol version

[CURRENT]

```c
#define GW_PROTOCOL_VERSION 2
```

---

# 50. Version nuance quan trọng

[CURRENT]

CBOR decoder có thể parse:

```text
protocol_version 1
protocol_version 2
```

và reject:

```text
0
> 2
```

Tuy nhiên dispatcher Phase hiện tại correlation ACK bằng `request_id`.

Vì vậy:

```text
[DEVICE MUST] new esp-ble-device implements protocol v2
```

Không target v1 cho firmware mới.

---

# 51. Missing protocol_version nuance

[CURRENT]

Decoder mặc định protocol version hiện tại nếu key version không tồn tại.

Nhưng:

```text
[DEVICE MUST]
```

Firmware mới phải emit explicit:

```text
protocol_version = 2
```

Không dựa vào decoder fallback.

---

# 52. CBOR key table

[CURRENT]

```text
0  protocol_version
1  type
2  device_id
3  command
4  int_value
5  bool_value
6  name
7  device_type
8  ble_addr
9  ble_addr_type
10 request_id
```

Không tự đổi numeric key.

---

# 53. Current `gw_message_t`

[CURRENT]

```c
typedef struct {
    uint8_t protocol_version;

    char type[24];
    char device_id[32];
    char command[32];

    uint32_t request_id;
    int has_request_id;

    int int_value;
    int bool_value;

    int has_device_id;

    char name[32];
    char device_type[16];

    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    int has_ble_addr;
} gw_message_t;
```

---

# 54. Effective string limits

Do buffers cần NUL terminator:

```text
type:
    max CBOR string bytes = 23

device_id:
    max = 31

command:
    max = 31

name:
    max = 31

device_type:
    max = 15
```

[DEVICE MUST]

Không emit chuỗi dài bằng đúng capacity nếu decoder phải thêm `\0`.

---

# 55. Required CBOR fields — rất quan trọng

Gateway decoder hiện tại yêu cầu các field sau tồn tại:

```text
type
command
int_value
bool_value
```

Điều này đúng kể cả khi semantic của message không cần cả hai value fields.

---

# 56. `int_value` requirement

[DEVICE MUST]

Mọi message Device → Gateway phải encode key:

```text
4 = int_value
```

Nếu không dùng:

```text
int_value = 0
```

---

# 57. `bool_value` requirement

[DEVICE MUST]

Mọi message Device → Gateway phải encode key:

```text
5 = bool_value
```

Nếu không dùng:

```text
bool_value = false
```

hoặc semantic tương ứng.

---

# 58. `type` requirement

[DEVICE MUST]

Không được rỗng.

Device TX hợp lệ hiện tại:

```text
device_ack
device_event
```

---

# 59. `command` requirement

[DEVICE MUST]

Mọi message phải có `command` non-empty.

Với event:

```text
command = event name
```

Ví dụ:

```text
button_pressed
temperature
state_changed
```

---

# 60. request_id rules

At codec level:

```text
request_id optional
```

Nếu present:

```text
1 <= request_id <= UINT32_MAX
```

`0` bị reject.

---

# 61. ACK request_id rule

[DEVICE MUST]

`device_ack` phải có:

```text
request_id
```

và phải echo chính xác `request_id` từ `device_command`.

---

# 62. device_id rules

At codec level:

```text
device_id optional
```

Nhưng ACK dispatcher yêu cầu explicit.

---

# 63. ACK device_id rule

[DEVICE MUST]

`device_ack` phải chứa:

```text
device_id
```

non-empty.

---

# 64. Event device_id nuance

[CURRENT]

Gateway nhận `device_id` transport context từ BLE connection trước khi gọi dispatcher.

Với:

```text
device_event
```

dispatcher hiện không yêu cầu embedded `msg.device_id`.

Do đó embedded event `device_id` không phải hard requirement của Gateway hiện tại.

[DEVICE SHOULD]

Vẫn include `device_id` trong `device_event` để:

```text
debugging dễ hơn
protocol consistency
future routing
test clarity
```

nếu không gây size problem.

---

# 65. `name` và `device_type`

Optional wire fields:

```text
name
device_type
```

Không bắt buộc cho ACK/event bình thường.

---

# 66. BLE address fields

Optional:

```text
ble_addr
ble_addr_type
```

Product Device không nên đưa chúng vào mọi event.

Đây chủ yếu là metadata/control schema phía Gateway.

---

# 67. Gateway encoder behavior

[CURRENT]

Khi Gateway encode message, nó luôn thêm:

```text
protocol_version
type
command
int_value
bool_value
```

Optional:

```text
device_id
request_id
name
device_type
ble_addr + ble_addr_type
```

Device decoder phải tương thích.

---

# 68. Device command example

JSON-equivalent để đọc:

```json
{
  "protocol_version": 2,
  "type": "device_command",
  "device_id": "relay_01",
  "command": "set_state",
  "request_id": 142,
  "int_value": 0,
  "bool_value": true
}
```

Wire thực tế là CBOR numeric map.

---

# 69. Device command CBOR diagnostic notation

Ví dụ:

```text
{
  0: 2,
  1: "device_command",
  2: "relay_01",
  3: "set_state",
  4: 0,
  5: true,
  10: 142
}
```

CBOR map order không phải contract.

Numeric key mới là contract.

---

# 70. ACK success example

```text
{
  0: 2,
  1: "device_ack",
  2: "relay_01",
  3: "set_state",
  4: 0,
  5: true,
  10: 142
}
```

---

# 71. ACK failure example

```text
{
  0: 2,
  1: "device_ack",
  2: "relay_01",
  3: "set_state",
  4: 0,
  5: false,
  10: 142
}
```

Gateway map:

```text
bool_value = true
→ command OK
```

```text
bool_value = false
→ DEVICE_ERROR
```

---

# 72. Event example

```text
{
  0: 2,
  1: "device_event",
  2: "relay_01",
  3: "button_pressed",
  4: 1,
  5: true
}
```

Không có `request_id` vì event không phải ACK.

---

# 73. Sensor value convention Phase 1

Protocol v2 chỉ có common scalar:

```text
int_value
bool_value
```

[DEVICE SHOULD]

Dùng scaled integer + unit encoded by command/event contract.

Ví dụ:

```text
command = temperature_mC
int_value = 25375
```

nghĩa là:

```text
25.375 °C
```

Không tự thêm float/string payload mà Gateway hiện chưa hỗ trợ.

---

# 74. Message types trên Gateway

[CURRENT]

Known wire types:

```text
gateway_command
device_command
device_ack
device_event
```

Device RX application chỉ cần xử lý:

```text
device_command
```

Device TX:

```text
device_ack
device_event
```

---

# 75. Device must reject wrong direction types

[DEVICE SHOULD]

Nếu Device nhận qua COMMAND:

```text
gateway_command
device_ack
device_event
```

không route vào product handler.

Có thể log/drop.

---

# 76. Gateway command dispatch lifecycle

[CURRENT]

Khi upper layer gửi `device_command`:

```text
1. validate device is READY
2. allocate one pending request for device
3. clone wire message
4. assign monotonic nonzero request_id
5. send via ABF1 Write Without Response
6. wait ACK up to 2000 ms
7. release pending slot
```

---

# 77. One pending request per Device

[CURRENT]

Phase 1 invariant:

```text
1 pending command / device
```

Nếu đã có pending:

```text
BUSY
```

Device không cần support parallel in-flight request semantics từ normal Gateway dispatcher ở Phase 1.

---

# 78. ACK timeout

[CURRENT]

```text
DISPATCHER_ACK_TIMEOUT_MS = 2000
```

Nếu không có matching ACK:

```text
TIMEOUT
```

---

# 79. Device ACK performance target

[DEVICE MUST]

ACK trước 2 giây.

[DEVICE SHOULD]

Target normal operation:

```text
< 250 ms
```

để có margin cho:

```text
BLE scheduling
multiple Gateway links
notification queue
FreeRTOS scheduling
```

---

# 80. ACK correlation exact rule

[CURRENT]

Pending request chỉ complete nếu tất cả khớp:

```text
type == "device_ack"

AND

transport device_id matches pending device

AND

embedded msg.device_id matches

AND

request_id matches

AND

command matches
```

---

# 81. ACK command echo

[DEVICE MUST]

Không biến:

```text
command = set_state
```

thành:

```text
command = state_changed
```

trong ACK.

ACK phải echo command request.

Completion event có thể dùng name khác.

---

# 82. ACK request_id echo

[DEVICE MUST]

Không generate request_id mới.

Không increment.

Không reuse request trước.

Phải copy exact value từ request.

---

# 83. ACK device_id echo

[DEVICE MUST]

ACK phải trả logical `device_id` expected by Gateway.

Nếu Device không biết logical ID chính xác thì identity/provisioning design phải giải quyết trước production integration.

Không bỏ field này khỏi ACK.

---

# 84. Stale ACK

[CURRENT]

ACK đến sau timeout sẽ không complete request mới chỉ vì command giống nhau.

`request_id` là primary correlation identity.

Device không cần retry stale ACK vô hạn.

---

# 85. `device_event` không phải ACK

[CURRENT]

`device_event`:

```text
never completes pending command
```

Do đó không dùng event để thay ACK.

---

# 86. Long-running operation pattern

[DEVICE MUST]

Không chờ operation dài hoàn tất mới ACK nếu có nguy cơ >2s.

Pattern:

```text
Gateway
   │ device_command
   ▼
Device
   │ validate
   │ enqueue operation
   │
   ├── device_ack accepted
   │
   ▼
background job
   │
   ▼
device_event operation_complete
```

---

# 87. Example motor command

Request:

```text
device_command
command = move
request_id = 501
```

Device:

```text
validate target
schedule move
```

ACK:

```text
device_ack
command = move
request_id = 501
bool_value = true
```

Sau đó:

```text
device_event
command = move_complete
```

---

# 88. ABF1 callback architecture

[DEVICE MUST]

GATT write callback không làm business logic dài.

Correct:

```text
ABF1 WRITE
   ↓
validate length
   ↓
copy bytes
   ↓
enqueue
   ↓
return to NimBLE
```

Worker:

```text
dequeue
decode
validate
dispatch
ACK
```

---

# 89. ABF1 callback forbidden work

Không làm trực tiếp trong NimBLE callback:

```text
CBOR parsing phức tạp
sensor blocking read
motor wait
display render
NVS write
sleep/delay
wait semaphore lâu
wait ACK/event TX completion
```

---

# 90. Gateway follows same callback principle

[CURRENT]

Gateway NOTIFY callback:

```text
validate
copy mbuf
enqueue timeout=0
return
```

Worker mới decode CBOR và gọi application callback.

Device framework nên mirror nguyên tắc này.

---

# 91. Gateway notification queue

[CURRENT]

Queue depth:

```text
8
```

Queue đầy:

```text
drop newest
```

Gateway chấp nhận drop notification thay vì block NimBLE host.

---

# 92. Implication cho Device TX

[DEVICE SHOULD]

Không burst telemetry vô hạn.

Đặc biệt:

```text
ACK phải ưu tiên hơn telemetry
```

Nếu Device gửi 20 telemetry notifications rồi ACK, Gateway queue có thể drop ACK.

---

# 93. Recommended TX priority

```text
ACK
 >
critical event
 >
edge event
 >
state event
 >
telemetry
```

---

# 94. ACK queue reservation

[DEVICE SHOULD]

Framework nên đảm bảo ACK không bị telemetry chiếm hết queue.

Có thể dùng:

```text
separate ACK queue
```

hoặc:

```text
priority queue
```

hoặc:

```text
reserved capacity
```

Implementation tùy framework.

Behavior mới là contract.

---

# 95. Telemetry coalescing

[DEVICE SHOULD]

Các value dạng state:

```text
temperature
humidity
battery
RSSI-like metric
```

có thể coalesce latest value khi congested.

Không cần queue từng sample nếu Gateway chưa nhận kịp.

---

# 96. Event throttling

[DEVICE SHOULD]

Không publish heartbeat tốc độ cao.

Phase đầu Gateway notification queue nhỏ.

Target event rate phải conservative và benchmark trên multi-device connection.

---

# 97. Write Without Response implication

[CURRENT]

Gateway command transport dùng:

```text
Write Without Response
```

BLE ATT write success ở Gateway không có nghĩa product command đã execute.

ACK application-level là nguồn xác nhận.

---

# 98. Device must always ACK accepted commands

[DEVICE MUST]

Mỗi valid `device_command` mà Device xử lý phải tạo ACK:

```text
success
```

hoặc:

```text
failure
```

Không im lặng khi command bị product reject.

---

# 99. Unknown command

[DEVICE SHOULD]

Nếu CBOR/device_command hợp lệ nhưng command unknown:

```text
send device_ack
bool_value = false
echo request_id
echo command
echo device_id
```

Thay vì drop và bắt Gateway timeout.

---

# 100. Invalid CBOR

[DEVICE SHOULD]

Nếu không decode được request_id/device_id/command thì thường không thể ACK an toàn.

Log/drop.

Không crash.

Không mutate product state.

---

# 101. Duplicate command/request

Current Gateway normal dispatcher không chủ động retry same request ID sau timeout.

[DEVICE SHOULD]

Framework vẫn nên bảo vệ side effect commands nếu tương lai retransmission được thêm.

Phase đầu không cần persistent deduplication phức tạp.

---

# 102. Common commands đề xuất cho Device

[DEVICE SHOULD]

Mọi product support:

```text
ping
get_info
get_state
```

Đây là Device Framework convention, không phải Gateway hard-coded requirement.

Gateway dispatcher có thể gửi bất kỳ `device_command` name nào.

---

# 103. `ping`

Recommended behavior:

```text
request:
command = ping

ACK:
command = ping
bool_value = true
```

Không cần event.

---

# 104. `get_state`

Vì protocol v2 payload hạn chế, product-specific state mapping cần document.

Ví dụ relay:

```text
ACK bool_value = relay_on
```

ACK success semantics và product state cùng dùng bool có thể gây ambiguity.

Do đó tốt hơn:

```text
ACK bool_value = true
ACK int_value = state enum/value
```

nhưng product contract phải rõ.

---

# 105. ACK bool semantic collision

[CURRENT]

Gateway dispatcher hiểu:

```text
ACK bool_value == true
→ accepted/success

ACK bool_value == false
→ device rejected
```

Do đó:

```text
[DEVICE MUST NOT]
```

dùng ACK `bool_value` để trả product boolean state nếu `false` là state hợp lệ.

Ví dụ relay OFF không được trả:

```text
bool_value = false
```

nếu muốn Gateway hiểu command thành công.

---

# 106. State return trong protocol v2

[DEVICE SHOULD]

Với `get_state`, dùng:

```text
bool_value = true
```

để biểu thị ACK success.

Đặt state trong:

```text
int_value
```

Ví dụ:

```text
0 = OFF
1 = ON
```

Cho đến khi protocol schema có response payload riêng.

---

# 107. Device event bool semantics

`device_event` không bị dispatcher dùng `bool_value` làm success.

Có thể dùng bool cho event state nếu product contract quy định.

---

# 108. Gateway notification ingress

[CURRENT]

Gateway chỉ chấp nhận application message type từ Device:

```text
device_ack
device_event
```

Type khác:

```text
dropped
```

---

# 109. Malformed ACK

[CURRENT]

ACK bị drop nếu thiếu một trong:

```text
device_id
request_id
command
```

hoặc request_id zero.

Device sẽ khiến Gateway timeout.

---

# 110. Invalid protocol version

[CURRENT]

Notification bị drop nếu:

```text
version < 1
version > 2
```

New Device phải dùng v2.

---

# 111. Device event processing hiện tại

[CURRENT]

Gateway hiện log:

```text
device
command
int_value
```

cho `device_event`.

Dispatcher hiện chưa có full event routing/application subscription architecture.

[DEVICE SHOULD]

Vẫn thiết kế Device event API đúng abstraction để Gateway có thể mở rộng sau.

Không hard-code behavior dựa trên việc hiện tại Gateway chỉ log event.

---

# 112. Gateway reconnect scheduler prerequisites

[CURRENT]

Device được auto reconnect khi:

```text
configured runtime exists
reconnect enabled
device OFFLINE/BACKOFF
no connection slot
stored peer address exists
retry time reached
host ready
scan not active
```

---

# 113. Scan vs reconnect

[CURRENT]

Reconnect supervisor bỏ qua attempt khi scan đang active.

Device không cần biết chi tiết này.

Nhưng test automation phải tránh kết luận reconnect hỏng trong lúc Gateway đang scan liên tục.

---

# 114. One global CONNECTING policy

[CURRENT]

Gateway hiện chỉ cho:

```text
max 1 global connection attempt in CONNECTING
```

Các device khác chờ scheduler.

Device phải tiếp tục advertise đủ lâu, không chỉ burst ngắn vài giây.

---

# 115. Advertising duration

[DEVICE SHOULD]

Fixed-power devices:

```text
advertise indefinitely while disconnected
```

ít nhất trong Phase đầu.

Battery optimization làm sau khi integration stable.

---

# 116. Device reboot flow

Expected:

```text
Device reboot
   ↓
initialize storage/framework/product
   ↓
initialize NimBLE/GATT
   ↓
advertise 0xABF0
   ↓
Gateway stored identity reconnects
   ↓
security/bond
   ↓
discover/CCCD
   ↓
READY
```

---

# 117. Gateway reboot flow

Expected:

```text
Gateway reboot
   ↓
load Device Store
   ↓
BLE host sync
   ↓
reconnect supervisor
   ↓
connect known Device identity
   ↓
READY
```

Device phải vẫn advertising/connectable.

---

# 118. Bond persistence test

Required integration scenario:

```text
pair once
reboot Device
reconnect
reboot Gateway
reconnect
```

Không được yêu cầu factory reset sau mỗi reboot.

---

# 119. Bond mismatch recovery

Test:

```text
Gateway bond exists
Device bond cleared
```

và:

```text
Device bond exists
Gateway bond cleared
```

Repeat pairing/re-pair behavior phải được verify.

---

# 120. Factory reset physical path

[DEVICE SHOULD]

Có physical factory-reset mechanism.

Ví dụ:

```text
hold button 5–10 s
```

Flow:

```text
stop new product actions
ACK/indicator if applicable
clear resettable settings
clear bonds
clear claim state
restart
advertise
```

---

# 121. Device startup invariant

[DEVICE MUST]

Không advertise compatibility UUID cho tới khi Device thực sự có thể phục vụ contract.

Sai:

```text
NimBLE advertise ABF0
product init 5 seconds
command queue not ready
```

Đúng:

```text
storage
core
product
command registry
event pipeline
GATT server
advertise
```

---

# 122. Reference Device requirement

Trước product thật, `esp-ble-device` nên có:

```text
devices/reference_device
```

Golden Peripheral này là compatibility target cho Gateway.

---

# 123. Reference Device minimum hardware

```text
1 LED
1 button
```

---

# 124. Reference Device minimum commands

```text
ping
get_info
get_state
set_led
```

---

# 125. Reference Device minimum events

```text
button_pressed
state_changed
```

Optional:

```text
heartbeat
```

nhưng heartbeat không được gây notification burst.

---

# 126. Reference integration scenario

```text
1. boot Device
2. verify advertising contains ABF0
3. Gateway scan finds Device
4. add device with address/type
5. Gateway connects
6. security succeeds
7. GATT discovery succeeds
8. CCCD subscription succeeds
9. Gateway reports READY
10. send ping
11. receive matching ACK
12. send set_led
13. LED changes
14. receive matching ACK
15. press button
16. receive device_event
17. reboot Device
18. auto reconnect
19. reboot Gateway
20. auto reconnect
```

---

# 127. Gateway acceptance logs

Useful Gateway logs include:

```text
SECURING
DISCOVERING
READY
CMD_SEND
CMD_ACK
CMD_TIMEOUT
ACK_UNMATCHED
ACK_PROTOCOL_ERROR
DEVICE_EVENT
```

AI Agent should use these to localize integration failures.

---

# 128. Failure: Device không xuất hiện khi scan

Check:

```text
Is ABF0 in advertisement?
Is Device advertising?
Is Gateway scan active?
Is UUID encoded as 16-bit service UUID?
```

Không debug CBOR trước.

---

# 129. Failure: connect rồi disconnect ngay

Check:

```text
security compatibility
pairing/bond
GATT service exists
ABF1 WRITE_NO_RSP
ABF2 NOTIFY
CCCD exists
discovery timing
```

Gateway terminate khi contract discovery fail.

---

# 130. Failure: Gateway không READY

Check sequence:

```text
connect
security
service discovery
char discovery
descriptor discovery
CCCD write
```

READY chỉ sau CCCD write success.

---

# 131. Failure: command timeout

Check:

```text
Did ABF1 receive packet?
Did Device decode CBOR?
Did command handler run?
Did Device encode ACK?
Does ACK have device_id?
Does ACK have request_id?
Does request_id match?
Does command match?
Is bool_value true/false intentional?
Was ACK sent via ABF2?
Was CCCD enabled?
Was Gateway notify queue flooded?
```

---

# 132. Failure: ACK_UNMATCHED

Likely:

```text
wrong request_id
wrong device_id
stale ACK
missing field
```

---

# 133. Failure: ACK_PROTOCOL_ERROR

Likely:

```text
request_id matched
but command did not match
```

ACK must echo request command exactly.

---

# 134. Failure: event received nhưng pending command timeout

Expected if Device chỉ gửi:

```text
device_event
```

without:

```text
device_ack
```

Event không complete request.

---

# 135. Failure: notification decode error

Check CBOR:

```text
numeric keys
type present
command present
int_value present
bool_value present
valid version
length <= 256
strings under limits
request_id > 0 if present
```

---

# 136. Build responsibility separation

Gateway firmware và Device firmware là hai ESP-IDF project độc lập.

```text
esp-ble-gateway
```

không được build chung binary với:

```text
esp-ble-device
```

---

# 137. Protocol source architecture — current state

[CURRENT]

Protocol definitions đang nằm trong Gateway tại:

```text
components/cbor_codec
```

và BLE UUID constants nằm trong:

```text
components/ble_central
```

Đây là current implementation, chưa phải ideal final architecture.

---

# 138. Protocol source architecture — target state

[FUTURE / RECOMMENDED]

Tách shared:

```text
esp-gatt-protocol
```

để cả:

```text
esp-ble-gateway
esp-ble-device
```

dùng cùng source.

---

# 139. AI Agent rule trước khi shared repo tồn tại

Nếu `esp-gatt-protocol` chưa được tạo:

```text
DO NOT invent new protocol.
```

Implement Device theo contract trong tài liệu này/current Gateway code.

Tổ chức Device-side protocol component sao cho dễ thay bằng shared component sau.

---

# 140. AI Agent rule sau khi shared protocol tồn tại

Không duplicate:

```text
GW_PROTOCOL_VERSION
ABF0
ABF1
ABF2
CBOR key enum
gw_message_t
codec
```

Dùng dependency chung.

---

# 141. Firmware version vs protocol version

Không đồng nhất.

Example:

```text
Gateway FW 1.8.0
Protocol 2

Relay FW 1.2.1
Protocol 2
```

Hợp lệ.

---

# 142. ESP-IDF baseline

[CURRENT]

Gateway repository ghi baseline:

```text
ESP-IDF 5.4.4
```

[DEVICE SHOULD]

Dùng cùng baseline ở Phase đầu để giảm khác biệt NimBLE.

---

# 143. Device sdkconfig baseline

Recommended compatible baseline:

```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y

CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n

CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y

CONFIG_BT_NIMBLE_GATT_SERVER=y

CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1

CONFIG_BT_NIMBLE_SECURITY_ENABLE=y
CONFIG_BT_NIMBLE_SM_SC=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y

CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256
```

Exact symbols phải verify với target/ESP-IDF version.

---

# 144. Device chip

Gateway target hiện tại:

```text
ESP32-S3
```

Device không bắt buộc cùng chip.

Có thể:

```text
ESP32-C3
ESP32-C6
ESP32-S3
```

miễn NimBLE Peripheral behavior pass integration.

---

# 145. Device framework boundaries

Recommended:

```text
device_app
device_core
ble_peripheral
gateway_protocol
device_command
device_event
device_storage
board_io
product
drivers
```

---

# 146. `ble_peripheral` responsibility

```text
NimBLE
GAP
advertising
GATT server
security
bond
MTU
CCCD readiness
Notify transport
```

Không chứa relay/sensor business logic.

---

# 147. `device_command` responsibility

```text
RX queue
CBOR decode
validation
registry
dispatch
ACK construction
```

---

# 148. `device_event` responsibility

```text
event queue
priority
coalescing
CBOR encode
Notify scheduling
```

---

# 149. `device_app` responsibility

```text
composition
boot order
profile registration
framework startup
product startup
factory-reset coordination
lifecycle
```

Không gọi NimBLE low-level từ product.

---

# 150. Driver isolation

Correct:

```text
command handler
   ↓
relay_driver
```

Wrong:

```text
relay_driver
   ↓
ble_gatts_notify
```

---

# 151. Device command queue

Recommended:

```text
ABF1 callback
   ↓
bounded command queue
   ↓
command worker
```

Initial queue:

```text
4–8
```

Benchmark sau.

---

# 152. Device event queue

Recommended:

```text
product event
   ↓
bounded event queue
   ↓
TX worker
```

ACK cần priority.

---

# 153. No dynamic hot-path dependency

[DEVICE SHOULD]

Tránh malloc/free liên tục trong:

```text
ABF1 RX
ACK TX
high-frequency event TX
```

Ưu tiên fixed buffers/bounded queues.

---

# 154. Offline behavior

[DEVICE SHOULD]

Device local function không phụ thuộc Gateway connectivity nếu product không yêu cầu.

Example relay:

```text
local button vẫn có thể toggle
```

Nếu Gateway offline, telemetry có thể:

```text
drop
coalesce
```

Không ghi unbounded backlog vào flash.

---

# 155. Current Gateway event reliability

[CURRENT]

Notifications có thể bị drop nếu Gateway queue full.

Phase hiện tại không có guaranteed delivery cho `device_event`.

Device không được assume every event is persisted/acknowledged.

---

# 156. ACK reliability model

ACK cũng chạy qua Notify và có thể drop do congestion.

Vì vậy Device-side scheduling phải ưu tiên ACK để giảm xác suất timeout.

Protocol hiện chưa có explicit ACK-of-ACK.

---

# 157. Command idempotency

[DEVICE SHOULD]

Commands thay state nên được thiết kế idempotent khi có thể.

Ví dụ:

```text
set_state(true)
```

tốt hơn cho retry tương lai so với:

```text
toggle
```

`toggle` vẫn có thể support cho UX, nhưng side-effect retry semantics phức tạp hơn.

---

# 158. Product command naming

Use stable snake_case:

```text
set_state
get_state
set_interval
read_sensor
factory_reset
```

Tối đa 31 bytes.

Không rename tùy firmware minor version nếu không có compatibility plan.

---

# 159. Event naming

Stable snake_case:

```text
state_changed
button_pressed
temperature_mC
motion_detected
```

Tối đa 31 bytes.

---

# 160. Device type naming

Buffer max effective:

```text
15 bytes
```

Examples:

```text
relay
temp_sensor
motion
motor
```

---

# 161. Device ID naming

Max effective:

```text
31 bytes
```

Nên dùng ASCII simple identifiers ở Phase đầu:

```text
relay_01
sensor_03
```

Tránh Unicode/complex normalization trong identity.

---

# 162. Security ownership model

Gateway hiện biết bond/identity nhưng chưa định nghĩa full application ownership protocol.

[DEVICE SHOULD]

Framework có thể internal state:

```text
UNCLAIMED
CLAIMED
```

nhưng không invent wire commands bắt buộc nếu Gateway chưa support.

---

# 163. Device identity provisioning gap

Current Gateway contract cho phép user/add flow truyền logical `device_id` và BLE identity.

Current protocol không có một mandatory Device self-registration handshake.

AI Agent không được invent mandatory bootstrap handshake rồi yêu cầu Gateway hỗ trợ.

Nếu cần provisioning UX, thiết kế riêng và update Gateway contract sau.

---

# 164. Capability discovery gap

Current Gateway không yêu cầu:

```text
capability characteristic
Device Information Service
custom manifest characteristic
```

Không thêm dependency bắt buộc trong Phase đầu.

Có thể internal `get_info` command.

---

# 165. OTA gap

Current Device integration contract chưa có Device OTA protocol.

AI Agent không được đưa OTA vào critical path của initial connection.

---

# 166. Protocol v3 gap

Current project chưa có v3.

Không tự thêm:

```text
CBOR nested payload
float
string value
array payload
response object
```

vào wire v2 mà không cập nhật Gateway.

---

# 167. Additional GATT services

[DEVICE MAY]

Product có thể expose standard services khác nếu cần.

Nhưng Gateway compatibility không dựa trên chúng.

[DEVICE MUST]

Không thay thế ABF0/ABF1/ABF2 bằng custom service riêng cho product.

---

# 168. Additional properties

ABF1 có thêm WRITE property có thể vẫn pass nếu WRITE_NO_RSP tồn tại.

ABF2 có thêm properties có thể vẫn pass nếu NOTIFY tồn tại.

Nhưng minimum contract phải giữ.

---

# 169. Duplicate characteristics

[DEVICE SHOULD NOT]

Expose nhiều ABF1 hoặc ABF2 cùng service.

Gateway implementation không thiết kế để disambiguate multiple same UUID characteristics.

---

# 170. CCCD correctness

Device GATT server phải:

```text
accept 0x0001
track subscription state
allow Notify only khi subscribed
```

Device-side READY nên dựa trên subscribe callback/state.

---

# 171. Notify before subscription

[DEVICE MUST NOT]

Gửi normal ABF2 notification trước khi Gateway enable CCCD.

Có thể bị fail/drop và làm lifecycle khó debug.

---

# 172. MTU timing

Gateway starts MTU exchange ngay sau connect, song song trước/around security.

Device TX path phải handle MTU update asynchronously.

Không assume MTU finalized tại moment connect callback.

---

# 173. Security before application traffic

Gateway không mark READY trước encryption + discovery.

Device nên reject/defer application-level command nếu session chưa secure/ready.

Normal Gateway sẽ chưa gửi command trước READY.

---

# 174. Re-advertising after failed security

Nếu Gateway terminate do security failure:

```text
Device disconnect callback
  ↓
restart advertising
```

Không enter permanent stuck state.

---

# 175. Re-advertising after discovery failure

Tương tự:

```text
disconnect
  ↓
advertise
```

Sau khi firmware/config lỗi được sửa, Gateway reconnect có thể thành công.

---

# 176. Persistent product state

Product settings và BLE bond nên tách namespace/ownership.

Factory reset policy phải biết cái gì được xóa.

Không gọi `nvs_flash_erase()` mù quáng nếu có calibration/factory data.

---

# 177. Gateway delete_device

[CURRENT]

Khi delete Device:

```text
snapshot stored BLE identity
forget BLE peer / remove bond
delete Device Store entry
```

Nếu forget fail, Gateway giữ store entry để operation có thể retry.

Device không cần special packet cho delete.

---

# 178. Device reaction khi Gateway forgets

Nếu Gateway disconnects/forgets:

Device có thể vẫn giữ bond local.

Lần pairing sau có thể trigger repeat-pair recovery.

Physical Device factory reset vẫn cần cho ownership recovery/support.

---

# 179. Logging requirements phía Device

Recommended tags:

```text
device_app
ble_peripheral
device_command
device_event
device_storage
<product>
```

Important logs:

```text
ADV_START
CONNECTED
SECURING
SECURED
SUBSCRIBED
READY
CMD_RX
ACK_TX
EVENT_TX
DISCONNECTED
FACTORY_RESET
```

---

# 180. Correlation logs

Mỗi command log:

```text
device_id
request_id
command
```

Ví dụ:

```text
[CMD_RX] device=relay_01 request_id=142 command=set_state
[ACK_TX] device=relay_01 request_id=142 command=set_state result=ok
```

Giúp pair với Gateway:

```text
[CMD_SEND]
[CMD_ACK]
```

---

# 181. Never log secrets

Pairing keys/bond keys không log.

Raw CBOR chỉ debug level và bounded output nếu cần.

---

# 182. Integration test matrix

Minimum:

| Test | Expected |
|---|---|
| Advertisement contains ABF0 | Gateway discovers |
| Connect | success |
| Security | success <10s |
| Service discovery | ABF0 found |
| COMMAND property | WRITE_NO_RSP |
| STATUS property | NOTIFY |
| CCCD | found + write success |
| READY | Gateway reports connected |
| ping | matching ACK |
| unknown command | negative ACK preferred |
| device_event | received, does not complete command |
| Device reboot | auto reconnect |
| Gateway reboot | auto reconnect |
| bond persistence | reconnect without reset |
| factory reset | re-pair possible |
| malformed CBOR | Device remains stable |
| event burst | ACK still prioritized |

---

# 183. Protocol tests

Device codec compatibility tests phải verify:

```text
key 0 version
key 1 type
key 2 device_id
key 3 command
key 4 int_value
key 5 bool_value
key 10 request_id
```

Plus optional metadata keys.

---

# 184. Golden command vector

Input equivalent:

```text
{
  0: 2,
  1: "device_command",
  2: "ref_01",
  3: "ping",
  4: 0,
  5: false,
  10: 1
}
```

Expected ACK:

```text
{
  0: 2,
  1: "device_ack",
  2: "ref_01",
  3: "ping",
  4: 0,
  5: true,
  10: 1
}
```

---

# 185. Negative command vector

Input:

```text
{
  0: 2,
  1: "device_command",
  2: "ref_01",
  3: "unknown_xyz",
  4: 0,
  5: false,
  10: 2
}
```

Recommended ACK:

```text
{
  0: 2,
  1: "device_ack",
  2: "ref_01",
  3: "unknown_xyz",
  4: 0,
  5: false,
  10: 2
}
```

---

# 186. Mandatory decode fields test

Device → Gateway test vector missing:

```text
int_value
```

must be considered incompatible with current Gateway decoder.

Same for missing:

```text
bool_value
```

Agent must include both on every outgoing message.

---

# 187. Max length tests

Test:

```text
type 23 bytes → allowed
type 24 bytes → reject/avoid

device_id 31 → allowed
device_id 32 → reject/avoid

command 31 → allowed
command 32 → reject/avoid

device_type 15 → allowed
device_type 16 → reject/avoid
```

---

# 188. Message size test

Generate ACK/event near maximum.

Verify:

```text
CBOR <= 256
CBOR <= MTU - 3
```

No fragmentation layer currently defined at application protocol.

---

# 189. Command timing test

Measure:

```text
ABF1 write received
→ ABF2 ACK submitted
```

P50/P95/P99.

Target normal:

```text
P99 comfortably < 2000 ms
```

Recommended:

```text
P99 < 250–500 ms
```

depending product load.

---

# 190. Multi-device awareness test

Even Device firmware only has one link, Gateway may have 9.

Test Device with Gateway under multiple connected peripherals if possible.

Verify:

```text
ACK still under timeout
event rate acceptable
connection interval updates tolerated
```

---

# 191. Soak test

Recommended:

```text
8h minimum
24h preferred
```

Loop:

```text
ping
get_state
occasional product command
periodic event
random Device reboot
```

Track:

```text
disconnects
ACK timeouts
notify failures
heap
reset reason
```

---

# 192. Fault injection

Required cases:

```text
disconnect during command
disconnect before ACK
CCCD disabled
invalid request_id
unknown command
queue full
slow product handler
security failure
Device reset during pairing
Gateway reset during connection
```

---

# 193. AI Agent implementation order

AI Agent should implement in this order:

```text
1. protocol constants/types
2. Device GATT server
3. advertising ABF0
4. connect/disconnect lifecycle
5. security/bonding
6. CCCD/READY tracking
7. ABF1 bounded RX queue
8. CBOR v2 decode
9. command registry
10. ACK encode/TX priority
11. device_event pipeline
12. reference_device
13. Gateway integration
14. factory reset
15. first real product
```

Không bắt đầu bằng relay/sensor business feature.

---

# 194. Milestone A — Device visible

Pass when:

```text
Gateway scan sees Device
```

Only focus:

```text
advertising
ABF0
```

---

# 195. Milestone B — Device READY

Pass when Gateway logs READY.

Requires:

```text
connect
security
ABF0
ABF1
ABF2
CCCD
```

Không cần product feature.

---

# 196. Milestone C — ping ACK

Pass:

```text
Gateway sends ping
Device receives
Device ACKs with exact correlation
Gateway returns OK
```

Đây là milestone protocol quan trọng nhất.

---

# 197. Milestone D — event

Pass:

```text
button pressed
→ device_event
→ Gateway logs DEVICE_EVENT
```

---

# 198. Milestone E — reconnect

Pass:

```text
Device reboot
→ Gateway reconnects
→ READY
```

và:

```text
Gateway reboot
→ reconnect
→ READY
```

---

# 199. Milestone F — product

Chỉ sau Reference Device pass A–E mới thêm:

```text
relay
sensor
motor
```

---

# 200. Agent hard rules

AI Agent implementing `esp-ble-device` MUST follow:

```text
DO NOT modify UUIDs.

DO NOT invent new required GATT characteristics.

DO NOT use JSON over BLE.

DO NOT omit int_value/bool_value from Device TX CBOR.

DO NOT send ACK without device_id.

DO NOT send ACK without request_id.

DO NOT change request_id.

DO NOT change command name in ACK.

DO NOT use device_event as ACK.

DO NOT block NimBLE callback with product logic.

DO NOT assume MTU is always 256.

DO NOT advertise ABF0 before Device is ready.

DO NOT make product drivers depend on BLE.

DO NOT optimize for battery before basic integration passes.

DO NOT implement protocol v3 features as if Gateway supports them.
```

---

# 201. Agent preferred rules

```text
Use bounded queues.

Use fixed-size protocol structures.

Prefer static allocation in hot path.

Prioritize ACK.

Coalesce telemetry.

Use stable BLE identity.

Keep product code outside transport.

Make main.c minimal.

Keep Reference Device simple.

Add tests with each protocol behavior.
```

---

# 202. What Gateway expects from Device — compact contract

```text
DISCOVERY
  Advertise 0xABF0

CONNECTION
  Peripheral accepts Gateway connection

SECURITY
  compatible bonding + LE Secure Connections

GATT
  ABF0
    ABF1 WRITE_NO_RSP
    ABF2 NOTIFY + CCCD

PROTOCOL
  CBOR numeric map
  version 2

COMMAND RX
  type=device_command
  request_id generated by Gateway

ACK TX
  type=device_ack
  device_id required
  command exact echo
  request_id exact echo
  bool_value success/failure
  int_value present

EVENT TX
  type=device_event
  command=event name
  int_value present
  bool_value present

SIZE
  <= 256
  <= MTU-3

TIMING
  ACK < 2000ms

LIFECYCLE
  reconnectable
  re-advertise after disconnect
  stable identity preferred
```

---

# 203. Full end-to-end sequence diagram

```text
Device                                     Gateway
  │                                           │
  │ boot                                      │
  │ init product/framework                    │
  │ init GATT                                 │
  │                                           │
  │ ADV: service 0xABF0                       │
  │ ─────────────────────────────────────────►│ scan
  │                                           │
  │                                           │ add_device/store identity
  │                                           │
  │◄──────────────────────────────────────────│ CONNECT
  │                                           │
  │◄──────────────────────────────────────────│ Security initiate
  │──────── pairing/encryption ───────────────►│
  │                                           │
  │◄──────────────────────────────────────────│ Discover ABF0
  │──────────────── service ─────────────────►│
  │                                           │
  │◄──────────────────────────────────────────│ Discover chars
  │──────── ABF1 / ABF2 ─────────────────────►│
  │                                           │
  │◄──────────────────────────────────────────│ Discover CCCD
  │──────────────── 0x2902 ──────────────────►│
  │                                           │
  │◄──────────────────────────────────────────│ Write CCCD 0x0001
  │                                           │
  │                 READY                     │
  │                                           │
  │◄──────────────────────────────────────────│ ABF1 device_command
  │                                           │ request_id=142
  │ queue/decode/dispatch                      │
  │                                           │
  │ ABF2 device_ack                           │
  │ request_id=142                            │
  │ command exact echo                        │
  │ ─────────────────────────────────────────►│
  │                                           │ command completes
  │                                           │
  │ product event                             │
  │ ABF2 device_event                         │
  │ ─────────────────────────────────────────►│
  │                                           │
```

---

# 204. Architecture relation với tài liệu `esp-ble-device`

Tài liệu repository `esp-ble-device` mô tả:

```text
how to structure Device software
```

Tài liệu này mô tả:

```text
what Gateway requires from Device
```

Hai tài liệu phải được dùng cùng nhau.

Nếu kiến trúc Device đẹp nhưng vi phạm contract này, integration fail.

---

# 205. Architecture relation với `device_app`

`device_app` phải orchestrate để contract này ready trước advertising.

Expected boot:

```text
storage
 ↓
device_core
 ↓
product init
 ↓
command registry
 ↓
event pipeline
 ↓
ble_peripheral/GATT
 ↓
advertising
```

---

# 206. Proposed shared protocol extraction

[FUTURE]

Target:

```text
esp-gatt-protocol/
├── gw_protocol.h
├── gw_message.h
├── gw_ble_contract.h
├── gw_cbor_encode.c
├── gw_cbor_decode.c
└── validation
```

Khi component này tồn tại, tài liệu integration vẫn quan trọng cho lifecycle/timing/behavior ngoài schema.

---

# 207. Current vs target architecture

Current:

```text
Gateway owns protocol code
Device must mirror contract
```

Target:

```text
Gateway ─┐
         ├── same shared protocol component
Device ──┘
```

Không được trì hoãn Reference Device chỉ vì shared repo chưa hoàn thiện, nhưng không được tạo schema khác.

---

# 208. Definition of Done — Device/Gateway compatibility

Device được coi là compatible khi tất cả pass:

```text
[ ] advertises 0xABF0
[ ] Gateway scan sees it
[ ] stable address/identity can be stored
[ ] Gateway connects
[ ] security completes
[ ] bond survives reboot
[ ] ABF0 discovered
[ ] ABF1 discovered
[ ] ABF1 has WRITE_NO_RSP
[ ] ABF2 discovered
[ ] ABF2 has NOTIFY
[ ] CCCD discovered
[ ] CCCD write succeeds
[ ] Gateway enters READY
[ ] Device receives CBOR v2 command
[ ] Device parses request_id
[ ] ACK has protocol v2
[ ] ACK has device_id
[ ] ACK has exact request_id
[ ] ACK has exact command
[ ] ACK contains int_value
[ ] ACK contains bool_value
[ ] ACK arrives < 2s
[ ] Gateway marks success/rejected correctly
[ ] device_event arrives
[ ] device_event never substitutes ACK
[ ] payload respects 256 byte limit
[ ] payload respects MTU-3
[ ] Device re-advertises after disconnect
[ ] Device reboot reconnect passes
[ ] Gateway reboot reconnect passes
[ ] bond mismatch recovery tested
[ ] factory reset path tested
[ ] event burst does not starve ACK
```

---

# 209. Definition of Done — AI Agent implementation quality

```text
[ ] NimBLE callback bounded
[ ] no product logic in BLE transport
[ ] no BLE API in drivers
[ ] command queue bounded
[ ] event queue bounded
[ ] ACK priority explicit
[ ] telemetry backpressure explicit
[ ] state machine explicit
[ ] READY state explicit
[ ] MTU tracked runtime
[ ] security state tracked
[ ] CCCD state tracked
[ ] tests cover malformed CBOR
[ ] tests cover ACK correlation
[ ] tests cover reconnect
[ ] Reference Device exists
```

---

# 210. Known current limitations

AI Agent phải biết để không over-engineer hoặc hiểu sai:

```text
Gateway allows only 1 pending command/device.

Gateway ACK timeout is 2s.

Gateway notification queue depth is 8.

Gateway device_event handling is currently mostly logging/routing boundary.

Protocol v2 payload model is limited.

No application-level reliable event delivery.

No mandatory capability discovery.

No Device OTA wire protocol defined here.

No multi-Gateway Device ownership protocol defined here.

No protocol negotiation handshake beyond protocol_version decode.
```

---

# 211. Compatibility-safe future evolution

Khi cần mở rộng:

```text
structured sensor payload
OTA
capabilities
ownership
async command result
reliable events
```

phải update:

```text
Gateway
shared protocol
Device
integration tests
this contract
```

Không chỉ update Device.

---

# 212. Final instruction to AI Agent

Nếu bạn là AI Agent đang xây `esp-ble-device`, mục tiêu đầu tiên không phải:

```text
build a relay
```

Mục tiêu đầu tiên là:

```text
build a Golden Peripheral that the current Gateway can discover,
secure, discover, subscribe, command, ACK and reconnect reliably.
```

Chỉ khi Golden Peripheral đạt:

```text
READY
+
ping ACK
+
event
+
reconnect
```

mới bắt đầu product-specific firmware.

---

# 213. Final integration target

```text
                         ESP-GATT
                            │
               ┌────────────┴────────────┐
               │                         │
               ▼                         ▼
       esp-ble-gateway             esp-ble-device
       Central / Client            Peripheral / Server
               │                         │
               │       BLE v2            │
               ├─────────────────────────┤
               │                         │
               │ ABF1 device_command     │
               ├────────────────────────►│
               │                         │
               │ ABF2 device_ack/event   │
               │◄────────────────────────┤
               │                         │
               └────────── READY ────────┘
```

The compatibility contract is:

```text
0xABF0
0xABF1 WRITE_NO_RSP
0xABF2 NOTIFY + CCCD
security + bonding
CBOR protocol v2
exact ACK correlation
bounded message size
reconnectable stable Device
```

Đây là baseline mà `esp-ble-device` phải implement trước khi mở rộng tính năng.
