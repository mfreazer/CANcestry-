#!/usr/bin/env python3
"""CANcestry Phase 12 HIL fault-injection runner.

The default backend is a deterministic software model of the hardware boundary.
It intentionally models the hardware decision before the software decision:
CAN CRC rejection never enters the software queue, bus-off is asserted by the
controller before the FSM enters SAFE_STATE, and a BOR latches zero torque and
opens the contactor before the simulated MCU powers down.

Implements: SW-FR-HIL-001..006.
Test ids: HIL-BUSOFF-001, HIL-CRC-001, HIL-BROWNOUT-001.

A real HIL adapter can use the same scenario contract by replacing
``SoftwareHilBackend``.  The runner has no dependency on NI VeriStand, dSPACE,
SocketCAN, QEMU, or a Python CAN package, which keeps the evidence reproducible
in CI and makes the order of hardware-first assertions explicit.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Dict, List, Optional


class FaultKind(str, Enum):
    """Physical anomaly injected at the modeled hardware boundary."""

    BUS_OFF = "bus_off"
    CRC_ERROR = "crc_error"
    BROWNOUT = "brownout"


class GatewayState(str, Enum):
    """Safety state observed by the gateway FSM."""

    ACTIVE = "ACTIVE"
    SAFE_STATE = "SAFE_STATE"
    RECOVERING = "RECOVERING"
    POWERED_OFF = "POWERED_OFF"


@dataclass(frozen=True)
class CanFrame:
    """A small physical CAN frame used by the software HIL model."""

    can_id: int
    data: bytes
    crc_valid: bool = True


@dataclass(frozen=True)
class TraceEntry:
    """One deterministic hardware/software observation."""

    sequence: int
    time_ms: int
    source: str
    event: str
    details: Dict[str, object] = field(default_factory=dict)


@dataclass
class HardwareStatus:
    """State that would be read from the controller and power supervisor."""

    bus_off: bool = False
    crc_rejections: int = 0
    bor_triggered: bool = False
    iwdg_triggered: bool = False
    torque_nm: int = 0
    contactors_closed: bool = False
    tx_authorized: bool = False
    mcu_running: bool = True
    reset_count: int = 0


class SoftwareHilBackend:
    """High-fidelity, deterministic hardware boundary for host HIL evidence.

    The backend is deliberately small but preserves the safety ordering that a
    physical controller must provide.  It is a simulation backend, not a
    replacement for target acceptance: the report identifies it as such and
    provides the adapter seam for a target or QEMU run.
    """

    def __init__(
        self,
        *,
        nominal_voltage_mv: int = 400_000,
        bor_threshold_mv: int = 270_000,
        bus_off_recovery_ms: int = 5,
        iwdg_timeout_ms: int = 50,
    ) -> None:
        if nominal_voltage_mv <= 0 or bor_threshold_mv <= 0:
            raise ValueError("voltage thresholds must be positive")
        if bor_threshold_mv >= nominal_voltage_mv:
            raise ValueError("BOR threshold must be below nominal voltage")
        if bus_off_recovery_ms <= 0 or iwdg_timeout_ms <= 0:
            raise ValueError("timeouts must be positive")
        self.nominal_voltage_mv = nominal_voltage_mv
        self.bor_threshold_mv = bor_threshold_mv
        self.bus_off_recovery_ms = bus_off_recovery_ms
        self.iwdg_timeout_ms = iwdg_timeout_ms
        self.voltage_mv = nominal_voltage_mv
        self.status = HardwareStatus()
        self.time_ms = 0
        self._bus_off_at_ms: Optional[int] = None
        self._watchdog_last_feed_ms = 0
        self._trace: List[TraceEntry] = []
        self._sequence = 0

    @property
    def trace(self) -> List[TraceEntry]:
        """Return a copy so callers cannot mutate the evidence sequence."""

        return list(self._trace)

    def _record(self, source: str, event: str, **details: object) -> None:
        self._sequence += 1
        self._trace.append(
            TraceEntry(self._sequence, self.time_ms, source, event, details)
        )

    def boot_active(self) -> None:
        """Put the simulated hardware in its nominal, authorized test state."""

        if not self.status.mcu_running:
            raise RuntimeError("cannot activate a powered-off MCU")
        self.status.tx_authorized = True
        self.status.contactors_closed = True
        self.status.torque_nm = 100
        self._watchdog_last_feed_ms = self.time_ms
        self._record("hardware", "BOOT_ACTIVE", voltage_mv=self.voltage_mv)

    def inject_bus_off(self) -> None:
        """Force the CAN controller into hardware Bus-Off."""

        if not self.status.mcu_running:
            raise RuntimeError("cannot inject bus-off after power-down")
        self.status.bus_off = True
        self.status.tx_authorized = False
        self._bus_off_at_ms = self.time_ms
        self._record("hardware", "BUS_OFF_ASSERTED", controller="CAN1")

    def inject_crc_error(self, frame: CanFrame) -> bool:
        """Apply controller CRC validation and return hardware admission.

        The return value is deliberately *before* the software boundary.  A
        false value means the frame was rejected in the CAN controller and is
        impossible for the FSM to process.
        """

        if not self.status.mcu_running or self.status.bus_off:
            self._record("hardware", "FRAME_REJECTED_CONTROLLER_UNAVAILABLE")
            return False
        if not frame.crc_valid:
            self.status.crc_rejections += 1
            self._record(
                "hardware",
                "CAN_CRC_REJECTED",
                can_id=frame.can_id,
                crc_valid=False,
            )
            return False
        self._record("hardware", "CAN_FRAME_ACCEPTED", can_id=frame.can_id)
        return True

    def inject_brownout(self, voltage_mv: int) -> None:
        """Drop supply voltage and apply the BOR safe latch before reset."""

        if voltage_mv < 0:
            raise ValueError("voltage cannot be negative")
        self.voltage_mv = voltage_mv
        self._record("hardware", "SUPPLY_DROP", voltage_mv=voltage_mv)
        if voltage_mv <= self.bor_threshold_mv and self.status.mcu_running:
            # Hardware is first: GPIO/contactors are safe before the core sees
            # a reset and before the MCU is marked powered down.
            self.status.bor_triggered = True
            self.status.torque_nm = 0
            self.status.contactors_closed = False
            self.status.tx_authorized = False
            self._record(
                "hardware",
                "BOR_SAFE_LATCH",
                torque_nm=0,
                contactors_closed=False,
            )
            self.status.mcu_running = False
            self.status.reset_count += 1
            self._record("hardware", "MCU_POWERED_DOWN", reset="brownout")

    def feed_watchdog(self) -> None:
        """Feed the modeled IWDG while the MCU is running."""

        if self.status.mcu_running:
            self._watchdog_last_feed_ms = self.time_ms
            self._record("hardware", "IWDG_FED")

    def tick(self, delta_ms: int, *, feed_watchdog: bool = False) -> None:
        """Advance deterministic hardware time and recovery supervisors."""

        if delta_ms < 0:
            raise ValueError("time cannot move backwards")
        self.time_ms += delta_ms

        if self.status.mcu_running and feed_watchdog:
            self.feed_watchdog()
        if (
            self.status.mcu_running
            and self.time_ms - self._watchdog_last_feed_ms >= self.iwdg_timeout_ms
        ):
            self.status.iwdg_triggered = True
            self.status.torque_nm = 0
            self.status.contactors_closed = False
            self.status.tx_authorized = False
            self._record("hardware", "IWDG_SAFE_LATCH", torque_nm=0)
            self.status.mcu_running = False
            self.status.reset_count += 1
            self._record("hardware", "MCU_POWERED_DOWN", reset="watchdog")

        if (
            self.status.mcu_running
            and self.status.bus_off
            and self._bus_off_at_ms is not None
            and self.time_ms - self._bus_off_at_ms >= self.bus_off_recovery_ms
        ):
            # This represents the controller's automatic recovery protocol,
            # not a software rewrite of a corrupted frame.
            self.status.bus_off = False
            self._bus_off_at_ms = None
            self._record("hardware", "BUS_OFF_RECOVERED", controller="CAN1")


class SimulatedGateway:
    """Minimal FSM boundary wired to the simulated hardware backend."""

    def __init__(self, hardware: Optional[SoftwareHilBackend] = None) -> None:
        self.hardware = hardware or SoftwareHilBackend()
        self.state = GatewayState.ACTIVE
        self.software_queue: List[CanFrame] = []
        self.processed_frames = 0
        self.safe_state_transitions = 0
        self.recovery_count = 0
        self._healthy_recovery_ticks = 0
        self.hardware.boot_active()

    @property
    def trace(self) -> List[TraceEntry]:
        return self.hardware.trace

    def _software_record(self, event: str, **details: object) -> None:
        self.hardware._record("software", event, **details)

    def _enter_safe_state(self, reason: str) -> None:
        if self.state != GatewayState.SAFE_STATE:
            self.state = GatewayState.SAFE_STATE
            self.safe_state_transitions += 1
            self._software_record("FSM_SAFE_STATE", reason=reason)
        self.hardware.status.tx_authorized = False
        self.hardware.status.torque_nm = 0
        self.hardware.status.contactors_closed = False

    def tick(self, delta_ms: int) -> None:
        """Run one bounded main-loop observation after hardware time advances."""

        self.hardware.tick(delta_ms)
        if not self.hardware.status.mcu_running:
            self._enter_safe_state("hardware_reset")
            self.state = GatewayState.POWERED_OFF
            return
        if self.hardware.status.bus_off:
            self._enter_safe_state("bus_off")
            self._healthy_recovery_ticks = 0
            return
        if self.state == GatewayState.SAFE_STATE:
            self.state = GatewayState.RECOVERING
            self._healthy_recovery_ticks = 1
            self._software_record("FSM_RECOVERY_STARTED")
            return
        if self.state == GatewayState.RECOVERING:
            self._healthy_recovery_ticks += 1
            if self._healthy_recovery_ticks >= 2:
                self.state = GatewayState.ACTIVE
                self.recovery_count += 1
                self.hardware.status.tx_authorized = True
                self.hardware.status.contactors_closed = True
                self.hardware.status.torque_nm = 100
                self._software_record("FSM_RECOVERY_COMPLETE")

    def receive(self, frame: CanFrame) -> bool:
        """Pass a frame through hardware CRC, then the software queue."""

        admitted = self.hardware.inject_crc_error(frame)
        if not admitted:
            # HIL-CRC-001's key invariant: no malformed frame reaches this
            # queue, so no software/FSM workaround can accidentally process it.
            return False
        if self.state != GatewayState.ACTIVE:
            self._software_record("FRAME_BLOCKED_SAFE_STATE", can_id=frame.can_id)
            return False
        self.software_queue.append(frame)
        self.processed_frames += 1
        self._software_record("FSM_PROCESSED_FRAME", can_id=frame.can_id)
        return True

    def inject_bus_off(self) -> None:
        self.hardware.inject_bus_off()

    def inject_brownout(self, voltage_mv: int) -> None:
        self.hardware.inject_brownout(voltage_mv)


@dataclass(frozen=True)
class ScenarioResult:
    """Machine-readable verification result for one injected fault."""

    test_id: str
    fault: str
    passed: bool
    assertions: List[str]
    metrics: Dict[str, object]
    trace: List[Dict[str, object]]


class HilFaultInjectionRunner:
    """Run deterministic Phase 12 fault scenarios and produce evidence."""

    def __init__(self, backend: Optional[SoftwareHilBackend] = None) -> None:
        self.backend = backend

    @staticmethod
    def _trace_dict(entries: List[TraceEntry]) -> List[Dict[str, object]]:
        return [asdict(entry) for entry in entries]

    def run_bus_off_recovery(self) -> ScenarioResult:
        """HIL-BUSOFF-001: hardware Bus-Off -> SAFE_STATE -> auto recovery."""

        gateway = SimulatedGateway(self.backend or SoftwareHilBackend())
        gateway.inject_bus_off()
        gateway.tick(1)
        safe_seen = gateway.state == GatewayState.SAFE_STATE
        tx_blocked = not gateway.hardware.status.tx_authorized
        gateway.tick(gateway.hardware.bus_off_recovery_ms)
        recovered_seen = not gateway.hardware.status.bus_off
        gateway.tick(1)
        gateway.tick(1)
        active_seen = gateway.state == GatewayState.ACTIVE
        automatic_recovery = gateway.recovery_count == 1
        assertions = [
            "CAN controller asserted BUS_OFF before the FSM decision",
            "FSM entered SAFE_STATE and revoked transmission",
            "controller automatic recovery cleared BUS_OFF",
            "FSM completed bounded recovery before re-authorizing transmission",
        ]
        passed = safe_seen and tx_blocked and recovered_seen and active_seen and automatic_recovery
        return ScenarioResult(
            "HIL-BUSOFF-001",
            FaultKind.BUS_OFF.value,
            passed,
            assertions,
            {
                "safe_state_transitions": gateway.safe_state_transitions,
                "recovery_count": gateway.recovery_count,
                "tx_authorized_after_recovery": gateway.hardware.status.tx_authorized,
                "bus_off": gateway.hardware.status.bus_off,
            },
            self._trace_dict(gateway.trace),
        )

    def run_crc_rejection(self) -> ScenarioResult:
        """HIL-CRC-001: corrupted frame is rejected by hardware CRC."""

        gateway = SimulatedGateway(self.backend or SoftwareHilBackend())
        before_processed = gateway.processed_frames
        frame = CanFrame(can_id=0x180, data=b"\xDE\xAD\xBE\xEF", crc_valid=False)
        admitted = gateway.receive(frame)
        assertions = [
            "hardware CRC rejected the corrupted frame",
            "rejected frame did not enter the software queue",
            "FSM processed-frame count did not change",
        ]
        passed = (
            not admitted
            and gateway.hardware.status.crc_rejections == 1
            and len(gateway.software_queue) == 0
            and gateway.processed_frames == before_processed
        )
        return ScenarioResult(
            "HIL-CRC-001",
            FaultKind.CRC_ERROR.value,
            passed,
            assertions,
            {
                "crc_rejections": gateway.hardware.status.crc_rejections,
                "software_queue_depth": len(gateway.software_queue),
                "processed_frames": gateway.processed_frames,
            },
            self._trace_dict(gateway.trace),
        )

    def run_brownout(self) -> ScenarioResult:
        """HIL-BROWNOUT-001: BOR safe latch precedes MCU power-down."""

        gateway = SimulatedGateway(self.backend or SoftwareHilBackend())
        gateway.inject_brownout(gateway.hardware.bor_threshold_mv)
        entries = gateway.trace
        latch_index = next(
            index for index, entry in enumerate(entries) if entry.event == "BOR_SAFE_LATCH"
        )
        power_down_index = next(
            index for index, entry in enumerate(entries) if entry.event == "MCU_POWERED_DOWN"
        )
        safe_before_power_down = latch_index < power_down_index
        hardware_safe = (
            gateway.hardware.status.torque_nm == 0
            and not gateway.hardware.status.contactors_closed
            and not gateway.hardware.status.tx_authorized
        )
        gateway.tick(0)
        assertions = [
            "BOR asserted at or below the configured threshold",
            "hardware latched zero torque and open contactors before power-down",
            "MCU stopped only after the safe latch was recorded",
        ]
        passed = (
            gateway.hardware.status.bor_triggered
            and not gateway.hardware.status.mcu_running
            and safe_before_power_down
            and hardware_safe
            and gateway.state == GatewayState.POWERED_OFF
        )
        return ScenarioResult(
            "HIL-BROWNOUT-001",
            FaultKind.BROWNOUT.value,
            passed,
            assertions,
            {
                "bor_triggered": gateway.hardware.status.bor_triggered,
                "voltage_mv": gateway.hardware.voltage_mv,
                "bor_threshold_mv": gateway.hardware.bor_threshold_mv,
                "torque_nm": gateway.hardware.status.torque_nm,
                "contactors_closed": gateway.hardware.status.contactors_closed,
                "mcu_running": gateway.hardware.status.mcu_running,
            },
            self._trace_dict(gateway.trace),
        )

    def run(self, scenario: str = "all") -> List[ScenarioResult]:
        """Run one named scenario or all scenarios in stable order."""

        runners = {
            FaultKind.BUS_OFF.value: self.run_bus_off_recovery,
            FaultKind.CRC_ERROR.value: self.run_crc_rejection,
            FaultKind.BROWNOUT.value: self.run_brownout,
        }
        if scenario == "all":
            selected = [runners[key] for key in ("bus_off", "crc_error", "brownout")]
        elif scenario in runners:
            selected = [runners[scenario]]
        else:
            raise ValueError("scenario must be all, bus_off, crc_error, or brownout")
        return [runner() for runner in selected]


def _result_as_dict(result: ScenarioResult) -> Dict[str, object]:
    return asdict(result)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run CANcestry hardware-first HIL fault-injection scenarios"
    )
    parser.add_argument(
        "--scenario",
        choices=("all", "bus_off", "crc_error", "brownout"),
        default="all",
        help="fault scenario to execute (default: all)",
    )
    parser.add_argument(
        "--backend",
        choices=("simulation",),
        default="simulation",
        help="backend; simulation is the deterministic CI/QEMU substitute",
    )
    parser.add_argument(
        "--json-output",
        type=Path,
        help="write the complete deterministic evidence report to this path",
    )
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    # The explicit backend option is an adapter seam.  A target adapter can be
    # registered here without changing the scenario assertions or report.
    runner = HilFaultInjectionRunner(SoftwareHilBackend())
    try:
        results = runner.run(args.scenario)
    except (RuntimeError, ValueError, StopIteration) as error:
        print("HIL FAIL: %s" % error, file=sys.stderr)
        return 1

    report = {
        "backend": args.backend,
        "simulation": True,
        "results": [_result_as_dict(result) for result in results],
    }
    if args.json_output is not None:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        print("[%s] %s %s" % (status, result.test_id, result.fault))
        for assertion in result.assertions:
            print("  - %s" % assertion)
    return 0 if all(result.passed for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
