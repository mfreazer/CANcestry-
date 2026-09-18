"""yaml2c FSM generator tests (Phase 8, issue #22).

Requirements traced: SW-FR-TOOL-001, SW-FR-TOOL-003, SW-FR-TOOL-004.
Test ids: YAML2C-FSM-001 (compilable header of a valid v0.3.0 FSM file),
YAML2C-NOALLOC-001 (const static data only), YAML2C-SCHEMA-001 (invalid
documents refused), YAML2C-REFUSE-001..008 (loader-parity refusals).
"""

from __future__ import annotations

import textwrap

import pytest

import yaml

from tools.yaml2c import fsm_codegen
from tools.yaml2c.common import Yaml2cError
from tools.yaml2c.yaml2c import compose_document, detect_kind

from conftest import (INCLUDES, REPO_ROOT, check_no_alloc, compile_c,
                      compose, run_yaml2c)

BASE = """
schema_version: "0.3.0"
state_machines:
  - name: lab
    initial: S
    states:
      - name: S
        transitions:
          - event: can_rx
            target: T
      - name: T
instances:
  - id: lab.one
    machine: lab
    enabled: true
"""


def generate(text: str, tmp_path, prefix: str = "lab") -> str:
    return fsm_codegen.generate_fsm_header(compose(tmp_path, text),
                                           "input.yaml", "test", prefix)


# ---------------------------------------------------------------------------
# YAML2C-FSM-001: a valid v0.3.0 FSM file becomes a compilable C header.
# ---------------------------------------------------------------------------

def test_valid_file_generates_compilable_header(tmp_path, workspace, compiler):
    header = generate(BASE, workspace)
    assert "static const cancestry_fsm_set_t lab_fsm_set" in header
    out_h = workspace / "lab.h"
    out_h.write_text(header, encoding="utf-8")
    source = workspace / "use.c"
    source.write_text(textwrap.dedent("""
        #include "lab.h"
        int use(void) {
            return (lab_fsm_set.machine_count == 1u &&
                    lab_fsm_set.instance_count == 1u &&
                    lab_fsm_set.machines[0].initial_index == 0u &&
                    lab_fsm_set.machines[0].states[1].transitions[0]
                        .target_index == 0u) ? 0 : 1;
        }
    """), encoding="utf-8")
    result = compile_c(compiler, source, workspace / "use.o", INCLUDES)
    assert result.returncode == 0, result.stderr


def test_generated_object_is_allocation_free(tmp_path, workspace, compiler):
    """YAML2C-NOALLOC-001: the generated data references no allocator."""
    header = generate(BASE, workspace)
    (workspace / "lab.h").write_text(header, encoding="utf-8")
    (workspace / "use.c").write_text(
        '#include "lab.h"\nint use(void) { return lab_fsm_set.machine_count; }\n',
        encoding="utf-8")
    result = compile_c(compiler, workspace / "use.c", workspace / "use.o",
                       INCLUDES)
    assert result.returncode == 0, result.stderr
    gate = check_no_alloc(workspace / "use.o")
    assert gate.returncode == 0, gate.stdout + gate.stderr


