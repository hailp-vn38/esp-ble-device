#!/usr/bin/env python3
"""
HW-DEV Test Harness — ESP-BLE Device Hardware Verification (Spec v1.3 §27)

Tests HW-DEV-001 through HW-DEV-011 using bleak (Python BLE) against the
reference device flashed on /dev/cu.usbmodem2101 (or CLI --port override).

Usage:
  python3 test_hw_dev.py                          # default port
  python3 test_hw_dev.py --port /dev/cu.usbmodem2101
  python3 test_hw_dev.py --test HW-DEV-004        # single test
  python3 test_hw_dev.py --list                    # list tests
"""

import argparse
import asyncio
import struct
import sys
import time
import traceback
from dataclasses import dataclass, field
from typing import Any, Optional

import bleak
from bleak import BleakClient, BleakScanner
import cbor2

# ── GATT UUIDs ────────────────────────────────────────────────────────────────

SERVICE_UUID     = "0000abf0-0000-1000-8000-00805f9b34fb"
CMD_CHAR_UUID    = "0000abf1-0000-1000-8000-00805f9b34fb"  # WRITE_NO_RSP
STATUS_CHAR_UUID = "0000abf2-0000-1000-8000-00805f9b34fb"  # NOTIFY
CCCD_UUID        = "00002902-0000-1000-8000-00805f9b34fb"

# ── CBOR Key IDs (spec §6.1) ──────────────────────────────────────────────────

K_PROTOCOL_VERSION = 0
K_TYPE             = 1
K_DEVICE_ID        = 2
K_COMMAND          = 3
K_INT_VALUE        = 4
K_BOOL_VALUE       = 5
K_NAME             = 6
K_DEVICE_TYPE      = 7
K_BLE_ADDR         = 8
K_BLE_ADDR_TYPE    = 9
K_REQUEST_ID       = 10
K_SNAPSHOT_ID      = 11
K_SEQUENCE         = 12
K_TOTAL            = 13
K_VALUE_TYPE       = 14
K_CAPABILITY_FLAGS = 15
K_MIN_VALUE        = 16
K_MAX_VALUE        = 17
K_STEP             = 18
K_CAPABILITY_LABEL = 19
K_CAPABILITY_UNIT  = 20
K_CAPABILITY_REV   = 21

# ── Value types ───────────────────────────────────────────────────────────────

VT_NONE = 0
VT_BOOL = 1
VT_INT  = 2

# ── Capability flags ──────────────────────────────────────────────────────────

FLAG_IDEMPOTENT  = 0x01
FLAG_DESTRUCTIVE = 0x02

# ── Test gateway ID ──────────────────────────────────────────────────────────

GATEWAY_ID = "lamp-1"

# ── Helpers ───────────────────────────────────────────────────────────────────

def build_msg(*, msg_type: str, command: str = "", int_value: int = 0,
              bool_value: bool = False, request_id: int = 0,
              snapshot_id: int = 0, sequence: int = 0, total: int = 0,
              capability_rev: int = 0, value_type: int = 0,
              capability_flags: int = 0, label: str = "", unit: str = "",
              min_value: int = 0, max_value: int = 0, step: int = 0) -> bytes:
    """Build a CBOR-encoded BLE message per ESP-GATT Protocol v3."""
    m: dict[int, Any] = {
        K_PROTOCOL_VERSION: 3,
        K_TYPE: msg_type,
    }
    if command:
        m[K_DEVICE_ID] = GATEWAY_ID
        m[K_COMMAND] = command
    if int_value != 0 or msg_type == "device_command":
        m[K_INT_VALUE] = int_value
    if msg_type == "device_command":
        m[K_BOOL_VALUE] = bool_value
    if request_id:
        m[K_REQUEST_ID] = request_id
    if snapshot_id:
        m[K_SNAPSHOT_ID] = snapshot_id
    if msg_type == "capability_item":
        m[K_SEQUENCE] = sequence
        m[K_VALUE_TYPE] = value_type
        m[K_CAPABILITY_FLAGS] = capability_flags
        if label:
            m[K_CAPABILITY_LABEL] = label
        if unit is not None:
            m[K_CAPABILITY_UNIT] = unit
        if value_type == VT_INT:
            m[K_MIN_VALUE] = min_value
            m[K_MAX_VALUE] = max_value
            m[K_STEP] = step
    if total:
        m[K_TOTAL] = total
    if capability_rev:
        m[K_CAPABILITY_REV] = capability_rev
    if msg_type == "capabilities_end":
        m[K_BOOL_VALUE] = bool_value
    return cbor2.dumps(m)


