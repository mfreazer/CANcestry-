"""Refusal-matrix extensions for the yaml2c generators (issue #22).

These drive the readers directly (no schema layer) so the loader-parity
refusals are exercised exactly where the generated C must agree with the
runtime loaders (SW-FR-TOOL-004).
Test ids: YAML2C-REFUSE-009..020, YAML2C-CODEC-REFUSE-007..012.
"""

from __future__ import annotations

import pytest

from tools.yaml2c import codec_codegen, fsm_codegen
from tools.yaml2c.common import Yaml2cError

FSM_BASE = """
schema_version: "0.3.0"
state_machines:
  - name: lab
    initial: S
    timers:
      - name: poll
        duration_ms: 5
        repeat: false
        auto_start: false
    variables:
      - name: count
        type: integer
    states:
      - name: S
        transitions:
          - event: can_rx
            target: T
      - name: T
instances:
  - id: one
    machine: lab
    enabled: true
"""


def fsm(text):
    return fsm_codegen.generate_fsm_header(_composed(text), "doc.yaml",
                                           "test", "lab")


def _composed(text):
    import tempfile
    import os
    from tools.yaml2c.yaml2c import compose_document
    handle = tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False)
    try:
        handle.write(text)
        handle.close()
        return compose_document(handle.name)
    finally:
        os.unlink(handle.name)


def refuse_fsm(fragment, action=None, doc=None):
    """Refusal via an action block appended to the first transition."""
    if doc is None:
        block = "".join("              " + line + "\n"
                        for line in action.strip("\n").splitlines())
        text = FSM_BASE.replace(
            "          - event: can_rx\n            target: T\n",
            "          - event: can_rx\n            target: T\n"
            "            actions:\n" + block)
    else:
        text = doc
    with pytest.raises(Yaml2cError) as info:
        fsm(text)
    assert fragment in str(info.value), str(info.value)


def test_action_value_type_is_checked():
    refuse_fsm("set_signal.value must be a number, a boolean or a string",
               action="""
               - set_signal:
                   signal: bogus
                   value: [1]
""")


def test_action_needs_exactly_one_key():
    refuse_fsm("an action must be a mapping with exactly one key",
               action="""
               - stop_timer:
                   timer: poll
                 transition:
                   target: T
""")


def test_send_message_needs_at_least_one_signal():
    refuse_fsm("send_message.signals must set at least one signal value",
               action="""
               - send_message:
                   interface: can0
                   message: M
                   signals: {}
""")


def test_log_level_vocabulary():
    refuse_fsm('log.level "debug" is not one of info, warning, error',
               action="""
               - log:
                   level: debug
                   message: nope
""")


def test_fault_severity_vocabulary():
    refuse_fsm('raise_fault.severity "fatal" is not one of warning, error,',
               action="""
               - raise_fault:
                   code: 1
                   severity: fatal
""")


def test_unknown_event_type():
    refuse_fsm('transition.event "can_fd_rx" is not a known event type',
               action="",
               doc=FSM_BASE.replace("- event: can_rx", "- event: can_fd_rx"))


def test_variable_type_vocabulary():
    text = FSM_BASE.replace("        type: integer", "        type: float32")
    refuse_fsm('variable type "float32" is not one of boolean, integer, float',
               doc=text)


def test_empty_bindings_and_variables_refused():
    refuse_fsm("instance.bindings must be a non-empty mapping",
               doc=FSM_BASE.replace(
                   "    enabled: true",
                   "    enabled: true\n    bindings: {}"))
    refuse_fsm("instance.variables must be a non-empty mapping",
               doc=FSM_BASE.replace(
                   "    enabled: true",
                   "    enabled: true\n    variables: {}"))


def test_transition_action_target_must_exist():
    refuse_fsm('a transition action targets "NOWHERE", which the state '
               "machine does not define",
               action="""
               - transition:
                   target: NOWHERE
""")


def test_default_prefix_comes_from_the_source_name():
    header = fsm_codegen.generate_fsm_header(_composed(FSM_BASE),
                                             "my-machine.yaml", "test")
    assert "my_machine_fsm_set" in header


def test_value_mapping_rejects_a_non_string_key_directly():
    """The loader view refuses non-scalar keys first, so exercise the
    reader's own guard with a composed exotic-key document."""
    import os
    import tempfile

    from tools.yaml2c.common import map_value as mv
    from tools.yaml2c.yaml2c import compose_document

    handle = tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False)
    handle.write("? [a]\n: 1\n")
    handle.close()
    try:
        root = compose_document(handle.name)
    finally:
        os.unlink(handle.name)
    reader = codec_codegen.CodecMapReader(root)
    with pytest.raises(Yaml2cError) as info:
        reader._node_pairs(root, "value mapping")
    assert "key must be a string" in str(info.value)


CODEC_BASE = """
schema_version: "0.3.0"
codec_map:
  name: lab
  version: 1.0.0
  messages:
    - id: 1
      name: M
      dlc: 8
      signals:
        - name: s
          start_bit: 0
          bit_length: 8
          type: uint
          endianness: little
"""


def codec(text, prefix="lab"):
    return codec_codegen.generate_codec_header(_composed(text), "map.yaml",
                                               "test", prefix)


def refuse_codec(fragment, text):
    with pytest.raises(Yaml2cError) as info:
        codec(text)
    assert fragment in str(info.value), str(info.value)


