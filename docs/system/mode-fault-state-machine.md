# CANcestry Mode and Fault State Machine

Version: 0.2.1

## 1. System Modes

The normative modes are:

- BOOT
- CONFIG
- LISTEN_ONLY
- ACTIVE
- SAFE
- OFF

GATEWAY and EMULATION are ACTIVE sub-profiles.

## 2. Mode Transitions

| From | To | Trigger | Condition |
|---|---|---|---|
| BOOT | CONFIG | Packages validated | No critical fault |
| BOOT | SAFE | Invalid config or hardware fault | Critical fault |
| CONFIG | LISTEN_ONLY | User command | RX allowed |
| CONFIG | ACTIVE | User command | Valid capabilities and governor ready |
| CONFIG | SAFE | Fault | Critical fault |
| LISTEN_ONLY | ACTIVE | User command | Explicit enable |
| LISTEN_ONLY | CONFIG | User command | None |
| LISTEN_ONLY | SAFE | Fault | Critical fault |
| ACTIVE | LISTEN_ONLY | User stop or policy recovery | TX disabled |
| ACTIVE | CONFIG | User command | None |
| ACTIVE | SAFE | Critical fault, governor violation, user safe command | Immediate TX disable |
| SAFE | CONFIG | Manual recovery | Diagnostics allowed |
| SAFE | LISTEN_ONLY | Recovery allowed and CAN healthy | Manual or policy-approved |
| SAFE | ACTIVE | Explicit recovery command | Requires explicit authorization |
| ANY | SAFE | Critical fault | Immediate containment |
| ANY | OFF | Power-down request | Safe shutdown |

## 3. Bus-Off Policy

CAN controller auto-recovery is allowed.

However:

- ACTIVE transmission shall not auto-resume by default.
- The system may return to LISTEN_ONLY automatically only if configured.
- SAFE mode requires explicit recovery to return to ACTIVE.

Default:

    fault_policy:
      bus_off:
        severity: error
        can_controller_auto_recover: true
        system_mode_after_recovery: LISTEN_ONLY
        auto_resume_active: false

## 4. Fault Severities

- INFO
- WARNING
- ERROR
- CRITICAL

## 5. Fault Reaction Table

| Fault Cause | Severity | Reaction |
|---|---:|---|
| Package schema invalid | CRITICAL | Reject package |
| Capability violation attempt | ERROR | Block action and increment counter |
| Repeated capability violation | CRITICAL | Suspend package and enter SAFE |
| Expression evaluation error | ERROR | Suspend FSM or recipe |
| Event queue overflow | WARNING | Drop according to policy |
| Persistent event overflow | ERROR | Suspend source package |
| CAN RX overrun | WARNING | Drop according to queue policy |
| CAN bus-off | ERROR | Suspend TX, recover controller, enter SAFE or LISTEN_ONLY by policy |
| Watchdog timeout | CRITICAL | Controlled reset into SAFE or CONFIG with TX disabled |
| Storage failure for logs | WARNING | Disable logging |
| Storage failure for config/packages | CRITICAL | Enter SAFE |
| Governor internal failure | CRITICAL | Deny all TX and enter SAFE |
| Timer overrun | WARNING | Emit one event with missed_count |
| FSM transition chain exceeded | ERROR | Suspend FSM instance |
| Unauthorized signal write | ERROR | Block action and increment violation counter |
