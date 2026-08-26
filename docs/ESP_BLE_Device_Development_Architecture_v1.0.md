# ESP-BLE-Device — Development Architecture & Implementation Plan

**Phiên bản:** 1.0  
**Ngày:** 26/08/2026  
**Project:** `esp-ble-device`  
**Vai trò:** Device Framework + Product Firmware cho hệ thống ESP-GATT  
**Framework:** ESP-IDF native + NimBLE  
**Protocol baseline:** ESP-GATT Protocol v2  
**BLE role:** Peripheral / GATT Server  

---

# 1. Mục tiêu tài liệu

Tài liệu này định nghĩa kiến trúc, cấu trúc source code, component boundaries, build model, test strategy, release model và roadmap triển khai cho repository:

```text
esp-ble-device
```

Repository này không phải một firmware duy nhất.

Nó là:

```text
Device Framework
      +
Reusable Components
      +
Product Firmware Projects
      +
Reference Device
```

Mục tiêu chính:

1. Dùng chung một framework cho nhiều loại thiết bị BLE.
2. Không copy BLE/CBOR/command code giữa các product.
3. Mỗi product firmware build độc lập.
4. Mỗi product có thể target chip ESP32 khác nhau.
5. Toàn bộ Device sử dụng cùng protocol contract với Gateway.
6. `device_app` chỉ làm composition/orchestration.
7. BLE transport độc lập với business logic.
8. Command/event pipeline có timing và concurrency rõ ràng.
9. Có Reference Device làm Golden Peripheral.
10. Có Definition of Done cho framework và từng product.

---

# 2. Vai trò của esp-ble-device

`esp-ble-device` chịu trách nhiệm firmware phía BLE Peripheral.

Kiến trúc hệ thống:

```text
          ESP32 Gateway
          BLE Central
               │
               │ BLE
               ▼
       ESP BLE Device
       Peripheral / GATT Server
               │
               ▼
        Product Application
```

Gateway thực hiện:

```text
scan
connect
security
discover
write command
subscribe notify
```

Device thực hiện:

```text
advertise
accept connection
security
serve GATT
receive command
execute command
send ACK
publish event
```

---

# 3. Những gì esp-ble-device KHÔNG chịu trách nhiệm

Không đặt vào repository này:

```text
Wi-Fi Gateway
Web UI Gateway
MCP endpoint
Gateway Device Store
Gateway Command Dispatcher
BLE Central connection pool
multi-device scheduler
```

Device Framework chỉ xử lý phía Peripheral.

---

# 4. Protocol contract bắt buộc

Mọi compatible Device phải implement:

```text
Primary Service
0xABF0
```

Characteristics:

```text
COMMAND
UUID 0xABF1
Gateway → Device
Write Without Response
```

```text
STATUS
UUID 0xABF2
Device → Gateway
Notify
CCCD 0x2902
```

Protocol:

```text
GW_PROTOCOL_VERSION = 2
```

Message types:

```text
device_command
device_ack
device_event
```

`gateway_command` không được route vào Device application.

---

# 5. Repository structure

Cấu trúc đề xuất:

```text
esp-ble-device/
├── CMakeLists.txt
├── README.md
├── AGENTS.md
├── .gitignore
├── .gitmodules
│
├── components/
│   ├── device_app/
│   ├── device_core/
│   ├── ble_peripheral/
│   ├── device_command/
│   ├── device_event/
│   ├── device_storage/
│   ├── board_io/
│   ├── device_health/
│   ├── device_factory_reset/
│   │
│   └── gateway_protocol/
│       └── shared submodule/component
│
├── devices/
│   ├── reference_device/
│   ├── relay_1ch/
│   ├── relay_4ch/
│   ├── temperature_sensor/
│   ├── motion_sensor/
│   └── ...
│
├── docs/
│   ├── ESP_BLE_Device_Development_Architecture_v1.0.md
│   ├── Device_App_Development_Spec_v1.0.md
│   ├── Device_Gateway_Protocol_v2.md
│   └── product/
│
├── test/
│   ├── framework_test_app/
│   └── integration/
│
└── tools/
    ├── build_device.sh
    ├── flash_device.sh
    ├── collect_artifacts.sh
    └── integration_test.sh
```

---

# 6. Kiến trúc component tổng thể

```text
                    app_main
                       │
                       ▼
                  device_app
                       │
       ┌───────────────┼────────────────┐
       │               │                │
       ▼               ▼                ▼
 device_core     device_command     device_event
       │               │                │
       │               │                │
       ├───────────────┼────────────────┤
       │               │                │
       ▼               ▼                ▼
device_storage   gateway_protocol   ble_peripheral
       │                                │
       ▼                                ▼
 board_io / drivers                 NimBLE Host
```

Product-specific layer:

```text
product_profile
product_commands
product_events
product_drivers
```

được đăng ký thông qua `device_app`.

---

# 7. Dependency direction

Dependency đúng:

```text
main
 ↓
device_app
 ↓
framework components
 ↓
gateway_protocol
 ↓
ESP-IDF/NimBLE
```

Product:

```text
product
 ↓
device_core
device_command
device_event
drivers
```

Không cho phép dependency ngược.

---

