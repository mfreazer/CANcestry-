"""Direct unit tests for the yaml2c shared helpers (issue #22).

Requirement traced: SW-FR-TOOL-004. Test ids: YAML2C-UNIT-001. (the generator must be a faithful 1:1
translation, so every C literal helper is pinned against the C loader's
acceptance rules).
"""

from __future__ import annotations

import pytest

from tools.yaml2c import common
from tools.yaml2c.common import (Emitter, StringTable, Yaml2cError,
                                 c_double, c_int, c_string_literal,
                                 check_count, header_guard, loader_view,
                                 map_pairs, map_value, operand_from_scalar,
                                 require_bool, require_text, require_uint,
                                 seq_items, symbolize)

from conftest import compose


def _scalar(tmp_path, text):
    """The value node of 'k: <text>' composed like the CLI composes it."""
    root = compose(tmp_path, "k: %s\n" % text)
    return map_value(root, "k")


# --- C literal helpers ------------------------------------------------------

class TestCStringLiteral:
    def test_plain(self):
        assert c_string_literal("abc") == '"abc"'

    def test_escapes(self):
        assert c_string_literal('a"b') == '"a\\"b"'
        assert c_string_literal("a\\b") == '"a\\\\b"'
        assert c_string_literal("a?b") == '"a\\?b"'
        assert c_string_literal("a\nb") == '"a\\012b"'
        assert c_string_literal("a\tb") == '"a\\011b"'
        assert c_string_literal("a\rb") == '"a\\015b"'

    def test_control_characters_use_octal(self):
        assert c_string_literal("a\x01b") == '"a\\001b"'

    def test_utf8_bytes_are_emitted_octal(self):
        assert c_string_literal("\u00e9") == '"\\303\\251"'


class TestCDoubleAndInt:
    def test_formats(self):
        assert c_double(0.01) == "0.01"
        assert c_double(163.83) == "163.83000000000001"
        assert c_double(-40.0) == "(-40.0)"
        assert c_double(0.0) == "0.0"
        assert c_double(1e-9) == "1.0000000000000001e-09"

    def test_non_finite_refused(self):
        with pytest.raises(Yaml2cError):
            c_double(float("inf"))
        with pytest.raises(Yaml2cError):
            c_double(float("nan"))

    def test_c_int(self):
        assert c_int(0, "uint8_t") == "(uint8_t)0"
        assert c_int(-1, "int32_t") == "(int32_t)-1"
        assert c_int(2 ** 33, "uint64_t") == "(uint64_t)8589934592"
        with pytest.raises(Yaml2cError) as info:
            c_int(2 ** 64, "uint64_t")
        assert "does not fit in 64 bits" in str(info.value)


class TestSymbolizeAndGuard:
    def test_symbolize(self):
        assert symbolize("my-map") == "my_map"
        assert symbolize("1thing") == "_1thing"
        assert symbolize("") == "_"
        assert symbolize("a+b c/d") == "a_b_c_d"

    def test_header_guard(self):
        assert header_guard("my header.yaml") == "MY_HEADER_H"
        assert header_guard("1.h") == "_1_H"


# --- Emitter and StringTable -------------------------------------------------

def render(fn) -> str:
    emitter = Emitter()
    fn(emitter)
    return emitter.text()


class TestEmitter:
    def test_open_close_and_indentation(self):
        text = render(lambda e: (e.open("typedef struct {"),
                                 e.line("int a;"),
                                 e.close("} name;", comma=True)))
        assert text == "typedef struct {\n    int a;\n} name;,\n"
    def test_push_pop_blank_comment(self):
        def body(e):
            e.comment("hello")
            e.blank()
            e.push()
            e.line("x;")
            e.pop()
            e.line("")
            e.line("y;")
        assert render(body) == "/* hello */\n\n    x;\n\ny;\n"


class TestStringTable:
    def test_dedup_and_order(self):
        table = StringTable("p")
        assert table.intern("b") == "p_str_000"
        assert table.intern("a") == "p_str_001"
        assert table.intern("b") == "p_str_000"
        text = render(table.emit)
        assert '"b"' in text and '"a"' in text
        assert text.index('"b"') < text.index('"a"')

    def test_empty_table_emits_nothing(self):
        assert render(StringTable("p").emit) == "\n"


# --- scalar accessors -------------------------------------------------------

