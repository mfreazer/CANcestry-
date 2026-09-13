# CANcestry Logging and Replay Specification

Version: 0.2.1

## 1. Canonical Log Format

The canonical exchange format is JSON Lines.

File extension:

    .canl.jsonl

Each line is one event.

## 2. CAN Frame Event

Example:

    {
      "v": 1,
      "ts_us": 123456,
      "seq": 42,
      "type": "can_rx",
      "if": "can0",
      "id": 416,
      "dlc": 8,
      "data": "0011223344556677",
      "flags": 0
    }

## 3. Signal Event

Example:

    {
      "v": 1,
      "ts_us": 123457,
      "seq": 43,
      "type": "signal",
      "name": "VehicleSpeed",
      "value": 42.3,
      "quality": "good"
    }

## 4. FSM Transition Event

Example:

    {
      "v": 1,
      "ts_us": 123458,
      "seq": 44,
      "type": "fsm_transition",
      "instance": "power0",
      "from": "OFF",
      "to": "ACTIVE",
      "trigger": "signal_changed"
    }

## 5. Timestamp Base

Timestamps are monotonic microseconds since boot.

Logs shall include a boot identifier where available.

Wall-clock time may be recorded as metadata but shall not be required.

## 6. Retention and Privacy

- Logs shall be stored locally by default.
- Logs shall be clearable by the user.
- Cloud connectivity shall not be required.
- Log export shall be explicit.

## 7. Replay Semantics

Replay shall:

- preserve event order,
- preserve relative timestamps,
- support time scaling,
- preserve sequence numbers,
- inject events into the same event model used by live hardware.

Simulation time shall start at the first event timestamp unless overridden.