# 8. Forbidden dependencies

Không cho phép:

```text
gateway_protocol → device_app
gateway_protocol → ble_peripheral
relay_driver → ble_peripheral
sensor_driver → gateway_protocol
board_io → device_command
ble_peripheral → relay_driver
```

Driver không biết BLE.

Protocol không biết product.

Transport không biết business logic.

---

# 9. device_app

`device_app` là composition root.

Trách nhiệm:

```text
load profile
initialize components
register commands
register event sources
start BLE
manage high-level lifecycle
coordinate factory reset
expose device status
```

Không làm:

```text
NimBLE API directly
CBOR encoding directly
GPIO driver logic
sensor reading
command execution inside BLE callback
NVS low-level implementation
```

---

# 10. device_app public API baseline

Đề xuất:

```c
typedef enum {
    DEVICE_APP_OK = 0,
    DEVICE_APP_ERR_INVALID_ARG,
    DEVICE_APP_ERR_INVALID_STATE,
    DEVICE_APP_ERR_STORAGE,
    DEVICE_APP_ERR_PROTOCOL,
    DEVICE_APP_ERR_COMMAND,
    DEVICE_APP_ERR_EVENT,
    DEVICE_APP_ERR_BLE,
    DEVICE_APP_ERR_PRODUCT,
    DEVICE_APP_ERR_NO_RESOURCE,
} device_app_result_t;
```

API:

```c
device_app_result_t device_app_start(void);
device_app_result_t device_app_stop(void);

device_app_result_t device_app_get_status(
    device_app_status_t *out_status);

device_app_result_t device_app_factory_reset(void);
```

---

# 11. device_app profile

Mỗi product cung cấp:

```c
typedef struct {
    const char *model;
    const char *device_type;

    const char *hardware_version;
    const char *firmware_version;

    const char *ble_name_prefix;

    uint8_t protocol_version;

    bool supports_factory_reset;
    bool supports_telemetry;
    bool supports_local_button;

    device_product_init_fn_t product_init;
    device_product_start_fn_t product_start;
    device_product_stop_fn_t product_stop;

    device_product_register_commands_fn_t register_commands;
    device_product_register_events_fn_t register_events;
} device_app_profile_t;
```

---

# 12. device_core

`device_core` sở hữu trạng thái logic chung của Device.

Ví dụ:

```c
typedef enum {
    DEVICE_STATE_BOOTING = 0,
    DEVICE_STATE_ADVERTISING,
    DEVICE_STATE_CONNECTED,
    DEVICE_STATE_SECURING,
    DEVICE_STATE_READY,
    DEVICE_STATE_DEGRADED,
    DEVICE_STATE_RESETTING,
    DEVICE_STATE_STOPPED,
} device_state_t;
```

Sở hữu:

```text
device logical state
connection readiness mirror
product metadata
uptime
health summary
ownership state
```

Không sở hữu GATT handles.

---

# 13. BLE Peripheral component

Component:

```text
components/ble_peripheral/
```

Trách nhiệm:

```text
NimBLE initialization
GAP Peripheral role
advertising
GATT Server
security
bonding
connection state
CCCD tracking
MTU tracking
RX callback
Notify TX
re-advertise
```

---

# 14. ble_peripheral internal structure

Đề xuất:

```text
ble_peripheral/
├── CMakeLists.txt
├── Kconfig
├── README.md
│
├── include/
│   └── ble_peripheral.h
│
├── ble_peripheral.c
├── ble_peripheral_gap.c
├── ble_peripheral_gatt.c
├── ble_peripheral_security.c
├── ble_peripheral_adv.c
├── ble_peripheral_notify.c
├── ble_peripheral_state.c
└── ble_peripheral_internal.h
```

---

# 15. BLE public API

Baseline:

```c
typedef void (*ble_peripheral_rx_cb_t)(
    const uint8_t *data,
    size_t len);

typedef void (*ble_peripheral_state_cb_t)(
    ble_peripheral_state_t state);
```

API:

```c
int ble_peripheral_init(
    const ble_peripheral_config_t *config,
    ble_peripheral_rx_cb_t rx_cb,
    ble_peripheral_state_cb_t state_cb);

int ble_peripheral_start(void);
int ble_peripheral_stop(void);

int ble_peripheral_notify(
    const uint8_t *data,
    size_t len);

bool ble_peripheral_is_connected(void);
bool ble_peripheral_is_ready(void);

uint16_t ble_peripheral_get_mtu(void);

int ble_peripheral_clear_bonds(void);
```

---

# 16. BLE state machine

```text
STOPPED
   │
   ▼
INITIALIZED
   │
   ▼
ADVERTISING
   │
   │ connect
   ▼
CONNECTED
   │
   ▼
SECURING
   │
   │ security OK
   ▼
WAIT_CCCD
   │
   │ STATUS notify enabled
   ▼
READY
   │
   │ disconnect
   ▼
ADVERTISING
```

Device không publish normal event trước READY.

---

# 17. Advertising

Bắt buộc advertise:

```text
Service UUID 0xABF0
```

Recommended packet:

```text
Flags
Complete 16-bit Service UUID list
  └── 0xABF0
Short/Complete Local Name
```