def test_full_construct_document(tmp_path, workspace):
    """Every generated construct: timers, guards, actions, subscriptions."""
    header = generate("""
schema_version: "0.3.0"
state_machines:
  - name: full
    description: "everything"
    initial: A
    variables:
      - name: count
        type: integer
        default: -3
      - name: ratio
        type: float
        default: 2.5
      - name: flag
        type: boolean
        default: true
      - name: empty
        type: integer
    timers:
      - name: tick
        duration_ms: 5
        repeat: true
        auto_start: true
    states:
      - name: A
        entry:
          - log: {level: warning, message: "watch out"}
        exit:
          - raise_fault: {code: EXITED_A, severity: critical}
        transitions:
          - event: signal_changed
            signal: Speed
            interface: can0
            guard: "sig.Speed > 1.5 and not var.flag"
            actions:
              - set_variable: {variable: count, value: "var.count + 1"}
              - start_timer: {timer: tick, duration_ms: 9, repeat: false}
              - send_message:
                  interface: can1
                  message: Msg
                  signals:
                    A: "sig.Speed * 2"
                    B: 7
              - reset_timer: {timer: tick}
              - set_signal: {signal: Out, value: 1.25}
              - transition: {target: B}
            target: B
          - event: timer_expired
            timer: tick
            target: A
          - event: can_rx
            interface: can0
            message: Drive
            target: A
          - event: fault_raised
            target: B
          - event: state_entered
            target: B
          - event: state_exited
            target: B
          - event: power_mode_changed
            target: B
      - name: B
        exit:
          - stop_timer: {timer: tick}
instances:
  - id: full.one
    machine: full
    enabled: false
    bindings:
      can0: vcan0
      can1: vcan1
    variables:
      count: 42
    subscriptions:
      can_rx:
        - interface: can0
          id: 0x123
          message: Drive
      signals:
        - Speed
      timers: false
      faults: true
      power_mode: true
""", workspace)
    # Spot-check the 1:1 mapping of tricky fields.
    assert "CANCESTRY_FSM_ACTION_SEND_MESSAGE" in header
    assert ".source_line = " in header
    assert ".initial_index = 0u" in header
    assert ".target_index = 1u" in header
    assert ".machine_index = 0u" in header
    assert ".has_can_id = true" in header
    assert ".can_id = 291u" in header
    assert "CANCESTRY_VALUE_KIND_INT, .value = {.integer = (int64_t)-3}" in header
    assert ".value = {.real = 2.5}" in header
    assert ".value = {.boolean = true}" in header
    assert ".has_default = false" in header
    assert "CANCESTRY_FSM_LAYOUT" not in header
    assert ".layout = CANCESTRY_CODEC_LAYOUT" not in header
    # No dynamic lifetime anywhere in the artifact.
    for forbidden in ("malloc", "calloc", "realloc", "free("):
        assert forbidden not in header


# ---------------------------------------------------------------------------
# Loader-parity refusals: every error below is one the C loader raises too.
# ---------------------------------------------------------------------------

def refuse(text: str, tmp_path, fragment: str) -> None:
    with pytest.raises(Yaml2cError) as info:
        generate(text, tmp_path)
    assert fragment in str(info.value), str(info.value)


def test_refuses_unknown_transition_target(tmp_path, workspace):
    refuse(BASE.replace("target: T", "target: NOPE"), workspace,
           'unknown transition target "NOPE"')


def test_refuses_unknown_initial_state(tmp_path, workspace):
    refuse(BASE.replace("initial: S", "initial: NOPE"), workspace,
           'unknown initial state "NOPE"')


def test_refuses_unknown_machine(tmp_path, workspace):
    refuse(BASE.replace("machine: lab", "machine: other"), workspace,
           'unknown machine "other"')


def test_refuses_duplicate_state_names(tmp_path, workspace):
    refuse(BASE.replace("- name: T", "- name: S"), workspace,
           'duplicate state name "S"')


def test_refuses_duplicate_machine_names(tmp_path, workspace):
    text = BASE.replace(
        "instances:",
        "  - name: lab\n    initial: S\n    states:\n      - name: S\n"
        "instances:")
    refuse(text, workspace, 'duplicate machine name "lab"')


def test_refuses_duplicate_instance_ids(tmp_path, workspace):
    text = BASE.replace("enabled: true",
                        "enabled: true\n  - id: lab.one\n    machine: lab\n"
                        "    enabled: true")
    refuse(text, workspace, 'duplicate instance id "lab.one"')


def test_refuses_unknown_set_variable(tmp_path, workspace):
    text = """
schema_version: "0.3.0"
state_machines:
  - name: m
    initial: A
    states:
      - name: A
        entry:
          - set_variable: {variable: nope, value: 1}
        transitions:
          - event: can_rx
            target: A
instances:
  - id: i
    machine: m
    enabled: true
"""
    refuse(text, workspace, 'set_variable names "nope"')


def test_refuses_unknown_timer_action(tmp_path, workspace):
    text = """
schema_version: "0.3.0"
state_machines:
  - name: m
    initial: A
    states:
      - name: A
        entry:
          - stop_timer: {timer: nope}
        transitions:
          - event: can_rx
            target: A
instances:
  - id: i
    machine: m
    enabled: true
"""
    refuse(text, workspace, 'a timer action names "nope"')


def test_refuses_unknown_start_timer(tmp_path, workspace):
    text = """
schema_version: "0.3.0"
state_machines:
  - name: m
    initial: A
    states:
      - name: A
        entry:
          - start_timer: {timer: nope}
        transitions:
          - event: can_rx
            target: A
instances:
  - id: i
    machine: m
    enabled: true
"""
    refuse(text, workspace, 'start_timer names "nope"')


