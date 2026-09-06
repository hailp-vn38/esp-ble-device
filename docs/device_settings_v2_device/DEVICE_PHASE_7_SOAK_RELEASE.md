# Device Phase D7 — Soak, NVS Durability & Release Hardening


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Goal

Release gate cho device firmware sau khi functional tests pass.

## 100-cycle save/reboot soak

Mỗi cycle:

```text
boot/connect
-> discover/read
-> BEGIN expected_revision
-> SET deterministic/random valid values
-> COMMIT
-> CONFIRM or selected loss scenario
-> reboot/reconnect
-> read
-> verify values + revision
```

Acceptance:

- [ ] revision increments exactly once per successful commit.
- [ ] no spontaneous factory default.
- [ ] no heap trend indicating leak.
- [ ] no watchdog/reset ngoài expected restart.

## 100-cycle discovery soak

- [ ] stable heap minima.
- [ ] no notify queue starvation.
- [ ] runtime feature commands still responsive between cycles.

## NVS durability policy

V2 intentionally writes only on COMMIT, never per SET. Review:

- expected settings update frequency;
- namespace/key layout;
- blob size;
- migration behavior;
- factory reset behavior.

## Power-loss model

Document guarantees actually provided by ESP-IDF/NVS usage. Test practical boundaries; không claim stronger atomicity than storage layer guarantees.

## Resource hardening

- [ ] All task stacks measured high-water mark.
- [ ] No settings worker stack oversized without evidence.
- [ ] No unbounded malloc in command loop.
- [ ] No large automatic arrays on task stack.
- [ ] Descriptor registry RAM footprint recorded.

## Release checklist

- [ ] `idf.py build` clean.
- [ ] warnings reviewed.
- [ ] unit tests pass.
- [ ] HIL matrix pass.
- [ ] protocol golden vectors pass.
- [ ] compatibility old gateway pass.
- [ ] 100 discovery pass.
- [ ] 100 save/reboot pass.
- [ ] secret log audit pass.
- [ ] reset/factory reset behavior documented.

## Exit gate

Firmware is ready to be paired with the gateway release candidate.