Optional:

```text
Manufacturer Data
```

Không dùng BLE name để Gateway xác định compatibility.

---

# 18. GATT table

```text
Primary Service 0xABF0
│
├── COMMAND
│   UUID 0xABF1
│   Property: Write Without Response
│
└── STATUS
    UUID 0xABF2
    Property: Notify
    Descriptor:
      CCCD 0x2902
```

Không tạo product-specific characteristic trong Phase đầu.

---

# 19. Security baseline

Device phải support:

```text
bonding
LE Secure Connections
NVS bond persistence
NO_IO
```

Device ownership model:

```text
UNCLAIMED
CLAIMED
```

Factory reset có thể quay về:

```text
UNCLAIMED
```

---

# 20. Stable BLE identity

Device phải dùng BLE identity ổn định.

Không dùng random address thay đổi tùy reboot nếu không có identity resolution thích hợp.

Khuyến nghị:

```text
public identity
```

hoặc:

```text
random static identity persisted
```

---

# 21. gateway_protocol component

`gateway_protocol` là shared dependency.

Repository/project Device không định nghĩa riêng:

```text
GW_PROTOCOL_VERSION
GW_BLE_SERVICE_UUID
GW_BLE_COMMAND_UUID
GW_BLE_STATUS_UUID
gw_message_t
CBOR numeric keys
```

Tất cả lấy từ shared component.

---

# 22. Protocol message baseline

```c
typedef struct {
    uint8_t protocol_version;

    char type[GW_MSG_TYPE_LEN];

    char device_id[GW_MSG_DEVICE_ID_LEN];
    char command[GW_MSG_COMMAND_LEN];

    uint32_t request_id;
    int has_request_id;

    int int_value;
    int bool_value;

    int has_device_id;

    char name[GW_MSG_NAME_LEN];
    char device_type[GW_MSG_DEVICE_TYPE_LEN];

    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
    int has_ble_addr;
} gw_message_t;
```

Device không nhất thiết sử dụng mọi field.

---

# 23. device_command component

Trách nhiệm:

```text
RX queue
CBOR decode
message validation
command registry
command dispatch
ACK generation
long-running command scheduling boundary
```

Không nhận raw NimBLE callback execution.

---

# 24. Command pipeline

```text
ABF1 Write
    │
    ▼
NimBLE callback
    │
    ├── validate size
    ├── copy bytes
    └── enqueue
          │
          ▼
device_command worker
          │
          ├── CBOR decode
          ├── validate
          ├── lookup handler
          ├── execute/accept
          └── generate ACK
```

---

# 25. NimBLE callback rule

NimBLE callback KHÔNG được:

```text
decode CBOR
read sensor
write display
move motor
sleep
wait mutex lâu
write NVS
wait command completion
```

Callback chỉ làm bounded work.

---

# 26. Command registry

API đề xuất:

```c
typedef device_command_result_t (*device_command_handler_t)(
    const gw_message_t *request,
    device_command_response_t *response);

int device_command_register(
    const char *command,
    device_command_handler_t handler);
```

Lifecycle:

```text
init
 ↓
register common commands
 ↓
register product commands
 ↓
freeze registry
 ↓
start BLE
```

Sau freeze không register thêm.

---

# 27. Common commands

Mọi Device nên support tối thiểu:

```text
ping
get_info
get_state
```

Optional:

```text
factory_reset
reboot
```

Các destructive command cần policy riêng.

---

# 28. Product commands

Relay:

```text
set_state
toggle
get_state
```

Sensor:

```text
read
set_interval
get_state
```

Motor:

```text
start
stop
set_speed
get_state
```

Không hard-code product command trong framework core.

---

# 29. ACK contract

Gateway command:

```text
type = device_command
request_id = X
command = Y
device_id = Z
```

Device ACK phải echo:

```text
type = device_ack
request_id = X
command = Y
device_id = Z
```

Success:

```text
bool_value = true
```

Failure:

```text
bool_value = false
```

---

# 30. ACK timing

Gateway hiện chờ khoảng:

```text
2000 ms
```

Device phải đảm bảo:

```text
ACK priority > telemetry
```

Khuyến nghị target:

```text
normal ACK < 250 ms
```

under normal load.

Hard requirement:

```text
ACK < Gateway timeout
```

---

# 31. Long-running commands

Không block ACK.

Pattern:

```text
command received
     │
     ▼
validate
     │
     ▼
schedule job
     │
     ├── ACK accepted
     │
     ▼
background operation
     │
     ▼
device_event complete
```

Ví dụ:

```text
calibrate
motor_move
sensor_scan
```

---

# 32. device_event component

Trách nhiệm:

```text
event queue
event encode
priority
coalescing
Notify submission
```

Không trực tiếp manipulate GATT handles.

---

# 33. Event classes

Đề xuất:

```text
ACK
CRITICAL_EVENT
EDGE_EVENT
STATE_EVENT
TELEMETRY
```

Priority:

```text
ACK
 >
CRITICAL_EVENT
 >
EDGE_EVENT
 >
STATE_EVENT
 >
TELEMETRY
```

---

# 34. Telemetry coalescing

Telemetry như:

```text
temperature
humidity
battery
signal level
```

