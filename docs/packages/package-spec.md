# CANcestry Package Specification

Version: 0.2.1

## 1. Package Structure

A package may contain:

    cancestry.toml
    maps/
    recipes/
    states/
    tests/

## 2. Manifest Fields

The manifest shall include:

- schema_version
- package
- runtime
- bindings
- dependencies
- capabilities
- limits

## 3. Package Identity

    [package]
    name = "example_speed_adapter"
    version = "0.1.0"
    type = "emulation"

Allowed package types:

- codec
- recipe
- emulation
- gateway
- test

## 4. Runtime Compatibility

    [runtime]
    min_cancestry_version = "0.2.0"

## 5. Bindings

Package-level interface aliases are optional.

    [bindings]
    car = "can0"
    device = "can1"

Interface resolution order:

1. physical interface name in capabilities,
2. package-level alias in bindings,
3. invalid.

FSM instance bindings override package-level bindings for that instance.

## 6. Dependencies

    [[dependencies.codec_maps]]
    name = "example_signals"
    version = "^0.1.0"

Dependency rules:

- semantic version constraints are required,
- missing dependencies are fatal,
- cyclic dependencies are invalid,
- dependencies load before dependents.

## 7. Capabilities

Capabilities shall explicitly declare:

- interfaces,
- RX permission,
- TX permission,
- RX ID filters,
- TX ID allowlists,
- signal read allowlists,
- signal write allowlists,
- rate limits.

Example:

    [capabilities.signals]
    read = ["VehicleSpeed", "IgnitionState"]
    write = ["SpeedMPH", "ClusterAlive"]

    [capabilities.interfaces.can0]
    rx = true
    tx = false
    rx_ids = [0x1A0, 0x320]

    [capabilities.interfaces.can1]
    rx = false
    tx = true
    tx_ids = [0x300, 0x280]
    max_tx_per_second = 50
    max_tx_burst = 10

## 8. Integrity and Trust

Packages shall include SHA-256 digests for all files.

Signatures may use Ed25519.

Trust levels:

- unsigned
- community
- verified
- maintainer

Unsigned packages shall not be enabled in ACTIVE mode unless explicitly overridden locally.
