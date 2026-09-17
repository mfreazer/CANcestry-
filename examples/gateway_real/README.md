# gateway_real

Physical CAN gateway harness (Phase 6, issue #17).

This example binds the CANcestry core to Linux SocketCAN instead of the
mock gateway harness. It demonstrates the full RX -> Decode -> FSM/Recipe ->
Encode -> TX loop over real CAN interfaces (or `vcan0` for host testing).

## Prerequisites

1. Linux host with SocketCAN support.
2. One or two `vcan` interfaces set up for loopback testing:

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link add dev vcan1 type vcan
sudo ip link set up vcan0
sudo ip link set up vcan1
```

3. Build CANcestry on the host (not cross-compiled):

```bash
mkdir -p build && cd build
cmake .. -DCANCESTRY_BUILD_EXAMPLES=ON
make cancestry_gateway_real
```

4. Run the example:

```bash
./examples/gateway_real/cancestry_gateway_real vcan0 vcan1
```

The program sends a known VehicleStatus frame on `vcan0`, lets the codec
decode it, runs the recipe/FSM loop, and transmits the mirrored
ClusterDisplay frame on `vcan1`. You can observe the loopback traffic with
`candump vcan0 vcan1` from the `can-utils` package.

## CAN FD demonstration (Phase 7, issue #20)

After the classic loop the example runs a CAN FD round trip on the first
interface (`HAL-REAL-CANFD-001`):

1. it shows the load-time capability gate - the CAN FD codec map
   (`can_fd: true`, `dlc: 64`) is refused with `ERR_UNSUPPORTED` when the
   declared platform capabilities say classic-only;
2. it opens the interface with `can_fd` requested and reads back what was
   actually negotiated through `cancestry_hal_iface_can_fd()`;
3. if CAN FD was negotiated it builds a 64-byte frame with a signal in the
   last payload byte, pushes it through HAL -> `core/event` -> codec decode,
   verifies the decoded value, and transmits the frame with `CANFD_MTU`.

If the interface does not provide CAN FD the demonstration prints a SKIP with
the exact `ip link` command that enables it and exits 0 - the fallback is
deterministic, never a truncated frame (SW-FR-CANFD-003):

```bash
sudo ip link set vcan0 type can bitrate 500000 dbitrate 2000000 fd on
```

Expected output on an FD-capable interface:

```text
CAN FD demonstration on vcan0
  classic-caps load refused: ERR_UNSUPPORTED
  fd_event=yes decoded=yes tx_sent=1
  CAN FD: PASS (64-byte payload through HAL, core/event and codec)
```

## Safety & determinism

The HAL poll loop is strictly non-blocking (SW-FR-HAL-003). Hardware
timestamps are captured with SO_TIMESTAMPNS and mapped to the CANcestry
monotonic clock in microseconds (SW-FR-HAL-004). Any bus error (Error
Passive, Bus Off, dropped frame, bad file descriptor) is raised as a
`FAULT_RAISED` event and the system moves toward SAFE mode without crashing
(fail-closed, SW-FR-HAL-005/SYS-SF-002).

## Requirement trace

| Test/Artifact | Requirements |
|---------------|--------------|
| gateway_real binary         | SW-FR-HAL-010, SW-FR-HAL-003, SW-FR-HAL-004, SW-FR-HAL-005 |
| cancestry_gateway_real_loop | HAL-REAL-LOOP-001 (RX->Decode->Encode->TX loopback) |
| CAN FD demonstration        | HAL-REAL-CANFD-001, SW-FR-CANFD-003, SW-FR-CANFD-004, SW-FR-CANFD-005 |
