# Phase 3 — `demo_device` Product

## 1. Tổng quan

Không mở rộng `reference_device` thành mega demo.

Tạo product riêng:

```text
devices/demo_device/
```

Mục tiêu là một reference peripheral đa chức năng để test toàn hệ thống.

---

## 2. Mục tiêu

- tạo product demo độc lập;
- 7 public tools + 10 features;
- module hóa feature/driver;
- dùng simulated sensor trước;
- không task-per-feature;
- giữ `reference_device` làm regression baseline.

---

## 3. Cần thêm / sửa gì

| Nhóm | Cần làm |
|---|---|
| Product | tạo `devices/demo_device` |
| Features | relay/plug/light/dimmer/fan/sensors/generic values |
| Drivers | GPIO/PWM/input/sensor abstraction |
| Composition | `demo_product.c` chỉ đăng ký/compose |
| Memory | static state, shared worker/timer |
| Tests | build/hardware/discovery/state/reconnect |

---

## 4. Target structure

```text
devices/demo_device/
├── CMakeLists.txt
├── product.json
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── main.c
    ├── demo_product.c
    ├── demo_product.h
    ├── demo_board.h
    ├── features/
    │   ├── demo_relay.c/.h
    │   ├── demo_plug.c/.h
    │   ├── demo_light.c/.h
    │   ├── demo_dimmer.c/.h
    │   ├── demo_fan.c/.h
    │   ├── demo_environment.c/.h
    │   ├── demo_contact.c/.h
    │   └── demo_process_values.c/.h
    └── drivers/
        ├── demo_gpio_output.c/.h
        ├── demo_pwm.c/.h
        ├── demo_input.c/.h
        └── demo_sensor.c/.h
```

---

## 5. Feature/tool map

### Actuators

| Feature | Tool | Value |
|---|---|---|
| relay_main | set_relay | BOOL |
| plug_main | set_plug | BOOL |
| light_main | set_light | BOOL |
| dimmer_main | set_brightness | INT 0..100 |
| fan_main | set_fan_speed | INT 0..100 |

### Sensors

| Feature | Writable | Value |
|---|---|---|
| temperature_main | no | INT decimals=1 °C |
| humidity_main | no | INT % |
| contact_main | no | BOOL |

### Generic values

`dryer_temperature`:

```text
title = Nhiệt độ sấy
type = GENERIC_VALUE
property = VALUE
unit = °C
decimals = 1
tool = set_dryer_temperature
tool range = 300..1000 step 5
```

`drying_time`:

```text
title = Thời gian sấy
type = GENERIC_VALUE
property = VALUE
unit = min
decimals = 0
tool = set_drying_time
tool range = 1..180 step 1
```

---

## 6. Hardware abstraction

Pin chỉ nằm trong:

```text
demo_board.h
```

Feature module không hard-code GPIO.

Ví dụ default mapping:

```c
#define DEMO_RELAY_GPIO       GPIO_NUM_4
#define DEMO_PLUG_GPIO        GPIO_NUM_5
#define DEMO_FAN_PWM_GPIO     GPIO_NUM_6
#define DEMO_CONTACT_GPIO     GPIO_NUM_7
#define DEMO_LIGHT_GPIO       GPIO_NUM_8
#define DEMO_BUTTON_GPIO      GPIO_NUM_9
#define DEMO_DIMMER_PWM_GPIO  GPIO_NUM_10
```

Phải verify board thực tế trước flash.

---

## 7. Driver rules

### GPIO output

Sở hữu active-high/active-low mapping.

### PWM

Dùng LEDC.

Không task riêng.

### Input

ISR chỉ signal/debounce, không CBOR/BLE work trong ISR.

### Sensor

API:

```c
typedef struct {
    int32_t temperature_raw;
    int32_t humidity_raw;
} demo_environment_sample_t;
```

Simulation mode trước, sensor thật sau.

---

## 8. Task / memory policy

Không tạo:

```text
relay_task
fan_task
light_task
temperature_task
...
```

Target thêm tối đa:

- 1 environment polling task/timer;
- 1 shared input/debounce worker nếu cần.

State static:

```c
static bool s_relay;
static uint8_t s_fan;
static int32_t s_temperature;
static int32_t s_dryer_temperature;
```

Không heap allocate per feature.

---

## 9. Sensor simulation

Deterministic:

```text
temperature raw:
250 -> 251 -> 252 -> 253 -> 252 -> 251

humidity:
55 -> 56 -> 57 -> 58 -> 57 -> 56
```

