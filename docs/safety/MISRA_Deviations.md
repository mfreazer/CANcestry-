# MISRA C:2012 Deviation Record

| Field | Value |
|---|---|
| Scope | `core/` and `platform/` C translation units |
| Standard | MISRA C:2012, with the Amendment 1 essential-type model where supported by the checker |
| Tool driver | [`formal/misra/run_cppcheck.sh`](../../formal/misra/run_cppcheck.sh) |
| Rule set | cppcheck `--enable=all --inconclusive --std=c99 --addon=misra.py --error-exitcode=1` |
| Status | Phase 9 baseline; every deviation below requires reviewer approval before an ASIL-B production release |
| Last review | 2026-09-18 |

This is a deviation record, not a suppression list. The driver does not hide
project findings. A checker finding must either be corrected or mapped to a
reviewed entry below. `missingIncludeSystem` is the only tool-level suppression;
system headers are outside the CANcestry safety boundary and are supplied by
the target toolchain.

## Deviation procedure

1. Run `formal/misra/run_cppcheck.sh` with the target compiler headers.
2. For each finding, record the file, line, rule, safety impact and a bounded
   rationale here. A deviation does not waive a runtime safety requirement.
3. The reviewer verifies that the alternative mechanism is deterministic,
   bounded and fail-closed, then records approval in the project review log.
4. Re-run the driver with `--error-exitcode=1`. New findings fail the gate;
   this document is not used to make an unreviewed finding appear clean.

## Reviewed deviations

| Rule | Location / pattern | Technical justification and compensating control |
|---|---|---|
| **10.1 / 10.3** essential type balance and composite expression types | Enum values compared with bounded integer fields in `core/event`, `core/fsm` and `core/hal` | The public C99 API intentionally uses named enums for protocol/state domains while counters and wire fields use fixed-width integers. Comparisons are either explicitly cast at the boundary or are guarded by the corresponding `*_is_valid()` check. The enum domains are closed, and invalid values fail closed. Unit and conformance tests cover every boundary. |
| **11.3** cast between pointer to object type and pointer to a different object type | `platform/linux/socketcan.c` context and `struct sockaddr` casts | **Rule 11.3 deviation required for zero-alloc hardware-register / OS-UAPI casting.** SocketCAN requires passing a caller-owned `struct sockaddr_can` to the POSIX socket API and recovering the caller-owned backend context from the opaque HAL callback pointer. The casts do not dereference an incompatible object: the destination type is the object type created by the caller at that call site. The backend checks interface indices and lengths before access; no production core code performs this cast. |
| **11.4** conversion between pointer and integral type | No intentional production use; retain as a monitored rule | The source does not use pointer-as-integer handles. Any future hardware-register address conversion must be isolated in `platform/`, use a fixed-width `uintptr_t`, document the register layout and receive a new deviation approval. A finding in `core/` is therefore a defect, not an accepted deviation. |
| **14.2** `for` loop counter and invariant form | Bounded scans in queue, codec, HAL and loader implementations | The loops iterate over caller/configuration-provided bounded arrays or fixed protocol tables. Their counters are initialized, monotonic and not modified in the body except by the loop expression. The formal contracts and explicit `size <= capacity` checks provide the bound; no loop depends on wraparound. |
| **15.5** single point of exit | Guard-clause validation in allocation-free runtime APIs | Early returns are used to fail closed before touching caller buffers and to keep error paths local. They do not bypass cleanup because runtime modules own no heap resources. Loader cleanup is handled in its explicit ownership layer. The alternative, a shared mutable status variable, would make safety review and write-set reasoning less reliable. |
| **18.1** pointer arithmetic and array bounds | Fixed payload arrays and caller-owned queue storage | All variable-length access is preceded by a legal-length, capacity or bit-span check. CAN FD lengths are restricted to 0..8/12/16/20/24/32/48/64; queue capacity is capped at 32767. The Frama-C contracts on queue and codec functions state the same bounds and `-wp-rte` checks them independently. |
| **18.4** union member / overlay use | Tagged `cancestry_value_t` and event payload union in `core/event` | The union is the wire-neutral representation of a tagged value. The `kind`/`type` discriminator is set before use and documented in the public type contract. Producers and consumers switch on the discriminator; no type-punning or address overlay is used. Sanitizer and unit tests exercise all legal tags and reject invalid tags. |
| **21.6** use of `<stdio.h>` | Host-only loader diagnostics and examples, when reported by a toolchain profile | Firmware runtime paths do not use stdio. Diagnostics are caller-buffered or delivered through the HAL/trace callback. Host-only loader/example output is excluded from the target compilation profile; a target build finding in `core/*/src` is a release-blocking issue. |
| **21.7 / 21.8** string and conversion-library facilities | `strtod`, `strlen` and bounded text parsing in host loaders and the expression parser | Text is bounded by `CANCESTRY_*_MAX` constants before parsing. `strtod` is checked for end position, `ERANGE`, NaN and infinity; production expression evaluation has no dynamic code execution. The loader is an explicitly separate, allocation-permitted host/package boundary and is not linked into the zero-allocation runtime archive. |
| **22.1** overlapping / aliasing assumptions | `memcpy` of caller-owned structs and frame buffers | Copies are between non-overlapping, typed caller buffers, or are ordinary structure assignment. No `memcpy` is used to manufacture an aliased object or to bypass the tagged-union contract. The no-allocation and ASan tests provide an additional runtime check. |

## Rules with no accepted deviation

The following are safety requirements, not waivable style preferences: undefined
shifts or conversions, unchecked array access, recursion outside a documented
bound, dynamic allocation in a runtime archive, unchecked return values from a
safety decision, and any MISRA finding introduced in `core/` that is not listed
above with a concrete location. Such findings must be fixed before the MISRA
gate can pass.

## Evidence

The executable evidence is generated by the command below in the verification
environment. The report is intentionally kept outside Git because it is
compiler-version and target-header dependent:

```sh
MISRA_REPORT=build/misra/cppcheck-report.txt formal/misra/run_cppcheck.sh
```

The Phase 9 verification record ([`SafetyManual.md`](SafetyManual.md)) records
the tool version, target headers, report hash and reviewer disposition for each
release candidate.
