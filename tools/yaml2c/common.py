"""Shared helpers for the CANcestry yaml2c code generator (issue #22).

Implements: SW-FR-TOOL-001, SW-FR-TOOL-002 (see docs/software/SwRS.md
section 13), SW-FR-TOOL-003 (zero-allocation const output).

The generator walks the *composed* YAML node tree (``yaml.compose``) rather
than the plain Python data, because two pieces of information the C loaders
use are only available there:

  * the original scalar text and quoting style, which decide whether an
    ``expression_or_literal`` value is a literal or an expression exactly the
    way ``fsm_operand()`` in core/fsm/src/loader.c decides it, and
  * the source line of every node, which the loaders store in
    ``cancestry_fsm_action_t.source_line`` for trace correlation.

Name length limits mirror the limits the C headers define; a document the
loader would reject must be rejected here as well (fail closed), because the
generated header replaces the loader on the target.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# Limits (mirrors of the limits in the C headers; keep in sync).
# ---------------------------------------------------------------------------

FSM_NAME_MAX = 64               # CANCESTRY_FSM_NAME_MAX
FSM_DESCRIPTION_MAX = 512       # CANCESTRY_FSM_DESCRIPTION_MAX
FSM_EXPRESSION_MAX = 256        # CANCESTRY_FSM_EXPRESSION_MAX
FSM_LOG_MESSAGE_MAX = 256       # CANCESTRY_FSM_LOG_MESSAGE_MAX
FSM_FAULT_CODE_MAX = 64         # CANCESTRY_FSM_FAULT_CODE_MAX

CODEC_NAME_MAX = 64             # CANCESTRY_CODEC_NAME_MAX
CODEC_UNIT_MAX = 32             # CANCESTRY_CODEC_UNIT_MAX
CODEC_LABEL_MAX = 64            # CANCESTRY_CODEC_LABEL_MAX
CODEC_VERSION_MAX = 32          # CODEC_LOAD_VERSION_MAX (core/codec/src/loader.c)
CODEC_DESCRIPTION_MAX = 512     # CODEC_LOAD_DESCRIPTION_MAX (core/codec/src/loader.c)

UINT16_MAX = 0xFFFF


class Yaml2cError(Exception):
    """A document the generator refuses to translate.

    ``line``/``column`` are 1-based positions in the input file when known,
    mirroring the loader error reports. The CLI turns these into messages and
    a non-zero exit status; nothing is generated for a refused document.
    """

    def __init__(self, message: str, line: Optional[int] = None,
                 column: Optional[int] = None) -> None:
        location = ""
        if line is not None:
            location = " (line %d, column %d)" % (line, column if column else 1)
        super().__init__("%s%s" % (message, location))
        self.message = message
        self.line = line
        self.column = column


# ---------------------------------------------------------------------------
# Scalar classification (the expression_or_literal rules of the C loader).
# ---------------------------------------------------------------------------

@dataclass
class Operand:
    """The three forms schema ``expression_or_literal`` can take.

    ``kind`` is one of ``"bool"``, ``"int"``, ``"real"`` or ``"expression"``,
    mirroring the loader's classification order: exact ``true``/``false``,
    then the YAML-subset number grammar, then "a string is an expression".
    """

    kind: str
    bool_value: bool = False
    int_value: int = 0
    real_value: float = 0.0
    expression: str = ""


def _classify_number(text: str) -> Optional[Operand]:
    """Classify plain scalar text like fsm_scan_number() in the FSM loader.

    Returns an Operand for an integer or real, or None when the text is not a
    number at all (which makes it an expression). Malformed near-numbers such
    as ``007`` or ``1e`` raise Yaml2cError from the caller, matching the
    loader's "not a representable number" refusal.
    """
    i = 0
    length = len(text)
    negative = False
    if text and text[0] in "+-":
        negative = text[0] == "-"
        i = 1
    base = 10
    # 0x/0o/0b prefixed integers; a leading zero before another digit (007)
    # is YAML 1.1 octal and refused exactly like the C loader refuses it.
    if i + 1 < length and text[i] == "0" and text[i + 1] in "xX":
        base = 16
        i += 2
    elif i + 1 < length and text[i] == "0" and text[i + 1] in "oO":
        base = 8
        i += 2
    elif i + 1 < length and text[i] == "0" and text[i + 1] in "bB":
        base = 2
        i += 2
    elif i + 1 < length and text[i] == "0" and text[i + 1].isdigit():
        return None  # the loader refuses it with "not a representable number"

    digits = "0123456789" if base == 10 else "0123456789abcdef"[:base]
    digits += digits.upper()
    magnitude = 0
    seen_digit = False
    overflow = False
    while i < length and text[i] in digits:
        seen_digit = True
        digit = int(text[i], 16 if base == 16 else base)
        if magnitude > (0xFFFF_FFFF_FFFF_FFFF - digit) // base:
            overflow = True
        else:
            magnitude = magnitude * base + digit
        i += 1
    if not seen_digit:
        return None
    if base != 10:
        if i != length:
            return None  # trailing text: an expression
        if negative:
            magnitude = -magnitude
        if overflow or magnitude > 0x7FFF_FFFF_FFFF_FFFF:
            return Operand("real", real_value=float(magnitude))
        return Operand("int", int_value=int(magnitude))

    # Decimal: an explicit fraction or exponent makes it a real.
    is_real = False
    if i < length and text[i] == ".":
        if i + 1 < length and text[i + 1].isdigit():
            is_real = True
            i += 1
            while i < length and text[i].isdigit():
                i += 1
        else:
            return None  # "1." is not a number: it would swallow a sentence
    if i < length and text[i] in "eE":
        j = i + 1
        if j < length and text[j] in "+-":
            j += 1
        if not any(c.isdigit() for c in text[j:]):
            return None  # malformed exponent
        is_real = True
        i = j
        while i < length and text[i].isdigit():
            i += 1
    if i != length:
        return None  # trailing text: an expression
    sign = -1.0 if negative else 1.0
    if is_real or overflow or magnitude > 0x7FFF_FFFF_FFFF_FFFF:
        try:
            value = sign * float(text)
        except ValueError:  # pragma: no cover - digit scan guarantees parse
            return None
        # The loader refuses non-finite results; mirror that (fail closed).
        if value != value or value in (float("inf"), float("-inf")):
            return None
        return Operand("real", real_value=value)
    return Operand("int", int_value=magnitude if not negative else -magnitude)


def operand_from_scalar(value: str, style: Optional[str], line: int, column: int,
                        what: str) -> Operand:
    """Interpret a scalar the way ``fsm_operand()`` does.

    ``value`` is the resolved scalar text and ``style`` the YAML style of the
    node (None for a plain scalar). A quoted scalar is always a string, hence
    an expression; a plain scalar is a boolean, a number, or an expression.
    """
    if style is not None:
        return Operand("expression", expression=value)
    if value == "true":
        return Operand("bool", bool_value=True)
    if value == "false":
        return Operand("bool", bool_value=False)
    number = _classify_number(value)
    if number is not None:
        return number
    if any(c.isdigit() for c in value) and _looks_like_broken_number(value):
        raise Yaml2cError("%s is not a representable number" % what, line, column)
    return Operand("expression", expression=value)


def _looks_like_broken_number(value: str) -> bool:
    """True for the malformed near-numbers the C loader refuses (007, 1e)."""
    text = value[1:] if value and value[0] in "+-" else value
    if text.startswith(("0x", "0X", "0o", "0O", "0b", "0B")):
        return False
    if text[:2] == "0." or text == "0":
        return False
    if len(text) > 1 and text[0] == "0" and text[1].isdigit():
        return True  # leading-zero octal spelling: the loader refuses it
    if text and text[0].isdigit() and not text.isdigit():
        tail = text.lstrip("0123456789")
        return tail in ("e", "E", "e+", "E+", "e-", "E-", ".")
    return False


# ---------------------------------------------------------------------------
# The "loader view" of a composed tree.
# ---------------------------------------------------------------------------

def _is_map(node) -> bool:
    """True for a YAML mapping node (a sequence node has a list value too)."""
    return (node is not None and hasattr(node, "tag")
            and str(node.tag).endswith(":map"))


def _is_seq(node) -> bool:
    return (node is not None and hasattr(node, "tag")
            and str(node.tag).endswith(":seq"))


def _is_scalar_node(node) -> bool:
    return (node is not None and hasattr(node, "tag")
            and not str(node.tag).endswith((":seq", ":map")))


def _int_if_loader_integer(text: str):
    """int(text) when the C loaders accept the spelling, else None.

    The loaders accept plain decimal (no leading zero), 0x/0o/0b and a sign;
    YAML 1.1 extras such as ``010`` or ``1_000`` are refused by them, so the
    loader view keeps the raw text and the schema layer rejects it.
    """
    body = text[1:] if text and text[0] in "+-" else text
    try:
        if body[:2].lower() in ("0x", "0o", "0b"):
            return int(text, {"0x": 16, "0o": 8, "0b": 2}[body[:2].lower()])
        if body.isdigit() and (len(body) == 1 or body[0] != "0") and "_" not in text:
            return int(text, 10)
    except ValueError:
        return None
    return None


def _loader_scalar(node):
    """One scalar the way the C loaders resolve it.

    Exact ``true``/``false`` plain scalars are the only booleans (the loaders
    implement exactly that); YAML 1.1 extras such as ``OFF`` or ``yes`` keep
    their raw text, which is what the hand-written loaders see. Integers and
    floats follow the loader number grammar; ``.inf``/``.nan`` and friends
    keep their text and are rejected downstream, like the loaders reject
    non-finite numbers.
    """
    tag = str(getattr(node, "tag", ""))
    text = node.value if isinstance(node.value, str) else ""
    if tag.endswith(":bool"):
        if text == "true":
            return True
        if text == "false":
            return False
        return text
    if tag.endswith(":int"):
        return _int_if_loader_integer(text) if text is not None else text
    if tag.endswith(":float"):
        if "_" in text or "." not in text and "e" not in text.lower():
            return text
        try:
            value = float(text)
        except ValueError:
            return text
        # PyYAML only tags finite floats here (.inf/.NaN fail float() above
        # and keep their text via the handler), so this is belt and braces.
        if value != value or value in (float("inf"), float("-inf")):  # pragma: no cover
            return text
        return value
    return text


def loader_view(node):
    """Plain Python data mirroring what the C loaders see in the document.

    This is the view the schema validation runs on (SW-FR-TOOL-004): it is
    built from the composed tree, so quoting styles, ``OFF``-as-text and the
    loader number grammar all agree with the generators below. Duplicate
    mapping keys are refused here, like every C loader refuses them.
    """
    if node is None:
        return None
    if _is_seq(node):
        return [loader_view(item) for item in node.value]
    if _is_map(node):
        result = {}
        for key_node, value_node in node.value:
            if not _is_scalar_node(key_node):
                raise Yaml2cError("mapping key must be a scalar",
                                  *_mark_of(key_node))
            # Keys become their raw text: the C loaders scan key text, so a
            # codec-map value key "0" is the text "0" whether or not it was
            # quoted, and JSON Schema propertyNames patterns see strings.
            key = key_node.value if isinstance(key_node.value, str) else ""
            if key in result:
                raise Yaml2cError("duplicate mapping key '%s'" % key,
                                  *_mark_of(key_node))
            result[key] = loader_view(value_node)
        return result
    return _loader_scalar(node)


def _mark_of(node):
    mark = getattr(node, "start_mark", None)
    if mark is not None:
        return mark.line + 1, mark.column + 1
    return None, None


def require_text(node, what: str, maximum: Optional[int] = None) -> Tuple[str, int, int]:
    """Extract scalar text with an optional byte-length limit (UTF-8 bytes).

    The loaders measure limits in bytes, so names with non-ASCII characters
    hit the limit earlier than a Python len() check would suggest; mirror that.
    """
    if node is None or not hasattr(node, "value") or isinstance(node.value, list):
        mark = getattr(node, "start_mark", None)
        raise Yaml2cError("%s must be a string" % what,
                          mark.line + 1 if mark else None, mark.column + 1 if mark else None)
    text = node.value
    if not isinstance(text, str):
        raise Yaml2cError("%s must be a string" % what, node.start_mark.line + 1,
                          node.start_mark.column + 1)
    if maximum is not None and len(text.encode("utf-8")) > maximum:
        raise Yaml2cError("%s is longer than %d characters" % (what, maximum),
                          node.start_mark.line + 1, node.start_mark.column + 1)
    return text, node.start_mark.line + 1, node.start_mark.column + 1


def require_bool(node, what: str) -> bool:
    """Strictly ``true``/``false`` plain scalars, like fsm_scalar_bool()."""
    text, line, column = require_text(node, what)
    if node.style is not None or text not in ("true", "false"):
        raise Yaml2cError("%s must be true or false" % what, line, column)
    return text == "true"


def require_uint(node, what: str, minimum: int = 0,
                 maximum: int = 0xFFFFFFFF) -> int:
    """Plain scalar integer in the decimal/0x/0o/0b forms the loaders accept."""
    text, line, column = require_text(node, what)
    if node.style is not None:
        raise Yaml2cError("%s must be an integer" % what, line, column)
    negative = text.startswith("-")
    try:
        if text[:2].lower() in ("0x", "0o", "0b"):
            value = int(text, {"0x": 16, "0o": 8, "0b": 2}[text[:2].lower()])
        else:
            if len(text) > 1 and text[0] == "0" and text[1].isdigit():
                raise Yaml2cError(
                    "%s: a leading zero means an octal literal; use 0o" % what, line, column)
            value = int(text, 10)
    except ValueError:
        raise Yaml2cError("%s must be an integer" % what, line, column) from None
    if negative or value < minimum or value > maximum:
        raise Yaml2cError("%s is out of range" % what, line, column)
    return value


# ---------------------------------------------------------------------------
# C emission.
# ---------------------------------------------------------------------------

def c_bytes(text: str) -> bytes:
    """UTF-8 bytes of a string, as the C compiler will store them."""
    return text.encode("utf-8")


def c_string_literal(text: str) -> str:
    """Render a Python string as a C string literal.

    Bytes outside printable ASCII become octal escapes (never hex escapes: a
    following hex digit would extend the escape); ``?`` is escaped so no
    trigraph can ever form, whatever the compiler's mode.
    """
    out = ['"']
    for byte in c_bytes(text):
        if byte == 0x22:  # "
            out.append('\\"')
        elif byte == 0x5C:  # backslash
            out.append("\\\\")
        elif byte == 0x3F:  # ?
            out.append("\\?")
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append("\\%03o" % byte)
    out.append('"')
    return "".join(out)


def c_double(value: float) -> str:
    """Render a finite double as a C double constant with an explicit point."""
    if value != value or value in (float("inf"), float("-inf")):
        raise Yaml2cError("non-finite floating point value cannot be generated")
    text = "%.17g" % value
    if "." not in text and "e" not in text and "E" not in text and "n/a" not in text:
        text += ".0"
    if text.startswith("-"):
        text = "(%s)" % text
    return text


def c_int(value: int, cast: str) -> str:
    """Render an integer with an explicit cast so -Wconversion stays quiet."""
    if value < -0x8000_0000_0000_0000 or value > 0xFFFF_FFFF_FFFF_FFFF:
        raise Yaml2cError("integer %d does not fit in 64 bits" % value)
    return "(%s)%s%d" % (cast, "" if value >= 0 else "-", abs(value))


def symbolize(text: str) -> str:
    """Turn a file stem into a legal C identifier fragment."""
    out = []
    for ch in text:
        out.append(ch if ch.isalnum() and ch.isascii() else "_")
    symbol = "".join(out)
    if not symbol or symbol[0].isdigit():
        symbol = "_" + symbol
    return symbol


class StringTable:
    """Interns strings and hands out stable symbol names for them."""

    def __init__(self, prefix: str) -> None:
        self._prefix = prefix
        self._index: Dict[str, int] = {}
        self._order: List[str] = []

    def intern(self, text: str) -> str:
        """Return the C symbol of the static char array holding ``text``."""
        if text not in self._index:
            self._index[text] = len(self._order)
            self._order.append(text)
        return "%s_str_%03d" % (self._prefix, self._index[text])

    def emit(self, emit: "Emitter") -> None:
        for text in self._order:
            emit.line("static const char %s[] = %s;"
                      % (self.intern(text), c_string_literal(text)))
        if self._order:
            emit.blank()


class Emitter:
    """Small indented line writer used by both code generators."""

    def __init__(self) -> None:
        self._parts: List[str] = []
        self._indent = 0

    def line(self, text: str = "") -> None:
        self._parts.append(("    " * self._indent + text) if text else "")

    def blank(self) -> None:
        self._parts.append("")

    def comment(self, text: str) -> None:
        self.line("/* %s */" % text)

    def push(self) -> None:
        self._indent += 1

    def pop(self) -> None:
        self._indent -= 1

    def open(self, head: str) -> None:
        self.line(head)
        self.push()

    def close(self, tail: str, comma: bool = False) -> None:
        self.pop()
        self.line(tail + ("," if comma else ""))

    def text(self) -> str:
        return "\n".join(self._parts) + "\n"


def header_guard(filename: str) -> str:
    """Derive the include guard name from the output file name."""
    base = filename.replace("\\", "/").split("/")[-1]
    base = base.rsplit(".", 1)[0] if "." in base else base
    guard = symbolize(base).upper()
    return "%s_H" % guard


def emit_prelude(emit: Emitter, guard: str, kind: str, include: str,
                 source: str, tool_version: str) -> None:
    """Banner comment, include guard and the runtime types include."""
    emit.line("/*")
    emit.line(" * Generated by CANcestry yaml2c %s -- DO NOT EDIT." % tool_version)
    emit.line(" *")
    emit.line(" * Source:   %s" % source)
    emit.line(" * Artifact: static const C definitions for a %s document." % kind)
    emit.line(" *")
    emit.line(" * The definitions below are const static data with no dynamic")
    emit.line(" * lifetime: including this header replaces the runtime YAML")
    emit.line(" * loader entirely (SW-FR-TOOL-001..003, SYS-NF-002). Every struct")
    emit.line(" * maps 1:1 onto the runtime types of %s." % include)
    emit.line(" */")
    emit.blank()
    emit.line("#ifndef %s" % guard)
    emit.line("#define %s" % guard)
    emit.blank()
    emit.line("#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L")
    emit.line('#error "yaml2c generated headers require C99 or later"')
    emit.line("#endif")
    emit.blank()
    emit.line("#include %s" % c_string_literal(include))
    emit.blank()


def emit_epilogue(emit: Emitter, guard: str) -> None:
    emit.blank()
    emit.line("#endif /* %s */" % guard)


def check_count(count: int, what: str, node=None) -> int:
    """All array counts land in uint16_t fields; refuse more like the loader."""
    if count > UINT16_MAX:
        mark = getattr(node, "start_mark", None)
        raise Yaml2cError("%s exceeds %d entries" % (what, UINT16_MAX),
                          mark.line + 1 if mark else None)
    return count


def map_pairs(node, what: str) -> Sequence[Tuple[object, object]]:
    """YAML mapping pairs in document order, refusing non-mappings."""
    if not _is_map(node):
        mark = getattr(node, "start_mark", None) if node is not None else None
        raise Yaml2cError("%s must be a mapping" % what,
                          mark.line + 1 if mark else None)
    return node.value


def seq_items(node, what: str) -> Sequence[object]:
    """YAML sequence items in document order, refusing non-sequences."""
    if not _is_seq(node):
        mark = getattr(node, "start_mark", None) if node is not None else None
        raise Yaml2cError("%s must be a sequence" % what,
                          mark.line + 1 if mark else None)
    return node.value


def map_value(node, key: str):
    """The value node of ``key`` in a mapping node, or None."""
    for key_node, value_node in map_pairs(node, "mapping"):
        if getattr(key_node, "value", None) == key:
            return value_node
    return None