nên coalesce latest value nếu queue pressure cao.

Không cần giữ mọi sample.

---

# 35. Edge events

Ví dụ:

```text
button_pressed
door_open
alarm_triggered
motion_detected
```

Có thể cần queue riêng/bounded retention.

Phase đầu chưa cần guaranteed delivery application protocol.

---

# 36. device_storage

Trách nhiệm:

```text
runtime user settings
claim/ownership state
product settings
factory-resettable config
schema version
```

Không dùng storage component làm bond database của NimBLE nếu stack đã quản lý riêng.

---

# 37. NVS namespace strategy

Đề xuất:

```text
dev_core
dev_app
product
board_io
```

Không dùng một namespace chung chứa mọi thứ.

Ví dụ:

```text
dev_core:
  schema
  claimed
  logical_id
```

```text
product:
  sample_interval
  relay_boot_state
```

---

# 38. Storage schema version

Mỗi namespace cần version nếu cấu trúc có thể thay đổi.

Ví dụ:

```c
#define DEVICE_STORAGE_SCHEMA_VERSION 1
```

Migration phải explicit.

Không tự xóa NVS khi schema mismatch trừ khi policy cho phép.

---

# 39. Factory reset

Factory reset không đồng nghĩa:

```c
nvs_flash_erase();
```

Factory reset đúng:

```text
stop product actions
disable/stop BLE
clear resettable product settings
clear claim state
clear bonds
preserve factory calibration if required
restart
```

---

# 40. Physical factory reset

Khuyến nghị:

```text
hold reset/config button 5–10 seconds
```

Flow:

```text
button hold
  ↓
confirm pattern
  ↓
factory reset
  ↓
restart
  ↓
advertise
```

---

# 41. board_io

Common component cho:

```text
status LED
reset button
factory-reset button
local action button
display status hook
```

`board_io` không chứa product business logic.

Nó chỉ phát event/hook.

---

# 42. Status LED baseline

Đề xuất:

```text
BOOTING       fast blink
ADVERTISING   slow blink
CONNECTED     medium blink
READY         solid/on pattern
DEGRADED      error pattern
RESETTING     distinct pattern
```

Product có thể override mapping.

---

# 43. device_health

Optional common component.

Metrics:

```text
uptime
free heap
minimum free heap
command rx count
command decode errors
ACK count
event sent
event dropped
BLE connects
BLE disconnects
security failures
notify failures
```

Không cần expose tất cả qua protocol v2 ngay lập tức.

---

# 44. Concurrency model

Recommended tasks:

```text
NimBLE Host Task
device_command_task
device_event/notify_task
product-specific task(s)
optional health task
```

Không cần tạo task cho mọi component.

---

# 45. Queue ownership

Example:

```text
ble RX
  ↓
command_rx_queue
  ↓
command task
```

```text
product
  ↓
event queue
  ↓
event task
  ↓
BLE notify
```

---

# 46. Locking rules

Không giữ framework mutex khi:

```text
calling NimBLE API
calling product callback
writing NVS
waiting queue/semaphore
```

Nếu cần snapshot:

```text
lock
copy state
unlock
call external subsystem
```

---

# 47. Message size

Shared protocol baseline:

```text
GW_MSG_MAX_LEN = 256
```

BLE payload phải thỏa:

```text
encoded_len <= negotiated_mtu - 3
```

Device Notify phải check MTU trước send.

---

# 48. MTU

Preferred MTU:

```text
256
```

Nhưng runtime không được assume negotiated MTU luôn 256.

Dùng negotiated value.

---

# 49. Device product model

Mỗi product project có:

```text
main
profile
commands
events
drivers
sdkconfig
```

Framework common nằm ngoài product.

---

# 50. Product project example

```text
devices/relay_1ch/
├── CMakeLists.txt
├── sdkconfig.defaults
├── sdkconfig.defaults.esp32c3
│
├── main/
│   ├── CMakeLists.txt
│   └── main.c
│
├── product/
│   ├── CMakeLists.txt
│   ├── relay_profile.c
│   ├── relay_commands.c
│   ├── relay_events.c
│   └── include/
│       └── relay_product.h
│
└── drivers/
    └── relay_driver/
```

---

# 51. Product main.c

```c
#include "device_app.h"
#include "relay_product.h"

void app_main(void)
{
    device_app_set_profile(relay_product_profile());
    device_app_start();
}
```

Có thể đơn giản hơn nếu profile được link-time inject.

---

# 52. Product profile ownership

Profile phải là static lifetime.

Không trả pointer tới stack object.

Ví dụ:

```c
const device_app_profile_t *relay_product_profile(void)
{
    static const device_app_profile_t profile = {
        ...
    };

    return &profile;
}
```

---

# 53. Reference Device

Project bắt buộc:

```text
devices/reference_device/
```

Vai trò:

```text
Golden Peripheral
Protocol validator
Gateway regression peer
Framework example
Hardware integration target
```

---

# 54. Reference Device hardware

Minimum:

```text
1 LED
1 button
```

Không cần sensor phức tạp.

---

# 55. Reference Device commands

```text
ping
get_info
get_state
set_led
```

---

# 56. Reference Device events