def test_signal_type_endianness_layout_vocabularies():
    refuse_codec("signal type 'float' is not one of uint, int,",
                 CODEC_BASE.replace("type: uint", "type: float"))
    refuse_codec("signal endianness 'middle' is not one of little, big",
                 CODEC_BASE.replace("little", "middle"))
    refuse_codec("signal layout 'interleaved' is not one of contiguous,",
                 CODEC_BASE.replace("endianness: little",
                                    "endianness: little\n"
                                    "          layout: interleaved"))


def test_sawtooth_must_fit_the_payload():
    text = """
schema_version: "0.3.0"
codec_map:
  name: lab
  version: 1.0.0
  can_fd: true
  messages:
    - id: 1
      name: M
      dlc: 64
      signals:
        - name: s
          start_bit: 504
          bit_length: 16
          type: uint
          endianness: big
          layout: sawtooth
"""
    refuse_codec("sawtooth signal 's' exceeds the 512-bit payload", text)


TWO_MESSAGES = """
schema_version: "0.3.0"
codec_map:
  name: lab
  version: 1.0.0
  messages:
    - id: 1
      name: M
      dlc: 8
      signals:
        - name: s
          start_bit: 0
          bit_length: 8
          type: uint
          endianness: little
    - id: 2
      name: M
      dlc: 8
      signals:
        - name: t
          start_bit: 0
          bit_length: 8
          type: uint
          endianness: little
"""


def test_message_level_checks():
    refuse_codec("duplicate message name 'M'", TWO_MESSAGES)


def test_fd_dlc_vocabulary_in_direct_mode():
    text = """
schema_version: "0.3.0"
codec_map:
  name: lab
  version: 1.0.0
  can_fd: true
  messages:
    - id: 1
      name: M
      dlc: 10
      signals:
        - name: s
          start_bit: 0
          bit_length: 1
          type: boolean
          endianness: little
"""
    refuse_codec("dlc 10 is not a CAN FD payload length", text)


def test_optional_message_fields_are_emitted():
    text = CODEC_BASE.replace(
        "      dlc: 8",
        '      dlc: 8\n      period_ms: 10\n      description: "the message"')
    header = codec(text)
    assert ".period_ms = 10u" in header
    assert "the message" in header


def test_unknown_fields_at_every_level():
    refuse_codec('unknown field "bogus" in the codec map file',
                 "bogus: 1\n" + CODEC_BASE)
    refuse_codec('unknown field "bogus" in codec_map',
                 CODEC_BASE.replace("  version: 1.0.0",
                                    "  version: 1.0.0\n  bogus: 1"))
    refuse_codec("unknown field 'gain' in signal",
                 CODEC_BASE.replace("          endianness: little",
                                    "          endianness: little\n"
                                    "          gain: 2"))


def test_map_description_is_emitted():
    text = CODEC_BASE.replace("  name: lab",
                              '  name: lab\n  description: "lab map"')
    assert "lab map" in codec(text)


def test_value_mapping_key_checks():
    """Duplicate and non-string value keys are refused by the reader helper;
    the loader view normally catches them first, so drive it directly."""
    import os
    import tempfile

    from tools.yaml2c.common import map_value as mv
    from tools.yaml2c.yaml2c import compose_document

    dup = CODEC_BASE.replace(
        "          endianness: little",
        "          endianness: little\n          values:\n"
        "            0: OFF\n            0: ON")
    handle = tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False)
    handle.write(dup)
    handle.close()
    try:
        root = compose_document(handle.name)
    finally:
        os.unlink(handle.name)
    codec_map = mv(root, "codec_map")
    first_message = mv(codec_map, "messages").value[0]
    first_signal = mv(first_message, "signals").value[0]
    reader = codec_codegen.CodecMapReader(root)
    with pytest.raises(Yaml2cError) as info:
        reader._node_pairs(mv(first_signal, "values"), "value mapping")
    assert "duplicate mapping key '0'" in str(info.value)


def test_require_number_branches():
    text = CODEC_BASE.replace("          endianness: little",
                              "          endianness: little\n"
                              "          scale: abc")
    refuse_codec("scale must be a number", text)
    text = CODEC_BASE.replace("          endianness: little",
                              "          endianness: little\n"
                              "          scale: 1e999")
    refuse_codec("scale must be a finite number", text)
    text = CODEC_BASE.replace("          endianness: little",
                              "          endianness: little\n"
                              "          min: true")
    refuse_codec("min must be a number", text)


def test_number_helper_with_synthetic_nodes():
    """The numeric node helper duck-types; pin the non-YAML branches."""
    from types import SimpleNamespace

    def node(value):
        return SimpleNamespace(value=value, style=None,
                               start_mark=SimpleNamespace(line=0, column=0))

    assert codec_codegen._require_number(node(5), "x") == 5.0
    with pytest.raises(Yaml2cError) as info:
        codec_codegen._require_number(node(True), "x")
    assert "must be a number" in str(info.value)
    assert codec_codegen._mark(object()) == (None, None)
    assert fsm_codegen._mark(object()) == (None, None)
    assert codec_codegen._require_number(node(0.5), "x") == 0.5
    with pytest.raises(Yaml2cError) as info:
        codec_codegen._require_number(node(float("nan")), "x")
    assert "finite" in str(info.value)
    with pytest.raises(Yaml2cError) as info:
        codec_codegen._require_number(node([1]), "x")
    assert "must be a number" in str(info.value)


def test_codec_default_prefix_comes_from_the_source_name():
    assert "map_codec_map" in codec(CODEC_BASE, prefix=None)
