"""Unit tests for the yaml2c expression validator (issue #22).

The validator must accept exactly the expressions the C engine's expression
parser (core/fsm/src/expression.c) accepts, and refuse the rest with the
same verdicts (SW-FR-TOOL-004).

Test id: YAML2C-EXPR-001 (acceptance matrix), YAML2C-EXPR-REFUSE-001..012.
"""

from __future__ import annotations

import pytest

from tools.yaml2c import expression as expr
from tools.yaml2c.common import Yaml2cError

VALID = [
    "true",
    "false",
    "1",
    "-1",
    "0.5",
    "-.5",
    "1e3",
    "1E+3",
    "sig.speed",
    "sig.speed > 0",
    "sig.speed >= 30.0 and sig.brake == false",
    "var.count != 3",
    "evt.can_id == 801",
    "not sig.enabled",
    "sig.a > 1 or sig.b > 2 and sig.c < 3",
    "(sig.a > 1 or sig.b > 2) and not sig.c",
    "sig.x % 2 == 0",
    "sig.x * 2 + 1 - 3 / 4",
    "- - sig.x",
    "clamp(sig.speed, 0.0, 100.0)",
    "abs(var.delta)",
    "min(sig.a, sig.b)",
    "max(sig.a, 0.0)",
    "round(sig.x)",
    "((((1))))",
    "sig.very_long_name_0123456789_0123456789",
    "var.1",
    "sig.a == true and not var.locked",
]

INVALID = [
    # (text, message fragment)
    ("sig.speed >", "unexpected token"),
    ("(sig.a", "expected ')'"),
    ("sig.a)", "trailing input"),
    ("1 +", "unexpected token"),
    ("*", "unexpected token"),
    ("speed", "bare identifiers are not permitted"),
    ("SIG.x", "bare identifiers are not permitted"),
    ("sig.", "empty name after 'sig'. prefix"),
    ("evt.bogus", "unknown evt. field"),
    ("sig." + "x" * 200, "longer than"),
    ("and", "unexpected keyword"),
    ("or true", "unexpected keyword"),
    ("sig.a = 1", "unexpected character '='"),
    ("sig.a && sig.b", "unexpected character '&'"),
    ("abs(1, 2)", "abs() takes 1"),
    ("round(1, 2)", "round() takes 1"),
    ("min()", "min() takes 2 argument(s), got 0"),
    ("min(1)", "min() takes 2 argument(s), got 1"),
    ("clamp(1, 2)", "clamp() takes 3"),
    ("min(1, 2, 3)", "takes 2 argument(s), got 3"),
    ("min(1,", "unexpected token"),
    ("min(1, 2, 3, 4)", "too many arguments"),
    ("abs(1", "expected ')' to close the call"),
    ("sig.a in", "trailing input 'in'"),
    ("sig.a ? 1", "unexpected character '?'"),
    ("!!true", "unexpected character '!'"),
    ("1..2", "unexpected character '.'"),
    ("0x1G", "trailing input"),
    ("sig.a < = 1", "unexpected character '='"),
    ("sig.a <> 1", "unexpected token"),
    ("1 < 2 < 3", "trailing input '<'"),
    ("evt.can_id == EVT_ID", "bare identifiers are not permitted"),
]


@pytest.mark.parametrize("text", VALID, ids=lambda t: t[:24])
def test_valid_expressions(text):
    expr.validate_expression(text, "guard", 1, 1, 512)


@pytest.mark.parametrize("text,fragment", INVALID, ids=lambda t: t[:24])
def test_invalid_expressions(text, fragment):
    with pytest.raises(Yaml2cError) as info:
        expr.validate_expression(text, "guard", 1, 1, 512)
    assert fragment in str(info.value), "%s: %s" % (text, info.value)


def test_length_limit_is_in_bytes():
    with pytest.raises(Yaml2cError) as info:
        expr.validate_expression("sig." + "\u00e9" * 300, "guard", 1, 1, 512)
    assert "longer than 512" in str(info.value)


def test_empty_and_whitespace_refused():
    for text in ("", "   "):
        with pytest.raises(Yaml2cError) as info:
            expr.validate_expression(text, "guard", 1, 1, 512)
        assert "empty" in str(info.value)


def test_error_carries_position():
    with pytest.raises(Yaml2cError) as info:
        expr.validate_expression("  sig.a !", "guard", 7, 3, 512)
    assert info.value.line == 7 and info.value.column == 3


def test_evt_fields_match_the_engine_event_type():
    """The evt. field list is pinned to the C event union fields."""
    assert "can_id" in expr.EVT_FIELDS
    assert "bogus" not in expr.EVT_FIELDS
    for field in ("timestamp_us", "fault_code", "instance_id"):
        assert field in expr.EVT_FIELDS


def test_offsets_are_zero_based_and_eof_is_last():
    tokens = expr._tokenize("sig.a == 1")
    assert tokens[0].offset == 0
    assert tokens[-1].kind == expr._TOKEN_EOF


def test_hex_literals_are_not_expression_literals():
    """The C expression grammar has no hex literal spelling: 0x321 is a
    number followed by a bare name, hence refused (mirror that)."""
    with pytest.raises(Yaml2cError) as info:
        expr.validate_expression("evt.can_id == 0x321", "guard", 1, 1, 512)
    assert "trailing input" in str(info.value)