```text
button_pressed
state_changed
heartbeat
```

Heartbeat optional.

---

# 57. Reference Device Definition of Done

```text
[ ] advertise ABF0
[ ] connect from Gateway
[ ] security/bond
[ ] CCCD enable
[ ] READY
[ ] ping ACK
[ ] set_led ACK
[ ] get_state ACK
[ ] button event
[ ] reboot reconnect
[ ] Gateway reboot reconnect
[ ] factory reset clears bond
```

---

# 58. ESP-IDF project model

Mỗi `devices/<product>` là ESP-IDF project riêng.

Ví dụ:

```bash
cd devices/reference_device
idf.py set-target esp32s3
idf.py build
```

Relay:

```bash
cd devices/relay_1ch
idf.py set-target esp32c3
idf.py build
```

Không build tất cả thông qua một generated sdkconfig chung.

---

# 59. Top-level CMakeLists.txt của product

Ví dụ:

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components"
    "product"
    "drivers"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(relay_1ch)
```

---

# 60. Product main/CMakeLists.txt

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_app
        relay_product
)
```

---

# 61. Component discovery

`EXTRA_COMPONENT_DIRS` phải làm cho product thấy:

```text
../../components
product
drivers
```

Không symlink thủ công từng common component vào product.

---

# 62. sdkconfig baseline

Device baseline:

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

---

# 63. Target-specific sdkconfig

Shared:

```text
sdkconfig.defaults
```

Chip-specific:

```text
sdkconfig.defaults.esp32c3
sdkconfig.defaults.esp32s3
sdkconfig.defaults.esp32c6
```

Không commit generated `sdkconfig` trừ policy riêng.

---

# 64. Build directory

Mỗi product:

```text
devices/relay_1ch/build/
devices/temp_sensor/build/
devices/reference_device/build/
```

Không dùng:

```text
esp-ble-device/build/
```

cho tất cả product.

---

# 65. Build command

Reference:

```bash
cd devices/reference_device

git submodule update --init --recursive

idf.py set-target esp32s3
idf.py build
```

Relay:

```bash
cd devices/relay_1ch

idf.py set-target esp32c3
idf.py build
```

---

# 66. Flash

```bash
idf.py -p <PORT> flash monitor
```

Ví dụ:

```bash
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

---

# 67. Build helper

Có thể có:

```bash
./tools/build_device.sh relay_1ch
```

Nhưng helper phải gọi ESP-IDF project đúng.

Không tự merge sdkconfig.

---

# 68. build_device.sh behavior

Expected:

```text
input product name
resolve devices/<name>
validate project
read target metadata
run idf.py
return build result
```

Không modify source.

---

# 69. Product target metadata

Có thể tạo:

```text
devices/relay_1ch/product.json
```

Ví dụ:

```json
{
  "name": "relay_1ch",
  "target": "esp32c3",
  "firmware_version": "1.0.0",
  "protocol_version": 2
}
```

Helper build dùng metadata này.

---

# 70. Firmware version

Mỗi product version độc lập.

Ví dụ:

```text
reference_device 1.0.0
relay_1ch        1.2.0
temp_sensor      2.1.3
```

Protocol:

```text
v2
```

có thể giống nhau.

---

# 71. Firmware version source of truth

Khuyến nghị dùng:

```text
project version
```

qua:

```text
PROJECT_VER
```

hoặc file version riêng.

Không duplicate nhiều chỗ.

---

# 72. Protocol version source of truth

Chỉ:

```text
gateway_protocol
```

Không set:

```c
#define DEVICE_PROTOCOL_VERSION 2
```

trong từng product nếu đã có:

```c
GW_PROTOCOL_VERSION
```

---

# 73. Test architecture

Có ba lớp:

```text
Unit Test
Integration Test
Hardware/System Test
```

---

# 74. Unit tests

Test common components:

```text
gateway_protocol
device_command
device_event
device_storage
device_core
factory_reset policy
```

---

# 75. Framework test app

Tạo:

```text
test/framework_test_app/
```

ESP-IDF project riêng.

Nó build common components với mock/test product.

---

# 76. device_command tests

Bắt buộc:

```text
valid command
unknown command
duplicate registration
registry freeze
invalid protocol version
missing request_id
missing device_id
oversize command
ACK echo request_id
ACK echo command
handler error
```

---

# 77. device_event tests

Bắt buộc:

```text
ACK priority
event priority
telemetry coalescing
queue full
drop policy
encode failure
not-ready behavior
```

---

# 78. BLE unit/integration boundary

Các behavior sau cần hardware:

```text
advertising
real connection
pairing
bond persistence
MTU negotiation
CCCD
Notify
disconnect/reconnect
```

Không giả vờ unit-test chúng hoàn toàn bằng mock.

---

# 79. Gateway integration test

Hardware:

```text
Gateway board
Reference Device board
```

Test:

```text
scan
connect
bond
discover
subscribe
command
ACK
event
reconnect
reset
```

---

# 80. Fault tests

Bắt buộc:

```text
invalid CBOR
oversize packet
unknown command
duplicate request
malformed request_id
Notify before CCCD
disconnect during command
Device reboot
Gateway reboot
bond mismatch
queue saturation
slow product handler
low heap
```

---

# 81. Soak test

Recommended:

```text
8h minimum
24h preferred
```

Test:

```text
periodic command
periodic telemetry
random reconnect
Device reboot cycle
```

Track:

```text
heap drift
queue drops
disconnect count
ACK timeout
reset reason
```

---

# 82. Logging policy

Tags:

```text
device_app
device_core
ble_peripheral
device_command
device_event
device_storage
board_io
<product>
```

Không log raw payload liên tục ở INFO.

Debug only.

---

# 83. Error severity

Đề xuất:

```text
FATAL
DEGRADED
RECOVERABLE
PROTOCOL
PRODUCT
```

---

# 84. Fatal boot errors

Ví dụ:

```text
NVS init fatal
protocol init fail
command registry fail
GATT init fail
product init fail
```

Policy:

```text
do not advertise
enter degraded/error state
```

Không advertise một Device chưa sẵn sàng xử lý command.

---

# 85. Degraded errors

Ví dụ:

```text
optional sensor unavailable
display unavailable
telemetry queue issue
```

Có thể vẫn advertise nếu core function còn đúng.

Profile/product quyết định.

---

# 86. Boot ordering

Recommended:

```text
1. basic logging
2. NVS init
3. device_storage init
4. device_core init
5. board_io init
6. product init
7. gateway_protocol init/check
8. device_command init
9. register common commands
10. register product commands
11. freeze command registry
12. device_event init
13. product event registration
14. ble_peripheral init
15. start product
16. start advertising
```

---

# 87. Critical invariant

Không start BLE advertising trước:

```text
command registry ready
product ready
event pipeline ready
```

Nếu Gateway connect ngay sau advertisement, Device phải xử lý command hợp lệ.

---

# 88. Stop ordering

```text
stop advertising
reject new commands
stop product operations
flush/disable event publishing
disconnect if needed
stop BLE
stop workers
persist required state
```

---

# 89. Reboot command policy

Nếu support:

```text
command = reboot
```

Device nên:

```text
ACK first
delay short bounded interval
restart
```

Không reboot trước ACK.

---

# 90. Factory reset command policy

Nếu remote factory reset được phép:

```text
validate command
ACK accepted
schedule reset
clear resettable state
clear bonds
restart
```

Nên có security/ownership restriction.

---

# 91. Product driver rules

Driver API:

```text
init
start
stop
read/write
get state
```

Driver không emit BLE event trực tiếp.

Driver trả data cho product layer.

---

# 92. Product event flow

Đúng:

```text
driver
  ↓
