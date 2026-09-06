# Device Settings v2 — Device Test Matrix & Release Checklist


**Repo:** `hailp-vn38/esp-ble-device`  
**Baseline:** `main`  
**Target:** ESP32-S3  
**Feature:** Device Settings v2  
**Protocol baseline:** ESP-GATT Protocol v4; Phase 0 quyết định giữ v4 extension hay bump v5.


## Matrix

| Area | Case | Expected |
|---|---|---|
| Registry | duplicate id | reject |
| Registry | max descriptors | exact boundary works |
| INT | below/above range | reject |
| ENUM | unknown value | reject |
| STRING | > max length | reject |
| Secret | read | configured only |
| Revision | stale BEGIN | conflict |
| Tx | SET before BEGIN | reject |
| Tx | disconnect pre-commit | no persist |
| Tx | duplicate COMMIT | one revision increment |
| Commit | NVS failure | old config remains |
| Confirm | confirm lost | timeout reboot |
| Reboot | post-commit | new values/revision |
| Discovery | disconnect mid-stream | clean abort/recover |
| Protocol | malformed CBOR | no crash/mutation |
| Compatibility | old gateway | no regression core features |

## PR checklist

### Architecture
- [ ] Generic component contains no product IDs.
- [ ] Descriptor data is flash-resident where possible.
- [ ] Registry freezes before BLE ready.
- [ ] Active/staging config separation is explicit.

### Persistence
- [ ] revision stored in same blob.
- [ ] one NVS commit per successful settings transaction.
- [ ] failed commit cannot promote staging.
- [ ] migration tested.

### BLE
- [ ] discovery streamed.
- [ ] frame max asserted/tested.
- [ ] tx command string ownership safe.
- [ ] disconnect paths release transaction state.

### Security
- [ ] secret plaintext never read/logged.
- [ ] BLE security policy unchanged/regression-tested.

### Reboot
- [ ] no restart directly inside critical BLE callback.
- [ ] commit-confirm timeout fallback tested.

## Final release condition

All D0–D7 exit gates pass and gateway RC HIL matrix can use the reference device without firmware-side workarounds.
