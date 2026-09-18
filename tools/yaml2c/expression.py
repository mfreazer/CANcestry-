"""Load-time expression grammar validation for yaml2c (issue #22).

Implements: SW-FR-TOOL-001. Normative references:
docs/system/expression-language.md section 2 (the grammar) and
core/fsm/src/expression.c, which the FSM loader runs over every guard,
default and operand expression before accepting a file.

The generated C header replaces the loader on the target, so an expression
the loader would refuse must be refused here as well (fail closed). This
module ports the accepted grammar, including the details that only show up
in expression.c:

  * ``evt.`` fields are static vocabulary; an unknown field is a definition
    error even in validate-only mode (fsm_expr_field_known),
  * ``sig.``/``var.`` names are checked for length only in validate-only
    mode; the suffix must be non-empty and fit the 64-byte name buffer,
  * function arity is checked at parse time (abs/round 1, min/max 2,
    clamp 3; more than 3 arguments is refused),
  * numbers are decimal with an optional fraction and exponent; ``.5`` and
    ``1e3`` are numbers, ``1.`` and ``1e`` are syntax errors.

Name resolution (does ``sig.X`` exist?) stays a runtime concern, exactly as
in the C loader, because it depends on the installed codec namespace.
"""

from __future__ import annotations

from typing import List, Optional

from .common import Yaml2cError

_TOKEN_NAME = "name"
_TOKEN_NUMBER = "number"
_TOKEN_OP = "op"
_TOKEN_EOF = "eof"

# fsm_expr_event_fields[] of core/fsm/src/expression.c (validate-only set).
EVT_FIELDS = (
    "event_id", "timestamp_us", "sequence", "cause_sequence", "priority_class",
    "type", "interface_id", "can_id", "length", "signal_id",
    "value", "timer_id", "instance_id", "missed_count", "state_id",
    "fault_code", "severity", "from_mode", "to_mode", "reason_code",
)

# fsm_expr_apply_call() arities: name -> required argument count.
FUNCTION_ARITY = {"abs": 1, "round": 1, "min": 2, "max": 2, "clamp": 3}

_MAX_CALL_ARGS = 3
_NAME_BUFFER_MAX = 64  # CANCESTRY_FSM_NAME_MAX; suffix must be < 64 bytes

_KEYWORDS = ("and", "or", "not", "true", "false")


class _Token:
    __slots__ = ("kind", "text", "offset")

    def __init__(self, kind: str, text: str, offset: int) -> None:
        self.kind = kind
        self.text = text
        self.offset = offset


def _is_ident_start(ch: str) -> bool:
    return ch.isalpha() or ch == "_"


def _is_ident_char(ch: str) -> bool:
    return ch.isalnum() or ch == "_"


def _scan_number(text: str, start: int) -> int:
    """Scan a decimal number (optionally .5, 1.25, 1e3); return the end."""
    i = start
    length = len(text)
    while i < length and text[i].isdigit():
        i += 1
    if i < length and text[i] == "." and i + 1 < length and text[i + 1].isdigit():
        i += 1
        while i < length and text[i].isdigit():
            i += 1
    if i < length and text[i] in "eE":
        j = i + 1
        if j < length and text[j] in "+-":
            j += 1
        if j < length and text[j].isdigit():
            i = j
            while i < length and text[i].isdigit():
                i += 1
        # An exponent without digits stays unconsumed: the char is a syntax
        # error, exactly as fsm_expr_parse_number_impl leaves it behind.
    return i


def _tokenize(text: str) -> List[_Token]:
    tokens: List[_Token] = []
    i = 0
    length = len(text)
    while i < length:
        ch = text[i]
        if ch in " \t\r\n":
            i += 1
            continue
        if ch.isdigit() or (ch == "." and i + 1 < length and text[i + 1].isdigit()):
            end = _scan_number(text, i)
            tokens.append(_Token(_TOKEN_NUMBER, text[i:end], i))
            i = end
            continue
        if _is_ident_start(ch):
            start = i
            while i < length and (_is_ident_char(text[i]) or text[i] == "."):
                i += 1
            tokens.append(_Token(_TOKEN_NAME, text[start:i], start))
            continue
        for op in ("==", "!=", "<=", ">="):
            if text.startswith(op, i):
                tokens.append(_Token(_TOKEN_OP, op, i))
                i += len(op)
                break
        else:
            if ch in "<>+-*/%(),":
                tokens.append(_Token(_TOKEN_OP, ch, i))
                i += 1
            else:
                raise Yaml2cError("unexpected character %r" % ch, column=i + 1)
    tokens.append(_Token(_TOKEN_EOF, "", length))
    return tokens