class TestRequireText:
    def test_string(self, tmp_path):
        node = _scalar(tmp_path, "hello")
        text, line, column = require_text(node, "name")
        assert text == "hello" and (line, column) == (1, 4)

    def test_non_string(self, tmp_path):
        from tools.yaml2c.common import Yaml2cError as _E

        class FakeMark:
            line, column = 3, 4

        class FakeNode:
            value = 5
            style = None
            start_mark = FakeMark()

        with pytest.raises(_E) as info:
            require_text(FakeNode(), "name")
        assert "must be a string" in str(info.value)
        assert info.value.line == 4 and info.value.column == 5

    def test_too_long_counts_utf8_bytes(self, tmp_path):
        node = _scalar(tmp_path, '"' + "\u00e9" * 40 + '"')
        with pytest.raises(Yaml2cError) as info:
            require_text(node, "name", maximum=64)
        assert "longer than 64" in str(info.value)

    def test_none_node(self):
        with pytest.raises(Yaml2cError):
            require_text(None, "name")


class TestRequireUintAndBool:
    def test_uint(self, tmp_path):
        assert require_uint(_scalar(tmp_path, "5"), "dlc") == 5
        assert require_uint(_scalar(tmp_path, "0x10"), "dlc") == 16
        with pytest.raises(Yaml2cError) as info:
            require_uint(_scalar(tmp_path, "007"), "dlc")
        assert "leading zero" in str(info.value)
        with pytest.raises(Yaml2cError):
            require_uint(_scalar(tmp_path, "abc"), "dlc")
        with pytest.raises(Yaml2cError):
            require_uint(_scalar(tmp_path, "-1"), "dlc")
        with pytest.raises(Yaml2cError) as info:
            require_uint(_scalar(tmp_path, "1e3"), "dlc")
        assert "must be an integer" in str(info.value)
        with pytest.raises(Yaml2cError) as info:
            require_uint(_scalar(tmp_path, "9"), "dlc", maximum=8)
        assert "out of range" in str(info.value)

    def test_bool(self, tmp_path):
        assert require_bool(_scalar(tmp_path, "true"), "flag") is True
        assert require_bool(_scalar(tmp_path, "false"), "flag") is False
        with pytest.raises(Yaml2cError):
            require_bool(_scalar(tmp_path, "0"), "flag")
        with pytest.raises(Yaml2cError):
            require_bool(_scalar(tmp_path, "'true'"), "flag")


class TestContainers:
    def test_map_pairs_refuses_non_mapping(self, tmp_path):
        node = compose(tmp_path, "- a\n- b")
        with pytest.raises(Yaml2cError) as info:
            list(map_pairs(node, "thing"))
        assert "thing must be a mapping" in str(info.value)

    def test_seq_items_refuses_non_sequence(self, tmp_path):
        with pytest.raises(Yaml2cError) as info:
            list(seq_items(_scalar(tmp_path, "x"), "list"))
        assert "must be a sequence" in str(info.value)

    def test_map_value(self, tmp_path):
        root = compose(tmp_path, "a: 1")
        assert map_value(root, "b") is None
        assert map_value(root, "a") is not None

    def test_check_count(self):
        assert check_count(3, "signals") == 3
        with pytest.raises(Yaml2cError) as info:
            check_count(0x10000, "signals")
        assert "exceeds 65535" in str(info.value)


# --- operand_from_scalar ----------------------------------------------------

class TestOperandFromScalar:
    def _op(self, text, style=None):
        return operand_from_scalar(text, style, 1, 1, "value")

    def test_quoted_is_expression(self):
        assert self._op("sig.speed > 0", style="'").kind == "expression"

    def test_booleans(self):
        assert self._op("true").bool_value is True
        assert self._op("false").bool_value is False

    def test_integers(self):
        assert self._op("10").int_value == 10
        assert self._op("0x10").int_value == 16
        assert self._op("0o17").int_value == 15
        assert self._op("0b101").int_value == 5
        assert self._op("-7").int_value == -7

    def test_reals(self):
        assert self._op("0.5").real_value == 0.5
        assert self._op("1e3").real_value == 1000.0
        assert self._op("10000000000000000000").kind == "real"
        assert self._op("18446744073709551615").kind == "real"

    def test_non_finite_becomes_an_expression(self):
        # float("1e999") is infinite, so the number classifier refuses it and
        # the scalar falls through to the expression grammar, exactly like
        # the C loader's operand parser does.
        assert self._op("1e999").kind == "expression"

    def test_broken_numbers_refused(self):
        for text in ("007", "1e", "1E+", "12.", "010"):
            with pytest.raises(Yaml2cError) as info:
                self._op(text)
            assert "not a representable number" in str(info.value), text

    def test_expression_passthrough(self):
        assert self._op("sig.speed").kind == "expression"