Không random noise.

Publish threshold:

- temperature: >= 1 raw unit nếu decimals=1 → 0.1°C;
- humidity: >=1%;
- polling 2–5 s.

Có thể tăng threshold khi BLE traffic cần giảm.

---

## 10. Local button demo

Short press:

```text
toggle relay_main
```

Flow:

```text
button
→ driver/input worker
→ relay module
→ hardware
→ semantic state
→ publish feature event
→ Gateway
→ WS
→ UI
```

Đây là test chứng minh state change không xuất phát từ Gateway.

---

## 11. `demo_product.c`

Chỉ composition:

```text
init modules
register commands
register features
register events
start/stop
```

Không chứa:

- `gpio_set_level`;
- `ledc_set_duty`;
- sensor algorithm;
- ISR implementation.

---

## 12. Product profile

Đề xuất:

```text
model = esp32s3-demo
ble_name_prefix = GW-DEMO
protocol_version = 4
capability_revision = 1 (initial v2 demo schema)
```

Sau khi schema public đã publish:

- đổi title;
- đổi decimals;
- đổi unit;
- đổi tool range;
- add/remove feature/tool;

đều phải bump capability revision.

---

## 13. Sửa / thêm ở đâu

### Device repo

| Path | Action |
|---|---|
| `devices/demo_device/` | tạo mới |
| `devices/demo_device/main/main.c` | composition entry |
| `devices/demo_device/main/demo_product.c` | profile/composition |
| `devices/demo_device/main/demo_board.h` | pin config |
| `devices/demo_device/main/features/*` | semantic modules |
| `devices/demo_device/main/drivers/*` | hardware modules |
| root build tooling | verify autodiscovery |
| `test/host/*` | product-level protocol tests nếu phù hợp |

`reference_device` chỉ sửa compile compatibility nếu shared API đổi.

---

## 14. Checklist

### Structure

- [ ] product mới build độc lập.
- [ ] reference device không chứa demo logic.
- [ ] mỗi feature module nhỏ/rõ trách nhiệm.
- [ ] hardware code chỉ ở driver.
- [ ] product file chỉ compose.

### Features

- [ ] 10 feature unique IDs.
- [ ] 7 tools unique commands.
- [ ] FAN=PERCENT_SETTING.
- [ ] DIMMER=LEVEL.
- [ ] measured temp khác dryer setpoint.
- [ ] generic values đúng scale.
- [ ] read-only không có tool.

### Runtime

- [ ] command ACK trả actual state.
- [ ] local event publish.
- [ ] sensor event publish.
- [ ] no task per feature.
- [ ] no per-feature heap.

---

## 15. Test plan

### T3.1 — Build

```bash
./tools/build.sh demo_device build
```

Expected pass.

### T3.2 — Flash/boot

Expected:

```text
GW-DEMO advertising
bonding ready
CCCD ready
```

### T3.3 — Discovery

Expected:

```text
tools=7
features=10
schema ready
```

### T3.4 — Relay/light/plug

Set ON/OFF, verify physical/logical state.

### T3.5 — Dimmer

0, 1, 50, 100.

### T3.6 — Fan

0, 25, 60, 100.

Nếu driver clamp, ACK phải trả actual.

### T3.7 — Dryer temperature

Input display 65.5 → raw 655.

Expected state/ACK raw consistent.

### T3.8 — Drying time

1, 30, 180; out-of-range reject.

### T3.9 — Sensor simulation

Expected deterministic events.

### T3.10 — Contact

Input transition → feature event.

### T3.11 — Local button

Expected realtime relay update.

### T3.12 — Reboot/reconnect

Expected schema/state recovery.

---

## 16. Exit criteria

- [ ] 7 tools.
- [ ] 10 features.
- [ ] all actuator commands pass.
- [ ] generic values pass.
- [ ] async event paths pass.
- [ ] no obvious task/heap regression.

## 17. Implementation status

- Đã tạo product độc lập `devices/demo_device/`, không sửa logic của `reference_device`.
- Đã đăng ký 7 public tools và 10 semantic features theo profile Phase 3.
- Đã thêm driver GPIO output, PWM, input/debounce và sensor simulation dùng task chia sẻ.
- Đã map FAN=`PERCENT_SETTING`, DIMMER=`LEVEL`, generic values với decimals/unit/range theo schema.
- Build độc lập pass với ESP-IDF 6.1-rc1; image chưa flash vì chưa xác nhận board thực tế tương ứng với `GW-DEMO`.