def test_refuses_unknown_instance_variable(tmp_path, workspace):
    text = BASE.replace("enabled: true",
                        "enabled: true\n    variables:\n      nope: 1")
    refuse(text, workspace, 'initialises variable "nope"')


def test_refuses_unknown_action(tmp_path, workspace):
    text = BASE.replace(
        "      - name: S\n",
        "      - name: S\n        entry:\n"
        "          - teleport: {to: moon}\n")
    refuse(text, workspace, 'unknown action "teleport"')


def test_refuses_unknown_field(tmp_path, workspace):
    text = BASE.replace("enabled: true", "enabled: true\n    color: red")
    refuse(text, workspace, 'unknown field "color" in instance')


def test_refuses_bad_expression(tmp_path, workspace):
    text = BASE.replace("- event: can_rx",
                        "- event: can_rx\n            guard: \"Speed > 1\"")
    refuse(text, workspace, "guard is not a valid expression")


def test_refuses_name_too_long(tmp_path, workspace):
    long_name = "x" * 65
    text = BASE.replace("name: lab", "name: %s" % long_name, 1)
    refuse(text, workspace, "is longer than 64 characters")


def test_refuses_leading_zero_number(tmp_path, workspace):
    """``007`` is YAML 1.1 octal; the C loader refuses it, so must yaml2c."""
    text = """
schema_version: "0.3.0"
state_machines:
  - name: m
    initial: A
    variables:
      - name: v
        type: integer
    states:
      - name: A
        transitions:
          - event: can_rx
            target: A
instances:
  - id: i
    machine: m
    enabled: true
    variables:
      v: 007
"""
    refuse(text, workspace, "is not a representable number")


def test_refuses_malformed_exponent(tmp_path, workspace):
    """``1e`` is refused as a malformed number like fsm_scan_number does."""
    text = BASE.replace("enabled: true",
                        "enabled: true\n    variables:\n      v: 1e")
    text = text.replace("  - name: lab\n    initial: S",
                        "  - name: lab\n    initial: S\n"
                        "    variables:\n      - name: v\n        type: integer")
    refuse(text, workspace, "is not a representable number")


def test_refuses_quoted_boolean(tmp_path, workspace):
    refuse(BASE.replace("enabled: true", 'enabled: "true"'), workspace,
           "instance.enabled must be true or false")


def test_refuses_timer_duration_zero(tmp_path, workspace):
    text = BASE.replace("initial: S", "initial: S\n    timers:\n"
                        "      - name: t\n        duration_ms: 0\n"
                        "        repeat: false\n        auto_start: false")
    refuse(text, workspace, "timer.duration_ms is out of range")


def test_refuses_sequence_where_mapping_expected(tmp_path, workspace):
    text = BASE.replace("machine: lab", "machine: [lab]")
    refuse(text, workspace, "instance.machine must be a string")


def test_source_lines_match_yaml(tmp_path, workspace):
    """source_line is the 1-based action line, like the C loader records."""
    text = """schema_version: "0.3.0"
state_machines:
  - name: m
    initial: A
    states:
      - name: A
        entry:
          - log:
              level: info
              message: hi
        transitions:
          - event: can_rx
            target: A
instances:
  - id: i
    machine: m
    enabled: true
"""
    header = generate(text, workspace)
    # The log action begins on line 8 ("- log:").
    assert ".source_line = 8u" in header


# ---------------------------------------------------------------------------
# CLI behaviour.
# ---------------------------------------------------------------------------

def test_cli_generates_file(tmp_path, workspace):
    source = workspace / "doc.yaml"
    source.write_text(BASE, encoding="utf-8")
    output = workspace / "out" / "doc.h"
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        "-o", str(output), str(source))
    assert result.returncode == 0, result.stderr
    assert output.exists()
    assert "cancestry_fsm_set_t doc_fsm_set" in output.read_text()


def test_cli_stdout_and_custom_prefix(tmp_path, workspace):
    source = workspace / "doc.yaml"
    source.write_text(BASE, encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        "--stdout", "--set-symbol", "my_pkg",
                        str(source))
    assert result.returncode == 0, result.stderr
    assert "my_pkg_fsm_set" in result.stdout