# --- compose_document and loader_view ----------------------------------------

class TestLoaderView:
    def test_loader_scalar_grammar(self, tmp_path):
        view = loader_view(compose(tmp_path, "a: true\nb: false\nc: 007\n"
                                           "d: 1_0\ne: no\nf: .inf\n"
                                           "g: 0x10\nh: 1.5\ni: 'x'\n"
                                           "j: 'true'\n"))
        assert view["a"] is True and view["b"] is False
        # The loader number grammar refuses 007 and 1_0: the view keeps no
        # value (None), which the schema layer rejects downstream.
        assert view["c"] is None and view["d"] is None
        # YAML 1.1 extras stay raw text, exactly what the C loaders scan.
        assert view["e"] == "no" and view["f"] == ".inf"
        assert view["g"] == 16 and view["h"] == 1.5
        assert view["i"] == "x" and view["j"] == "true"

    def test_nested_containers(self, tmp_path):
        view = loader_view(compose(tmp_path, "a:\n  - 1\n  - b: 2"))
        assert view["a"][0] == 1 and view["a"][1]["b"] == 2

    def test_duplicate_key_refused(self, tmp_path):
        with pytest.raises(Yaml2cError) as info:
            loader_view(compose(tmp_path, "a: 1\na: 2"))
        assert "duplicate mapping key 'a'" in str(info.value)

    def test_non_scalar_key_refused(self, tmp_path):
        with pytest.raises(Yaml2cError):
            loader_view(compose(tmp_path, "? [a]\n: 1"))

    def test_tagged_scalars_pass_through_as_text(self, tmp_path):
        # The C loaders are text scanners: a tagged scalar contributes its
        # text, and schema-level constraints reject it where not allowed.
        view = loader_view(compose(tmp_path, "a: !!binary aGk="))
        assert view["a"] == "aGk="

    def test_none_is_none(self):
        assert loader_view(None) is None


class TestBranchCompletion:
    """Precision tests for the number-grammar branches (loader parity)."""

    def _op(self, text):
        return operand_from_scalar(text, None, 1, 1, "value")

    def test_hex_with_trailing_text_becomes_an_expression(self):
        # 0x1g: the base-16 scan stops at 'g'; trailing text means "this is
        # an expression", not a number, exactly like the C loader.
        assert self._op("0x1g").kind == "expression"

    def test_negative_hex(self):
        assert self._op("-0x10").int_value == -16

    def test_hex_overflow_becomes_real(self):
        assert self._op("0xFFFFFFFFFFFFFFFFFF").kind == "real"

    def test_decimal_trailing_text_becomes_an_expression(self):
        assert self._op("12abc").kind == "expression"

    def test_zero_dot_is_an_expression(self):
        assert self._op("0.").kind == "expression"

    def test_identifier_with_digit_is_an_expression(self):
        assert self._op("a1").kind == "expression"

    def test_quoted_uint_is_refused(self, tmp_path):
        with pytest.raises(Yaml2cError) as info:
            require_uint(_scalar(tmp_path, "'5'"), "dlc")
        assert "must be an integer" in str(info.value)

    def test_mark_of_markless_node(self):
        assert common._mark_of(object()) == (None, None)

    def test_loader_scalar_float_spellings(self):
        from types import SimpleNamespace

        def node(tag, value):
            return SimpleNamespace(tag=tag, value=value, style=None,
                                   start_mark=SimpleNamespace(line=0,
                                                              column=0))

        float_tag = "tag:yaml.org,2002:float"
        # Underscored floats keep their text (the loaders refuse them).
        assert loader_view(node(float_tag, "1_0.5")) == "1_0.5"
        # Float text with a dot that cannot parse keeps its text.
        assert loader_view(node(float_tag, "1.5x")) == "1.5x"

    def test_int_if_loader_integer_error_paths(self):
        assert common._int_if_loader_integer("0xZZ") is None
        assert common._int_if_loader_integer("0b12") is None
        assert common._int_if_loader_integer("") is None
