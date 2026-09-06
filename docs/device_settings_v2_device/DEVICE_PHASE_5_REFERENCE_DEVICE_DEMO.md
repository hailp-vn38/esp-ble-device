# Device Phase D5 — Reference Device Integration & Demo Coverage


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Tạo reference implementation thể hiện đầy đủ generic Settings contract để gateway/UI không cần hardcode product.

## Files

```text
devices/reference_device/main/reference_product.c
devices/reference_device/main/reference_config.h
devices/reference_device/main/reference_config.c
```

## Demo settings tối thiểu

| ID | Title | Type | Example constraints |
|---|---|---|---|
| `sensor_enabled` | Sensor enabled | BOOL | writable |
| `sample_interval` | Sample interval | INT | 1..3600 s, step 1 |
| `target_temperature` | Nhiệt độ sấy | INT | 30..120 °C |
| `fan_mode` | Fan mode | ENUM | Off/Manual/Auto |
| `device_label` | Device label | STRING | max 32 |
| `admin_token` | Admin token | STRING/SECRET | KEEP/SET/CLEAR |
| `serial_number` | Serial number | STRING/READONLY | read-only |

Có thể giữ demo gọn nhưng phải cover mọi renderer/semantic chính.

## Registration

Product chỉ làm:

```c
static const descriptor ...;

device_setting_register(&desc_a);
device_setting_register(&desc_b);
...
```

Gateway không biết `target_temperature` trước discovery.

## Schema revision

Increment `settings_schema_revision` khi descriptor schema thay đổi có ảnh hưởng gateway cache/render:

- add/remove setting;
- type change;
- min/max/step;
- enum options;
- flags/title/group/unit nếu muốn UI refresh deterministic.

Không increment schema revision chỉ vì value thay đổi.

## Product config migration

Test ít nhất một fake previous format -> current format:

```text
format_version 1 -> 2
```

Migration phải preserve valid old fields, set defaults new fields, và không reset `config_revision` tùy policy đã khóa.

## Tests

- [ ] All settings discover with exact titles/groups/units.
- [ ] UI-independent BLE harness can update every writable type.
- [ ] READONLY rejects write.
- [ ] SECRET read returns configured state only.
- [ ] Cross-field invalid combination reject.
- [ ] schema_revision changes when demo schema changes.
- [ ] values survive reboot.
- [ ] factory reset policy for settings documented/tested.

## Checklist

- [ ] No product-specific ID in generic `device_settings` component.
- [ ] Product config is versioned.
- [ ] Defaults documented.
- [ ] Factory reset clears/retains secret according to explicit policy.
- [ ] Demo covers BOOL/INT/STRING/ENUM/SECRET/READONLY.

## Exit gate

Reference device can serve as HIL fixture for all gateway phases without firmware source changes.