class _Parser:
    """Recursive descent parser for expression-language.md section 2."""

    def __init__(self, tokens: List[_Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    def _peek(self) -> _Token:
        return self._tokens[self._pos]

    def _advance(self) -> _Token:
        token = self._tokens[self._pos]
        if token.kind != _TOKEN_EOF:
            self._pos += 1
        return token

    def _accept_op(self, *ops: str) -> Optional[_Token]:
        token = self._peek()
        if token.kind == _TOKEN_OP and token.text in ops:
            return self._advance()
        return None

    def parse(self) -> None:
        self._or_expr()
        token = self._peek()
        if token.kind != _TOKEN_EOF:
            raise Yaml2cError("trailing input %r" % token.text,
                              column=token.offset + 1)

    def _or_expr(self) -> None:
        self._and_expr()
        while True:
            token = self._peek()
            if token.kind == _TOKEN_NAME and token.text == "or":
                self._advance()
                self._and_expr()
            else:
                return

    def _and_expr(self) -> None:
        self._not_expr()
        while True:
            token = self._peek()
            if token.kind == _TOKEN_NAME and token.text == "and":
                self._advance()
                self._not_expr()
            else:
                return

    def _not_expr(self) -> None:
        token = self._peek()
        if token.kind == _TOKEN_NAME and token.text == "not":
            self._advance()
            self._not_expr()
        else:
            self._cmp_expr()

    def _cmp_expr(self) -> None:
        self._add_expr()
        if self._accept_op("==", "!=", "<=", ">=", "<", ">") is not None:
            self._add_expr()

    def _add_expr(self) -> None:
        self._mul_expr()
        while self._accept_op("+", "-") is not None:
            self._mul_expr()

    def _mul_expr(self) -> None:
        self._unary()
        while self._accept_op("*", "/", "%") is not None:
            self._unary()

    def _unary(self) -> None:
        if self._accept_op("-") is not None:
            self._unary()
        else:
            self._postfix()

    def _postfix(self) -> None:
        token = self._peek()
        if token.kind == _TOKEN_NAME and token.text in FUNCTION_ARITY \
                and self._tokens[self._pos + 1].kind == _TOKEN_OP \
                and self._tokens[self._pos + 1].text == "(":
            self._call(token)
            return
        self._primary()

    def _call(self, token: _Token) -> None:
        self._advance()  # function name
        self._advance()  # '('
        argc = 0
        if not (self._peek().kind == _TOKEN_OP and self._peek().text == ")"):
            while True:
                if argc >= _MAX_CALL_ARGS:
                    raise Yaml2cError("too many arguments to %r" % token.text,
                                      column=token.offset + 1)
                self._or_expr()
                argc += 1
                if self._accept_op(",") is None:
                    break
        if self._accept_op(")") is None:
            raise Yaml2cError("expected ')' to close the call to %r" % token.text,
                              column=token.offset + 1)
        if argc != FUNCTION_ARITY[token.text]:
            raise Yaml2cError("%s() takes %d argument(s), got %d"
                              % (token.text, FUNCTION_ARITY[token.text], argc),
                              column=token.offset + 1)

    def _primary(self) -> None:
        token = self._peek()
        if token.kind == _TOKEN_NUMBER:
            self._advance()
            return
        if token.kind == _TOKEN_OP and token.text == "(":
            self._advance()
            self._or_expr()
            if self._accept_op(")") is None:
                raise Yaml2cError("expected ')'", column=token.offset + 1)
            return
        if token.kind == _TOKEN_NAME:
            if token.text in ("true", "false"):
                self._advance()
                return
            if token.text in _KEYWORDS:
                raise Yaml2cError("unexpected keyword %r" % token.text,
                                  column=token.offset + 1)
            self._namespaced(token)
            return
        raise Yaml2cError("unexpected token %r" % (token.text or "<end>"),
                          column=token.offset + 1)

    def _namespaced(self, token: _Token) -> None:
        """Check a namespaced identifier the way fsm_expr_resolve() does."""
        name = token.text
        prefix = name.split(".", 1)[0]
        suffix = name[len(prefix) + 1:] if "." in name else ""
        if prefix == "evt" and name.startswith("evt."):
            if suffix not in EVT_FIELDS:
                raise Yaml2cError("unknown evt. field %r" % suffix,
                                  column=token.offset + 1)
        elif prefix in ("sig", "var") and name.startswith(prefix + "."):
            if not suffix:
                raise Yaml2cError("empty name after %r. prefix" % prefix,
                                  column=token.offset + 1)
            if len(suffix.encode("utf-8")) >= _NAME_BUFFER_MAX:
                raise Yaml2cError("name after %r. is longer than %d characters"
                                  % (prefix, _NAME_BUFFER_MAX - 1),
                                  column=token.offset + 1)
        else:
            raise Yaml2cError(
                "bare identifiers are not permitted (%r); use sig., var. or evt."
                % name, column=token.offset + 1)
        self._advance()


def validate_expression(text: str, what: str, line: int, column: int,
                        maximum: int) -> None:
    """Refuse an expression the C loader would refuse at load time.

    ``what`` names the expression site in error messages (e.g. "guard");
    ``line``/``column`` locate the scalar in the YAML source; ``maximum`` is
    the loader's length limit for the site (CANCESTRY_FSM_EXPRESSION_MAX).
    """
    if len(text.encode("utf-8")) > maximum:
        raise Yaml2cError("%s is longer than %d characters" % (what, maximum),
                          line, column)
    if not text.strip():
        raise Yaml2cError("%s is not a valid expression: empty" % what, line, column)
    try:
        _Parser(_tokenize(text)).parse()
    except Yaml2cError as error:
        raise Yaml2cError("%s is not a valid expression: %s" % (what, error.message),
                          line, column) from None
