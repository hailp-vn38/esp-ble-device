# esp-ble-device

ESP32-S3 BLE Peripheral framework + product firmware for the ESP-GATT protocol system.

## Structure

```
esp-ble-device/
├── components/          # Shared framework components
│   ├── device_command/  # Command RX pipeline (CBOR decode → handler → ACK)
│   ├── device_feature/  # Feature registry + semantic state publish
│   ├── device_event/    # Event pipeline (button, telemetry, feature_state)
│   ├── device_app/      # App lifecycle orchestration
│   ├── ble_peripheral/  # BLE transport (NimBLE GATT server)
│   └── gateway_protocol/ # Wire contract (CBOR codec, message model)
├── devices/
│   └── reference_device/ # Golden Peripheral: 1 LED (GPIO8) + 1 button (GPIO9)
├── tools/
│   └── build.sh          # Build & flash script
├── test/
│   ├── host/             # Host unit tests
│   └── hardware/         # Hardware integration tests
└── docs/                 # Architecture & protocol specs
```

## Prerequisites

- ESP-IDF v5.5+ installed at `~/.espressif/`
- ESP32-S3 dev board connected via USB

## Quick Start

```bash
# Interactive — pick device + command
./tools/build.sh

# Build reference_device
./tools/build.sh reference_device build
./tools/build.sh demo_device build


# Build + flash
./tools/build.sh reference_device flash

# Build + flash + monitor
./tools/build.sh reference_device monitor

# Full clean + rebuild
./tools/build.sh reference_device clean

# Use specific serial port
./tools/build.sh demo_device flash /dev/tty.usbmodem2101

# Use different ESP-IDF version
IDF_VERSION=6.1-rc1 ./tools/build.sh reference_device flash
```

The script auto-discovers devices in `devices/*/` — add a new folder there and it appears automatically.

## Adding a New Device

1. Create `devices/<your_device>/` with:
   - `CMakeLists.txt` — set `project(<your_device>)` and `EXTRA_COMPONENT_DIRS`
   - `sdkconfig.defaults` — target chip, flash size, BLE config
   - `partitions.csv` — partition layout
   - `main/` — product logic (`<product>.c`, `<product>.h`, `main.c`)
2. Register commands + features in your product's `register_commands()` / `register_features()`.
3. Run `./tools/build.sh <your_device> flash`.

## Reference Device

- **Commands:** `set_led` (bool, idempotent), `get_state` (read-only)
- **Features:** `led_main` (on_off_light, property: GW_PROP_ON_OFF)
- **Events:** `button_pressed`, `feature_state`, `heartbeat`
- **Target:** ESP32-S3, 4MB flash, NimBLE peripheral

## Protocol

Device ↔ Gateway communication uses ESP-GATT Protocol v4 over BLE GATT (NimBLE).

- Service: `0xABF0`
- Command characteristic (write): `0xABF1`
- Status characteristic (notify): `0xABF2`
- Encoding: CBOR with numeric keys

## Docs

- `docs/ESP_BLE_Device_Development_Architecture_v1.0.md` — full architecture
- `docs/ESP_BLE_Device_Development_Spec_v1.3.md` — implementation spec
- `docs/ESP_BLE_Gateway_Device_Integration_Contract_for_AI_Agent_v1.0.md` — integration contract