product logic
  ↓
device_event_publish()
  ↓
event worker
  ↓
gateway_protocol encode
  ↓
ble_peripheral_notify()
```

Sai:

```text
driver → ble_peripheral_notify()
```

---

# 93. Product command flow

Đúng:

```text
device_command
  ↓
product command handler
  ↓
driver
  ↓
result
  ↓
ACK
```

---

# 94. Device identity design

Phân biệt:

```text
model
device_type
logical device_id
BLE identity
```

`model`:

```text
relay-1ch-v1
```

`device_type`:

```text
relay
```

`device_id`:

```text
logical installation identity
```

BLE address:

```text
transport identity
```

---

# 95. Device ID generation

Tài liệu này không bắt buộc một strategy duy nhất.

Có thể:

```text
provisioned ID
factory-generated ID
MAC-derived display ID
NVS persistent logical ID
```

Nhưng Gateway không nên phụ thuộc BLE name.

---

# 96. get_info

Framework nên chuẩn bị internal data:

```text
model
device_type
fw_version
hw_version
protocol_version
uptime
capabilities
```

Protocol v2 hiện chưa tối ưu cho structured info lớn.

Phase đầu có thể trả subset.

---

# 97. Capability model

Có thể chuẩn bị local structure:

```c
typedef struct {
    bool supports_factory_reset;
    bool supports_telemetry;
    bool supports_local_button;
    bool supports_battery;
    bool supports_config;
} device_capabilities_t;
```

Không bắt buộc expose toàn bộ trên wire v2.

---

# 98. Memory policy

Common framework cần tránh:

```text
large dynamic allocation
unbounded queue
unbounded string
malloc/free in hot BLE path
```

Ưu tiên:

```text
static bounded structures
FreeRTOS queues
copy-out APIs
fixed-size messages
```

---

# 99. Queue sizing baseline

Initial suggestion:

```text
command RX queue: 4–8
ACK/event TX queue: 8–16
critical event queue: small bounded
```

Số cụ thể phải benchmark.

Không coi đây là API contract.

---

# 100. Backpressure

Khi TX queue full:

```text
ACK:
  reserve capacity / high priority

critical event:
  try preserve

telemetry:
  drop/coalesce latest