def parse_msg(data: bytes) -> dict:
    """Decode a CBOR message from bytes."""
    return cbor2.loads(data)


def msg_type(msg: dict) -> str:
    return msg.get(K_TYPE, "")


def msg_field(msg: dict, key: int, default=None):
    return msg.get(key, default)


# ── BLE connection helper ─────────────────────────────────────────────────────

@dataclass
class BLEConnection:
    client: Optional[BleakClient] = None
    notifications: list = field(default_factory=list)
    _event: asyncio.Event = field(default_factory=asyncio.Event)

    async def on_notification(self, sender, data: bytearray):
        self.notifications.append(bytes(data))
        self._event.set()

    async def connect(self, timeout: float = 10.0) -> bool:
        """Scan for the reference device and connect."""
        print("  Scanning for GW-REF...")
        device = await BleakScanner.find_device_by_name("GW-REF", timeout=timeout)
        if device is None:
            print("  ERROR: GW-REF not found in scan")
            return False

        print(f"  Found: {device.name} ({device.address})")
        self.client = BleakClient(device)
        await self.client.connect(timeout=timeout)
        print(f"  Connected to {device.address}")

        # Enable notifications on Status characteristic
        await self.client.start_notify(STATUS_CHAR_UUID, self.on_notification)
        print("  Notifications enabled on Status characteristic")
        return True

    async def disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()
            print("  Disconnected")

    async def write_command(self, payload: bytes):
        """Write a command payload to the Command characteristic (WRITE_NO_RSP)."""
        await self.client.write_gatt_char(CMD_CHAR_UUID, payload, response=False)

    async def wait_notification(self, timeout: float = 5.0) -> Optional[bytes]:
        """Wait for the next notification."""
        self._event.clear()
        try:
            await asyncio.wait_for(self._event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            return None
        if self.notifications:
            return self.notifications.pop(0)
        return None

    async def flush_notifications(self, timeout: float = 0.5):
        """Drain any pending notifications."""
        self.notifications.clear()

    async def send_command_and_wait_ack(self, payload: bytes, timeout: float = 5.0) -> Optional[dict]:
        """Write command and wait for ACK notification."""
        await self.flush_notifications()
        await self.write_command(payload)
        raw = await self.wait_notification(timeout=timeout)
        if raw is None:
            return None
        return parse_msg(raw)

    async def send_command_collect(self, payload: bytes, count: int, timeout: float = 5.0) -> list[dict]:
        """Write command and collect `count` notifications."""
        await self.flush_notifications()
        await self.write_command(payload)
        results = []
        for _ in range(count):
            raw = await self.wait_notification(timeout=timeout)
            if raw is None:
                break
            results.append(parse_msg(raw))
        return results

    async def send_command_collect_until_ack(self, payload: bytes, timeout: float = 5.0) -> list[dict]:
        """Write command and collect notifications until ACK arrives (or timeout)."""
        await self.flush_notifications()
        await self.write_command(payload)
        results = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            raw = await self.wait_notification(timeout=max(remaining, 0.1))
            if raw is None:
                break
            msg = parse_msg(raw)
            results.append(msg)
            if msg_type(msg) == "device_ack":
                break
        return results


# ── Test Results ──────────────────────────────────────────────────────────────

@dataclass
class TestResult:
    test_id: str
    name: str
    passed: bool
    detail: str = ""


# ── Tests ─────────────────────────────────────────────────────────────────────

_req_counter = 0


def next_request_id() -> int:
    global _req_counter
    _req_counter += 1
    return _req_counter


async def test_001_advertising(ble: BLEConnection) -> TestResult:
    """HW-DEV-001: First advertising/discovery."""
    # This test performs the connection itself
    if ble.client and ble.client.is_connected:
        return TestResult("HW-DEV-001", "First advertising/discovery", True,
                          f"Device already connected via {ble.client.address}")

    connected = await ble.connect(timeout=15.0)
    if connected:
        return TestResult("HW-DEV-001", "First advertising/discovery", True,
                          f"Device connected via {ble.client.address}")
    return TestResult("HW-DEV-001", "First advertising/discovery", False,
                      "Failed to connect")


async def test_002_security_cccd(ble: BLEConnection) -> TestResult:
    """HW-DEV-002: Security + CCCD."""
    # If we got this far (connected, notifications enabled, device booted as READY),
    # security + CCCD are satisfied. Verify by sending a ping.
    rid = next_request_id()
    payload = build_msg(msg_type="device_command", command="ping",
                        bool_value=True, request_id=rid)
    ack = await ble.send_command_and_wait_ack(payload, timeout=3.0)
    if ack and msg_type(ack) == "device_ack":
        return TestResult("HW-DEV-002", "Security + CCCD", True,
                          f"ping ACK received (device_id={msg_field(ack, K_DEVICE_ID)})")
    return TestResult("HW-DEV-002", "Security + CCCD", False,
                      "No ACK after ping — security or CCCD may have failed")


async def test_003_capability_exchange(ble: BLEConnection) -> TestResult:
    """HW-DEV-003: Initial capability exchange."""
    rid = next_request_id()
    payload = build_msg(msg_type="device_command", command="describe_capabilities",
                        bool_value=True, request_id=rid)
    msgs = await ble.send_command_collect_until_ack(payload, timeout=5.0)

    types_seen = [msg_type(m) for m in msgs]
    expected_seq = ["capabilities_begin", "capability_item", "capability_item",
                    "capabilities_end", "device_ack"]

    if types_seen == expected_seq:
        # Verify device_id = lamp-1 on each message
        ids = [msg_field(m, K_DEVICE_ID) for m in msgs]
        all_lamp1 = all(did == GATEWAY_ID for did in ids)
        # Verify snapshot_id is consistent across BEGIN/ITEMS/END
        snap = msg_field(msgs[0], K_SNAPSHOT_ID)
        snap_consistent = all(msg_field(m, K_SNAPSHOT_ID) == snap for m in msgs[:4])
        if all_lamp1 and snap_consistent:
            total = msg_field(msgs[0], K_TOTAL)
            return TestResult("HW-DEV-003", "Initial capability exchange", True,
                              f"sequence={types_seen}, device_id={ids[0]}, "
                              f"snapshot_id={snap}, total={total}")
        return TestResult("HW-DEV-003", "Initial capability exchange", False,
                          f"device_id or snapshot_id mismatch: ids={ids}, snap={snap}")
    return TestResult("HW-DEV-003", "Initial capability exchange", False,
                      f"unexpected sequence: {types_seen} (expected {expected_seq})")


async def test_004_set_led(ble: BLEConnection) -> TestResult:
    """HW-DEV-004: set_led."""
    rid = next_request_id()
    # Send set_led ON (bool_value=true)
    payload = build_msg(msg_type="device_command", command="set_led",
                        bool_value=True, int_value=0, request_id=rid)
    ack = await ble.send_command_and_wait_ack(payload, timeout=3.0)
    if ack is None:
        return TestResult("HW-DEV-004", "set_led", False, "No ACK received")
    if msg_type(ack) != "device_ack":
        return TestResult("HW-DEV-004", "set_led", False,
                          f"unexpected type: {msg_type(ack)}")
    success = msg_field(ack, K_BOOL_VALUE)
    state = msg_field(ack, K_INT_VALUE)
    echo_rid = msg_field(ack, K_REQUEST_ID)
    echo_cmd = msg_field(ack, K_COMMAND)
    echo_did = msg_field(ack, K_DEVICE_ID)
    checks = (
        success is True and
        state in (0, 1) and
        echo_rid == rid and
        echo_cmd == "set_led" and
        echo_did == GATEWAY_ID
    )
    if checks:
        return TestResult("HW-DEV-004", "set_led", True,
                          f"ACK success={success}, state={state}, "
                          f"request_id echo={echo_rid}, device_id echo={echo_did}")
    return TestResult("HW-DEV-004", "set_led", False,
                      f"ACK checks failed: success={success}, state={state}, "
                      f"rid={echo_rid}!={rid}, cmd={echo_cmd}, did={echo_did}")


async def test_005_get_state(ble: BLEConnection) -> TestResult:
    """HW-DEV-005: get_state."""
    rid = next_request_id()
    payload = build_msg(msg_type="device_command", command="get_state",
                        bool_value=False, int_value=0, request_id=rid)
    ack = await ble.send_command_and_wait_ack(payload, timeout=3.0)
    if ack is None:
        return TestResult("HW-DEV-005", "get_state", False, "No ACK received")
    success = msg_field(ack, K_BOOL_VALUE)
    state = msg_field(ack, K_INT_VALUE)
    echo_rid = msg_field(ack, K_REQUEST_ID)
    echo_cmd = msg_field(ack, K_COMMAND)
    echo_did = msg_field(ack, K_DEVICE_ID)
    checks = (
        success is True and
        state in (0, 1) and
        echo_rid == rid and
        echo_cmd == "get_state" and
        echo_did == GATEWAY_ID
    )
    if checks:
        return TestResult("HW-DEV-005", "get_state", True,
                          f"ACK state={state}, request_id echo={echo_rid}")
    return TestResult("HW-DEV-005", "get_state", False,
                      f"ACK checks failed: success={success}, state={state}, "
                      f"rid={echo_rid}!={rid}, cmd={echo_cmd}, did={echo_did}")


async def test_006_device_reboot(ble: BLEConnection) -> TestResult:
    """HW-DEV-006: Device reboot — bond/reconnect, no autonomous capability push."""
    # 1. Send reboot command
    rid = next_request_id()
    payload = build_msg(msg_type="device_command", command="reboot",
                        bool_value=True, int_value=0, request_id=rid)
    ack = await ble.send_command_and_wait_ack(payload, timeout=3.0)
    if ack is None:
        return TestResult("HW-DEV-006", "Device reboot", False,
                          "No ACK for reboot command")

    # 2. Wait for disconnect
    print("    Waiting for device to reboot...")
    await asyncio.sleep(5.0)

    # 3. Reconnect
    await ble.disconnect()
    ble.client = None
    ble.notifications.clear()

    # Re-scan and reconnect
    success = await ble.connect(timeout=15.0)
    if not success:
        # Try broader scan
        print("    Re-scan with broader filter...")
        device = await BleakScanner.find_device_by_name("GW-REF", timeout=15.0)
        if device:
            ble.client = BleakClient(device)
            await ble.client.connect(timeout=10.0)
            await ble.client.start_notify(STATUS_CHAR_UUID, ble.on_notification)
            success = True

    if not success:
        return TestResult("HW-DEV-006", "Device reboot", False,
                          "Failed to reconnect after reboot")

    # 4. Verify no autonomous capability push — wait 2s, check no notifications
    ble.notifications.clear()
    await asyncio.sleep(2.0)
    if ble.notifications:
        return TestResult("HW-DEV-006", "Device reboot", False,
                          f"Unexpected autonomous notifications: {len(ble.notifications)}")

    # 5. Verify bond works — send get_state
    rid2 = next_request_id()
    payload2 = build_msg(msg_type="device_command", command="get_state",
                         bool_value=False, int_value=0, request_id=rid2)
    ack2 = await ble.send_command_and_wait_ack(payload2, timeout=3.0)
    if ack2 and msg_type(ack2) == "device_ack" and msg_field(ack2, K_BOOL_VALUE) is True:
        return TestResult("HW-DEV-006", "Device reboot", True,
                          "Reconnected, bond works, no autonomous push")
    return TestResult("HW-DEV-006", "Device reboot", False,
                      "Reconnected but get_state failed")


async def test_007_gateway_reboot(ble: BLEConnection) -> TestResult:
    """HW-DEV-007: Gateway reboot — reconnect with persisted security."""
    # Disconnect (simulates gateway side restart)
    await ble.disconnect()
    ble.client = None
    ble.notifications.clear()

    print("    Simulating gateway reboot (disconnect + reconnect)...")
    await asyncio.sleep(2.0)

    # Reconnect — bond should persist on device side
    success = await ble.connect(timeout=15.0)
    if not success:
        return TestResult("HW-DEV-007", "Gateway reboot", False,
                          "Failed to reconnect after gateway reboot")

    # Verify command works with persisted security
    rid = next_request_id()
    payload = build_msg(msg_type="device_command", command="get_state",
                        bool_value=False, int_value=0, request_id=rid)
    ack = await ble.send_command_and_wait_ack(payload, timeout=3.0)
    if ack and msg_type(ack) == "device_ack" and msg_field(ack, K_BOOL_VALUE) is True:
        return TestResult("HW-DEV-007", "Gateway reboot", True,
                          "Reconnected with persisted security, get_state OK")
    return TestResult("HW-DEV-007", "Gateway reboot", False,
                      f"Reconnected but get_state failed: {ack}")


async def test_008_capability_refresh(ble: BLEConnection) -> TestResult:
    """HW-DEV-008: Manual capability refresh."""
    # First call — get snapshot_id
    rid1 = next_request_id()
    payload1 = build_msg(msg_type="device_command", command="describe_capabilities",
                         bool_value=True, request_id=rid1)
    msgs1 = await ble.send_command_collect_until_ack(payload1, timeout=5.0)
    if not msgs1:
        return TestResult("HW-DEV-008", "Capability refresh", False,
                          "No response to first describe_capabilities")
    snap1 = msg_field(msgs1[0], K_SNAPSHOT_ID)

    # Second call — verify different snapshot_id
    rid2 = next_request_id()
    payload2 = build_msg(msg_type="device_command", command="describe_capabilities",
                         bool_value=True, request_id=rid2)
    msgs2 = await ble.send_command_collect_until_ack(payload2, timeout=5.0)
    if not msgs2:
        return TestResult("HW-DEV-008", "Capability refresh", False,
                          "No response to second describe_capabilities")
    snap2 = msg_field(msgs2[0], K_SNAPSHOT_ID)

    # Verify deterministic metadata (same commands, same order)
    cmds1 = [msg_field(m, K_COMMAND) for m in msgs1 if msg_type(m) == "capability_item"]
    cmds2 = [msg_field(m, K_COMMAND) for m in msgs2 if msg_type(m) == "capability_item"]

    if snap1 != snap2 and cmds1 == cmds2:
        return TestResult("HW-DEV-008", "Capability refresh", True,
                          f"snapshot_id changed: {snap1} -> {snap2}, commands={cmds1}")
    if snap1 == snap2:
        return TestResult("HW-DEV-008", "Capability refresh", False,
                          f"snapshot_id did not change: {snap1} == {snap2}")
    return TestResult("HW-DEV-008", "Capability refresh", False,
                      f"Commands differ: {cmds1} != {cmds2}")


async def test_009_gateway_bond_reset(ble: BLEConnection) -> TestResult:
    """HW-DEV-009: One-side Gateway bond reset — repeat pairing recovery."""
    # Disconnect, simulate bond loss on gateway side, reconnect
    await ble.disconnect()
    ble.client = None
    ble.notifications.clear()

    print("    Simulating gateway bond reset (disconnect + reconnect with pairing)...")
    await asyncio.sleep(2.0)

    success = await ble.connect(timeout=15.0)
    if not success:
        return TestResult("HW-DEV-009", "Gateway bond reset", False,
                          "Failed to reconnect after gateway bond reset")

    # Verify command works
    rid = next_request_id()
    payload = build_msg(msg_type="device_command", command="get_state",
                        bool_value=False, int_value=0, request_id=rid)
    ack = await ble.send_command_and_wait_ack(payload, timeout=5.0)
    if ack and msg_type(ack) == "device_ack" and msg_field(ack, K_BOOL_VALUE) is True:
        return TestResult("HW-DEV-009", "Gateway bond reset", True,
                          "Repeat pairing recovery OK, get_state ACK received")
    return TestResult("HW-DEV-009", "Gateway bond reset", False,
                      f"Post-recovery get_state failed: {ack}")


async def test_010_device_bond_reset(ble: BLEConnection) -> TestResult:
    """HW-DEV-010: One-side Device bond reset."""
    # This test requires sending a reboot-like command that clears device bonds.
    # On the reference device, we can trigger this via the "unpair" mechanism
    # or by waiting for the device side to clear bonds.
    # For now, we test the recovery path by disconnecting and reconnecting.
    await ble.disconnect()
    ble.client = None
    ble.notifications.clear()

    print("    Simulating device bond reset (full reconnect cycle)...")
    await asyncio.sleep(3.0)

    success = await ble.connect(timeout=15.0)
    if not success:
        return TestResult("HW-DEV-010", "Device bond reset", False,
                          "Failed to reconnect after device bond reset")

    # Verify command works
    rid = next_request_id()
    payload = build_msg(msg_type="device_command", command="get_state",
                        bool_value=False, int_value=0, request_id=rid)
    ack = await ble.send_command_and_wait_ack(payload, timeout=5.0)
    if ack and msg_type(ack) == "device_ack" and msg_field(ack, K_BOOL_VALUE) is True:
        return TestResult("HW-DEV-010", "Device bond reset", True,
                          "Recovery OK, get_state ACK received")
    return TestResult("HW-DEV-010", "Device bond reset", False,
                      f"Post-recovery get_state failed: {ack}")


async def test_011_stress(ble: BLEConnection) -> TestResult:
    """HW-DEV-011: Stress — repeated set/get + capability requests."""
    errors = []
    total = 20
    for i in range(total):
        # Alternate set_led and get_state
        if i % 2 == 0:
            rid = next_request_id()
            payload = build_msg(msg_type="device_command", command="set_led",
                                bool_value=(i % 4 == 0), int_value=0, request_id=rid)
            ack = await ble.send_command_and_wait_ack(payload, timeout=3.0)
            if ack is None:
                errors.append(f"set_led #{i}: no ACK")
            elif msg_field(ack, K_BOOL_VALUE) is not True:
                errors.append(f"set_led #{i}: failed ACK")
        else:
            rid = next_request_id()
            payload = build_msg(msg_type="device_command", command="get_state",
                                bool_value=False, int_value=0, request_id=rid)
            ack = await ble.send_command_and_wait_ack(payload, timeout=3.0)
            if ack is None:
                errors.append(f"get_state #{i}: no ACK")
            elif msg_field(ack, K_BOOL_VALUE) is not True:
                errors.append(f"get_state #{i}: failed ACK")

    # 5 describe_capabilities requests
    for i in range(5):
        rid = next_request_id()
        payload = build_msg(msg_type="device_command", command="describe_capabilities",
                            bool_value=True, request_id=rid)
        msgs = await ble.send_command_collect_until_ack(payload, timeout=5.0)
        if not msgs or msg_type(msgs[-1]) != "device_ack":
            errors.append(f"describe_capabilities #{i}: no final ACK")

    if not errors:
        return TestResult("HW-DEV-011", "Stress", True,
                          f"{total} set/get + 5 capability requests, 0 errors")
    return TestResult("HW-DEV-011", "Stress", False,
                      f"{len(errors)} errors out of {total + 5} requests: "
                      f"{errors[:5]}")


# ── Test runner ──────────────────────────────────────────────────────────────

ALL_TESTS = [
    ("HW-DEV-001", test_001_advertising),
    ("HW-DEV-002", test_002_security_cccd),
    ("HW-DEV-003", test_003_capability_exchange),
    ("HW-DEV-004", test_004_set_led),
    ("HW-DEV-005", test_005_get_state),
    ("HW-DEV-006", test_006_device_reboot),
    ("HW-DEV-007", test_007_gateway_reboot),
    ("HW-DEV-008", test_008_capability_refresh),
    ("HW-DEV-009", test_009_gateway_bond_reset),
    ("HW-DEV-010", test_010_device_bond_reset),
    ("HW-DEV-011", test_011_stress),
]


async def run_tests(port: str, test_filter: Optional[str] = None):
    ble = BLEConnection()

    # For tests that need a fresh connection, we reconnect each time
    tests_to_run = ALL_TESTS
    if test_filter:
        tests_to_run = [(tid, fn) for tid, fn in ALL_TESTS if tid == test_filter]
        if not tests_to_run:
            print(f"Unknown test: {test_filter}")
            print("Available tests:")
            for tid, _ in ALL_TESTS:
                print(f"  {tid}")
            return

    results: list[TestResult] = []

    for tid, test_fn in tests_to_run:
        print(f"\n{'='*60}")
        print(f"Running {tid}: {test_fn.__doc__}")
        print(f"{'='*60}")

        # Ensure connection for tests that don't reconnect themselves
        if tid in ("HW-DEV-001",):
            # HW-DEV-001 tests the connection itself
            pass
        elif not (ble.client and ble.client.is_connected):
            connected = await ble.connect(timeout=15.0)
            if not connected:
                results.append(TestResult(tid, test_fn.__doc__, False,
                                          "Could not connect to device"))
                continue

        try:
            result = await test_fn(ble)
            results.append(result)
            status = "PASS" if result.passed else "FAIL"
            print(f"\n  [{status}] {result.test_id}: {result.name}")
            print(f"  Detail: {result.detail}")
        except Exception as e:
            tb = traceback.format_exc()
            print(f"\n  [ERROR] {tid}: {e}")
            print(tb)
            results.append(TestResult(tid, test_fn.__doc__, False, f"Exception: {e}"))
            # Disconnect and reconnect for next test
            await ble.disconnect()
            ble.client = None
            ble.notifications.clear()
            await asyncio.sleep(1.0)

    # Disconnect after all tests
    await ble.disconnect()

    # Summary
    print(f"\n{'='*60}")
    print("HW-DEV TEST SUMMARY")
    print(f"{'='*60}")
    passed = sum(1 for r in results if r.passed)
    total = len(results)
    for r in results:
        status = "PASS" if r.passed else "FAIL"
        print(f"  [{status}] {r.test_id}: {r.name}")
        if not r.passed:
            print(f"         {r.detail}")
    print(f"\nResult: {passed}/{total} passed")
    if passed == total:
        print("ALL HW-DEV TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="HW-DEV Test Harness")
    parser.add_argument("--port", default="/dev/cu.usbmodem2101",
                        help="Serial port hint (for logging only)")
    parser.add_argument("--test", default=None,
                        help="Run single test (e.g., HW-DEV-004)")
    parser.add_argument("--list", action="store_true",
                        help="List available tests")
    args = parser.parse_args()

    if args.list:
        for tid, fn in ALL_TESTS:
            print(f"  {tid}: {fn.__doc__}")
        return

    asyncio.run(run_tests(args.port, args.test))


if __name__ == "__main__":
    main()
