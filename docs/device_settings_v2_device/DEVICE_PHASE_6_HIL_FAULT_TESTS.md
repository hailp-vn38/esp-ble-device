# Device Phase D6 — HIL, Fault Injection & Compatibility Tests


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Chứng minh firmware behavior đúng dưới BLE disconnect, malformed protocol, revision conflict, NVS failure và reboot timing.

## HIL suite

### Discovery
- [ ] 100 repeated `describe_settings`.
- [ ] 100 repeated `read_settings`.
- [ ] disconnect at every stream position sampled.
- [ ] reconnect and rediscover.

### Transaction
- [ ] one-field save.
- [ ] max-change save.
- [ ] each supported type.
- [ ] stale revision.
- [ ] wrong tx id.
- [ ] abort.
- [ ] timeout.
- [ ] duplicate SET/COMMIT.

### Persistence
- [ ] NVS set_blob failure injection.
- [ ] NVS commit failure injection.
- [ ] reboot after successful commit.
- [ ] power cut simulation around commit boundary if fixture permits.

### Reboot/ACK loss
- [ ] drop commit ACK.
- [ ] drop confirm.
- [ ] disconnect after commit.
- [ ] disconnect before commit.

### Security/secret
- [ ] unready/unsecured BLE request rejected per existing policy.
- [ ] secret plaintext absent from discovery/read frames.
- [ ] KEEP preserves.
- [ ] CLEAR removes.
- [ ] SET replaces.

## Protocol fuzz

Feed malformed CBOR cases:

- truncated map;
- wrong integer widths/types;
- duplicate keys;
- unknown keys;
- oversized strings;
- invalid enum;
- random bytes.

Acceptance:

```text
no crash
no OOB
no persistent mutation
bounded error response
next valid command still works
```

## Compatibility

- [ ] old gateway ↔ new device: core device feature/control regression pass.
- [ ] new gateway ↔ old device handled by gateway pack, but device golden vectors must remain compatible.

## Instrumentation

Log at test builds:

```text
free internal heap
minimum free internal heap
free PSRAM if used by product
transaction state
config_revision
reset reason
```

Do not log SECRET values.

## Exit gate

Zero data-consistency failures across full fault matrix; all protocol fuzz cases are non-fatal and non-mutating.
