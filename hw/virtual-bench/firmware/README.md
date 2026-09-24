# T2 virtual-bench firmware ELF (off-tree, H-08 / issue #55)

This directory contains the **off-tree driver** that turns the tag `v1.0.0`
firmware sources into the standalone executable Renode runs for the T2
virtual bench. It is NOT part of the v1.0.0 firmware tree and changes here
never modify firmware (issue constraint: firmware read-only).

## What the ELF is

`build_firmware.sh` compiles, unmodified and at `-O0`:

* the tag `v1.0.0` sources: `core/event/src/{fault,queue,types,clock,
  isr_queue}.c`, `core/hal/src/{hal,types}.c`, `platform/cortex_m/{
  watchdog,hal_stm32,startup,alloc_stubs}.c`
* this directory: `startup.s` (vector table + reset handler; the v1.0.0
  platform ships no vector table because production links against vendor
  boot code) and `main.c` (the T2 driver)

…with the tag's own `platform/cortex_m/cancestry_baremetal.ld` (memory map
and zero-allocation link assertions), and links a standalone ELF.

## What `main.c` does

1. Post-reset boot (detected via the IWDG reset-cause flag in `RCC_CSR`,
   which the platform model persists across the scripted IWDG reset exactly
   as on real STM32G4 until RMVF): park in a safe-state nop spin without
   touching any event-capture slot (the H-07 capture contract requires each
   event exactly once).
2. First boot: arm the IWDG directly with the identical register sequence
   and default timeout as `cancestry_watchdog_init` (KR 0x5555, PR 3,
   RLR 50, KR 0xAAAA, KR 0xCCCC). `cancestry_watchdog_init` itself is NOT
   called because it enforces the safe state at boot, which would stamp a
   `safe_latch` event before the scenario starts.
3. Then invoke the REAL `cancestry_event_hard_fault_escalate` wrapper with
   the real firmware hooks:
   * `write_retention_register` = `cancestry_watchdog_write_retention_register`
   * `hal_fail_safe` = omitted — the production fail-safe hook's effect is
     the safe latch, which `cancestry_watchdog_escalate` asserts by design
     (the deliberate double-latch documented in `watchdog.c`); binding it
     as well would trip the platform's exactly-once capture contract
   * `iwdg_escalate` = T2 adapter: deterministic bounded delay, the real
     `cancestry_watchdog_escalate` (latch + IWDG RLR=1), then poll
     `IWDG_SR` until the scripted IWDG reset completes (the scripted model
     evaluates expiry on bus access; real silicon fires asynchronously)

The Renode `cpu AddSymbolHook`s in `../renode/cancestry-hw.resc` then stamp
the three event timestamps (retention write, safe latch, escalation) from
emulation virtual time; the IWDG model stamps the fire time at expiry. The
driver is compiled at `-O0` (no inlining) precisely so those symbol hooks
observe real function entries.

## Building (CI)

The `hw-nightly` workflow job `t2-virtual-bench` does:

```sh
git archive --format=tar v1.0.0 | tar -x -C /tmp/fw-tag        # tag sources
CANCESTRY_T2_FW_TAG_DIR=/tmp/fw-tag \
CANCESTRY_T2_ELF_OUT=build/hw/t2_firmware_v1.0.0.elf \
CANCESTRY_T2_BUILD_LOG=build/hw/t2_firmware_build.log \
  hw/virtual-bench/firmware/build_firmware.sh
```

inside the pinned T2 toolchain image (`ci/docker/Dockerfile.t2`). The build
log (compiler identity, source list, ELF sha256) travels with the run
evidence in the runlog.