```

---

# 101. BLE disconnect behavior

On disconnect:

```text
mark not ready
stop normal notify attempts
preserve product state
restart advertising
```

Không reset product state chỉ vì BLE disconnect.

---

# 102. Gateway unavailable

Device vẫn phải chạy local function nếu product yêu cầu.

Ví dụ relay:

```text
local button vẫn hoạt động
```

Sensor:

```text
sampling vẫn có thể tiếp tục
```

Events có thể drop/coalesce khi offline.

---

# 103. Offline queue policy

Phase 1:

```text
do not persist telemetry backlog
```

Không ghi mọi event xuống flash.

RAM-only bounded state.

---

# 104. Battery Device considerations

Không phải mọi Device đều luôn advertising nhanh.

Có thể sau này thêm profile:

```text
fixed-power
battery-normal
battery-low-power
```

Nhưng Phase đầu ưu tiên stable connectivity.

---

# 105. Power management boundary

`device_app` không trực tiếp implement sleep strategy.

Có thể có:

```text
device_power
```

component sau này.

Không thêm trước khi Reference Device ổn định.

---

# 106. OTA boundary

OTA chưa thuộc Phase đầu.

Sau này có thể thêm:

```text
device_ota
```

Nhưng không đặt OTA logic trong `device_app`.

---

# 107. Security upgrade boundary

Phase đầu:

```text
bonding
LE Secure Connections
NO_IO
```

Sau này:

```text
signed ownership
application auth
secure provisioning
```

có thể bổ sung component riêng.

---

# 108. Documentation per component

Mỗi component cần:

```text
README.md
public API description
state ownership
thread-safety
error codes
test instructions
config options
```

Không chỉ source code.

---

# 109. Component README minimum

Ví dụ:

```text
Purpose
Public API
Dependencies
State machine
Concurrency
Configuration
Errors
Tests
Known limitations
```

---

# 110. Product documentation

Mỗi product:

```text
devices/<product>/README.md
```

Nội dung:

```text
hardware
pin mapping
target chip
commands
events
config
build
flash
factory reset
known limitations
release version
```

---

# 111. Pin mapping

Product-specific.

Ví dụ relay:

```text
RELAY GPIO
BUTTON GPIO
STATUS LED GPIO
```

Không hard-code trong `board_io` common nếu board khác nhau.

---

# 112. Board configuration

Có thể dùng:

```text
board_config.h
```

product-local.

Hoặc Kconfig.

Phase đầu ưu tiên compile-time board config.

---

# 113. Kconfig strategy

Common component Kconfig chỉ chứa reusable behavior.

Product-specific pin/config nằm product Kconfig.

Không tạo giant Kconfig toàn repo.

---

# 114. Example common Kconfig

`device_command`:

```text
COMMAND_QUEUE_DEPTH
COMMAND_TASK_STACK
COMMAND_TASK_PRIORITY
```

`device_event`:

```text
EVENT_QUEUE_DEPTH
EVENT_TASK_STACK
```

`ble_peripheral`:

```text
PREFERRED_MTU
ADV_INTERVAL
```

---

# 115. Build reproducibility

Release phải record:

```text
product
target
ESP-IDF version
firmware version
protocol version
git commit
protocol submodule commit
sdkconfig defaults
```

---

# 116. Release artifacts

Example:

```text
relay-1ch-v1.2.0/
├── bootloader.bin
├── partition-table.bin
├── relay_1ch.bin
├── flash_args.txt
├── manifest.json
└── SHA256SUMS
```

---

# 117. Release manifest

```json
{
  "product": "relay_1ch",
  "firmware_version": "1.2.0",
  "protocol_version": 2,
  "target": "esp32c3",
  "esp_idf": "5.4.4",
  "git_commit": "abcdef1234",
  "protocol_commit": "123456abcd"
}
```

---

# 118. Git tags

Nếu nhiều product cùng repo:

```text
reference-device-v1.0.0
relay-1ch-v1.0.0
temp-sensor-v1.0.0
```

Framework release có thể:

```text
device-framework-v1.0.0
```

nếu cần.

---

# 119. CI build matrix

Example:

```text
reference_device / esp32s3
relay_1ch        / esp32c3
temperature      / esp32c3
motor_controller / esp32s3
```

---

# 120. CI minimum gates

Mỗi PR:

```text
[ ] shared component tests
[ ] reference_device build
[ ] changed product build
```

Protocol update:

```text
[ ] protocol tests
[ ] reference_device build
[ ] Gateway build compatibility
```

---

# 121. Static analysis

Recommended:

```text
compiler warnings
clangd
format check
ESP-IDF component dependency validation
```

Không bắt buộc thêm quá nhiều tooling ở Phase đầu.

---

# 122. Development milestones

## M0 — Repository bootstrap

Tạo:

```text
components/
devices/
docs/
test/
tools/
```

Add `gateway_protocol`.

Definition:

```text
repo clone + submodule init hoạt động
```

---

# 123. M1 — gateway_protocol integration

Device build được shared headers/codec.

Pass:

```text
protocol unit tests
```

---

# 124. M2 — ble_peripheral skeleton

Implement:

```text
NimBLE init
GATT ABF0/ABF1/ABF2
advertising
connect/disconnect
```

Pass:

```text
Gateway scan sees Device
Gateway connects
```

---

# 125. M3 — security + READY

Implement:

```text
bonding
LE SC
CCCD
READY state
MTU
```

Pass:

```text
Gateway considers session READY
```

---

# 126. M4 — command pipeline

Implement:

```text
RX queue
decode
validation
registry
ACK
```

Pass:

```text
ping command ACK
```

---

# 127. M5 — event pipeline

Implement:

```text
event queue
priority
Notify
```

Pass:

```text
button event reaches Gateway
```

---

# 128. M6 — device_app

Integrate:

```text
profile
boot sequence
lifecycle
factory reset coordination
```

Pass:

```text
Reference Device uses only device_app from main
```

---

# 129. M7 — Reference Device complete

Hardware:

```text
LED + button
```

Integration pass full lifecycle.

---

# 130. M8 — relay_1ch

Implement first real product.

Use common framework unchanged wherever possible.

Any framework change required by relay phải được đánh giá reusable trước khi đưa vào common.

---

# 131. M9 — Test hardening

Add:

```text
fault injection
reboot loops
soak test
queue saturation
low heap
```

---

# 132. M10 — CI/release

Add:

```text
build matrix
artifact collection
manifest
hashes
tags
```

---

# 133. Framework Definition of Done

```text
[ ] shared protocol integrated
[ ] BLE Peripheral stable
[ ] security/bonding works
[ ] READY state correct
[ ] command registry stable
[ ] ACK correlation correct
[ ] event pipeline bounded
[ ] factory reset works
[ ] Reference Device complete
[ ] Gateway integration passes
[ ] tests documented
[ ] build reproducible
```

---

# 134. Product Definition of Done

Mỗi product:

```text
[ ] profile defined
[ ] target chip defined
[ ] sdkconfig defaults defined
[ ] pin mapping documented
[ ] product init works
[ ] commands registered
[ ] events registered
[ ] drivers isolated
[ ] Gateway connects
[ ] commands ACK
[ ] events notify
[ ] reboot reconnect
[ ] factory reset verified
[ ] clean build passes
[ ] release manifest generated
```

---

# 135. PR review checklist

Framework PR:

```text
[ ] dependency direction đúng
[ ] không product logic vào common
[ ] không BLE logic vào driver
[ ] không blocking NimBLE callback
[ ] queue bounded
[ ] error codes explicit
[ ] test added
[ ] README updated
```

Product PR:

```text
[ ] main.c nhỏ
[ ] product profile rõ
[ ] driver boundary đúng
[ ] command/event documented
[ ] target/sdkconfig đúng
[ ] build pass
```

---

# 136. Anti-patterns cần tránh

Không:

```text
copy ble_peripheral sang từng product
copy CBOR schema
hard-code protocol version trong product
product handler gọi NimBLE trực tiếp
driver publish BLE Notify
main.c chứa tất cả initialization
NimBLE callback gọi sensor chậm
telemetry chiếm queue làm ACK timeout
factory reset = erase toàn bộ flash
```

---

# 137. Recommended implementation priority

Ưu tiên correctness:

```text
protocol
BLE
security
command ACK
event
reconnect
factory reset
```

Sau đó mới:

```text
power optimization
OTA
advanced telemetry
dynamic capabilities
```

---

# 138. Kiến trúc cuối cùng

```text
                       esp-ble-device
                             │
              ┌──────────────┴──────────────┐
              │                             │
              ▼                             ▼
        Common Framework               Product Projects
              │                             │
   ┌──────────┼──────────┐            ┌─────┼─────┐
   ▼          ▼          ▼            ▼     ▼     ▼