def test_cli_rejects_bad_document(tmp_path, workspace):
    source = workspace / "bad.yaml"
    source.write_text(BASE.replace("enabled: true", "enabled: 1"),
                      encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        str(source))
    assert result.returncode == 1
    assert "yaml2c" in result.stderr


def test_cli_rejects_missing_input(tmp_path, workspace):
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        str(workspace / "ghost.yaml"))
    assert result.returncode == 2


def test_cli_detects_codec_kind(tmp_path, workspace):
    source = workspace / "map.yaml"
    source.write_text("""schema_version: "0.3.0"
codec_map:
  name: m
  version: 1.0.0
  messages:
    - id: 1
      name: A
      dlc: 1
      signals:
        - name: s
          start_bit: 0
          bit_length: 1
          type: boolean
          endianness: little
""", encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        "--stdout", str(source))
    assert result.returncode == 0, result.stderr
    assert "cancestry_codec_map_t map_codec_map" in result.stdout


def test_cli_kind_mismatch_is_refused(tmp_path, workspace):
    source = workspace / "doc.yaml"
    source.write_text(BASE, encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        "--kind", "codec", "--stdout", str(source))
    assert result.returncode == 1
    assert "codec-map" in result.stderr


def test_cli_syntax_error_position(tmp_path, workspace):
    source = workspace / "broken.yaml"
    source.write_text("state_machines: [unclosed\n", encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        str(source))
    assert result.returncode == 1
    assert "YAML syntax error" in result.stderr


def test_cli_version():
    result = run_yaml2c("--version")
    assert result.returncode == 0
    assert result.stdout.startswith("yaml2c ")


def test_detect_kind_rejects_empty(tmp_path, workspace):
    with pytest.raises(Yaml2cError):
        detect_kind(compose(workspace, ""))
    with pytest.raises(Yaml2cError):
        detect_kind(compose(workspace, "- a\n- b\n"))
    with pytest.raises(Yaml2cError):
        detect_kind(compose(workspace, "nothing: here\n"))


# ---------------------------------------------------------------------------
# Duplicate mapping keys are refused like the hand-written loaders do.
# ---------------------------------------------------------------------------

def test_refuses_duplicate_mapping_keys(tmp_path, workspace):
    text = BASE + "  - id: lab.two\n    machine: lab\n    enabled: true\n" \
        "    variables: {}\n"
    duplicated = BASE.replace("enabled: true",
                              "enabled: true\n    enabled: false")
    refuse(duplicated, workspace, "duplicate mapping key 'enabled'")


# ---------------------------------------------------------------------------
# Schema validation integration (SW-FR-TOOL-004).
# ---------------------------------------------------------------------------

def test_schema_violation_refuses_generation(tmp_path, workspace):
    """A document the 0.3.0 schema rejects never reaches the generator."""
    bad = BASE.replace('schema_version: "0.3.0"', 'schema_version: "0.2.0"')
    source = workspace / "bad_version.yaml"
    source.write_text(bad, encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        str(source))
    assert result.returncode == 1
    assert "0.3.0" in result.stderr


def test_yaml11_booleans_follow_the_loader(tmp_path, workspace):
    """``OFF``/``yes`` are text for the C loaders, so the schema sees text.

    The generator keeps the loader view: an ``initial: OFF`` becomes the
    state name "OFF" (which does not exist here, hence the refusal with the
    loader's message, not a schema type error).
    """
    text = BASE.replace("initial: S", "initial: OFF")
    refuse(text, workspace, 'unknown initial state "OFF"')


def test_no_schema_validate_flag_skips_validation(tmp_path, workspace):
    """An 0.2.0 schema_version fails validation but the generator accepts
    the identical structure, so the flag isolates the schema check."""
    source = workspace / "doc.yaml"
    source.write_text(BASE.replace('schema_version: "0.3.0"',
                                   'schema_version: "0.2.0"'),
                      encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        "--no-schema-validate", "--stdout", str(source))
    assert result.returncode == 0, result.stderr


def test_real_package_example_still_loads(tmp_path, workspace):
    """The committed gateway_real FSM file generates without error."""
    source = REPO_ROOT / "examples" / "gateway_real" / "gateway_fsm.yaml"
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        "--stdout", str(source))
    assert result.returncode == 0, result.stderr
    assert "gateway_fsm_fsm_set" in result.stdout
