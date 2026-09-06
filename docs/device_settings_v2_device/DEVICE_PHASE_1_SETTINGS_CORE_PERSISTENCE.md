# Device Phase D1 — Settings Core, Registry, Validation & Persistence


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Xây core hoàn toàn độc lập BLE: descriptor registry, active/staging config, validation, atomic NVS commit và revision.

## Add files

```text
components/device_settings/CMakeLists.txt
components/device_settings/include/device_settings.h
components/device_settings/device_settings_registry.c
components/device_settings/device_settings_transaction.c
components/device_settings/device_settings_persistence.c
components/device_settings/device_settings_restart.c
```

Modify:

```text
components/device_app/include/device_app.h
components/device_app/device_app.c
```

## Descriptor contract

```c
typedef enum {
    DEVICE_SETTING_BOOL,
    DEVICE_SETTING_INT,
    DEVICE_SETTING_STRING,
    DEVICE_SETTING_ENUM,
} device_setting_type_t;

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
    const void *options;
    uint8_t option_count;
    esp_err_t (*read)(void *ctx, void *out);
    esp_err_t (*stage)(void *ctx, const void *value);
    void *ctx;
} device_setting_descriptor_t;
```

Flags V2:

```text
READONLY
SECRET
ADVANCED
```

`REQUIRES_REBOOT` không cần trong V2 vì mọi commit đều reboot.

## RAM policy

Descriptors:

```c
static const device_setting_descriptor_t s_xxx = { ... };
```

Registry:

```c
static const device_setting_descriptor_t *s_settings[DEVICE_SETTING_MAX_COUNT];
```

Không memcpy descriptors/strings vào heap.

## Device app lifecycle

Extend profile:

```c
esp_err_t (*register_settings)(void);
uint32_t settings_schema_revision;
```

Boot order:

```text
NVS init
-> product config load/migrate
-> device_settings_init
-> register_settings
-> device_settings_freeze
-> command/event init
-> BLE init/start
```

## Persistence model

Product config là một versioned blob, ví dụ:

```c
typedef struct {
    uint16_t format_version;
    uint16_t reserved;
    uint32_t config_revision;
    bool sensor_enabled;
    int32_t sample_interval;
    int32_t target_temperature;
    int32_t fan_mode;
} reference_config_t;
```

Invariant bắt buộc:

```text
new_revision = active.config_revision + 1
staging.config_revision = new_revision
nvs_set_blob(staging)
nvs_commit()
active = staging
```

Không increment revision sau commit.

## Local transaction API

```c
esp_err_t device_settings_tx_begin(uint64_t transaction_id,
                                   uint32_t expected_revision);
esp_err_t device_settings_tx_set(...);
esp_err_t device_settings_tx_commit(uint64_t transaction_id,
                                    uint32_t *new_revision);
esp_err_t device_settings_tx_abort(uint64_t transaction_id);
```

### BEGIN

- reject nếu transaction khác đang active;
- duplicate same tx id trả deterministic previous/current status;
- compare expected revision tại device;
- copy active -> staging;
- start timeout.

### SET

- resolve id;
- readonly/type/range/length/enum validation;
- mutate staging only;
- duplicate request id không apply hai lần.

### COMMIT

- validate all fields + cross-field rules;
- persist one blob + revision;
- promote staging only after NVS commit success;
- record last committed tx id/revision đủ để duplicate COMMIT idempotent.

### ABORT

- discard staging;
- no revision change;
- safe duplicate.

## Cross-field validation

Cần callback product-level, ví dụ:

```c
esp_err_t reference_config_validate(const reference_config_t *cfg);
```

Không chỉ validate từng field.

## Secret semantics

Read:

```text
configured: true/false
value: never plaintext
```

Write action:

```text
KEEP
SET(value)
CLEAR
```

Blank string không được tự động hiểu là CLEAR.

## Tests — Unit

### Registry
- [ ] register valid 4 types.
- [ ] duplicate id reject.
- [ ] max count boundary.
- [ ] invalid enum metadata reject.
- [ ] register after freeze reject.
- [ ] descriptor pointer remains valid without heap copy.

### Validation
- [ ] INT min/max/step.
- [ ] ENUM invalid value reject.
- [ ] STRING max length.
- [ ] READONLY reject SET.
- [ ] cross-field invalid combination rejects entire commit.

### Transaction
- [ ] BEGIN correct revision.
- [ ] BEGIN stale revision conflict.
- [ ] SET does not mutate active.
- [ ] ABORT restores no change.
- [ ] COMMIT increments exactly once.
- [ ] duplicate COMMIT same tx does not increment twice.
- [ ] timeout before commit aborts staging.

### Persistence
- [ ] reboot loads exact committed values + revision.
- [ ] NVS commit failure leaves active unchanged.
- [ ] format migration deterministic.
- [ ] corrupt blob follows safe-default/error policy.

## Checklist

- [ ] No per-setting heap descriptor copy.
- [ ] Active/staging ownership documented.
- [ ] `config_revision` persisted atomically.
- [ ] Product-level validation exists.
- [ ] Transaction timeout exists.
- [ ] Last transaction idempotency state bounded.
- [ ] Secret value never returned by generic read API.

## Exit gate

Local tests can perform 100 BEGIN/SET/COMMIT/reload cycles without revision/value mismatch and without BLE transport.