device_app ble_periph device_cmd    relay sensor motor
   │          │          │
   ├──────────┼──────────┤
   │          ▼          │
   │   gateway_protocol  │
   │          │          │
   └──────────┴──────────┘
              │
              ▼
           ESP-IDF
              │
              ▼
           NimBLE
```

---

# 139. Recommended first repository state

Ngay sau bootstrap, repository nên có tối thiểu:

```text
esp-ble-device/
├── components/
│   ├── device_app/
│   ├── ble_peripheral/
│   ├── device_command/
│   ├── device_event/
│   ├── device_core/
│   ├── device_storage/
│   └── gateway_protocol/
│
├── devices/
│   └── reference_device/
│
├── docs/
│   └── ESP_BLE_Device_Development_Architecture_v1.0.md
│
└── test/
```

Không cần tạo tất cả product ngay.

---

# 140. Kết luận

`esp-ble-device` phải được phát triển như một Device SDK/Framework chứ không như một firmware đơn lẻ.

Nguyên tắc cốt lõi:

```text
One framework
Many products
One protocol contract
Independent firmware builds
```

`device_app` là composition root.

`ble_peripheral` là transport.

`gateway_protocol` là wire contract.

`device_command` và `device_event` là application messaging layer.

Product layer chỉ chứa logic sản phẩm và driver.

Reference Device là chuẩn kiểm thử bắt buộc trước khi phát triển product thực tế.

Thứ tự triển khai nên là:

```text
repo bootstrap
→ shared protocol
→ BLE Peripheral
→ security/READY
→ command ACK
→ event pipeline
→ device_app
→ Reference Device
→ Gateway integration
→ first real product
→ CI/release
```

Đây là nền tảng được khuyến nghị để `esp-ble-device` có thể mở rộng từ một thiết bị thử nghiệm lên nhiều dòng thiết bị mà không làm phân mảnh BLE protocol hoặc kiến trúc firmware.
