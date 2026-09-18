"""Codec map -> static C definitions (issue #22, SW-FR-TOOL-002).

Consumes the composed YAML node tree of a codec map validated against
schemas/codec-map-0.3.0.schema.json and emits ``static const`` definitions
initializing ``cancestry_codec_map_t`` 1:1 with ``cancestry/codec/types.h``.

The compile step mirrors core/codec/src/loader.c:

  * duplicate message ids, message names and signal names are conflicts,
  * ``scale == 0`` on a uint/int signal is refused (it cannot be encoded),
  * enum signals require a ``values`` mapping whose keys are non-negative
    integers that fit the signal's bit length,
  * the bit-model sanity rules use the *map-level* payload limit (64 bits
    classic, 512 for ``can_fd: true``), exactly like the loader: a signal may
    address payload bits its message's ``dlc`` does not cover, because a
    short frame only becomes a decode warning at run time,
  * the derived fields ``first_bit``/``last_bit``/``message_id`` use exactly
    the loader's formulas, including the sawtooth min/max walk,
  * name/unit/label/version/description length limits match the C headers.

Emission is bottom-up (strings, value mappings, signals, messages, the map)
with const static data only: zero dynamic allocation (SW-FR-TOOL-003).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple

from .common import (CODEC_DESCRIPTION_MAX, CODEC_LABEL_MAX, CODEC_NAME_MAX,
                     CODEC_UNIT_MAX, CODEC_VERSION_MAX, Emitter, StringTable,
                     Yaml2cError, c_double, check_count, emit_epilogue,
                     emit_prelude, header_guard, loader_view, map_pairs,
                     map_value, require_bool, require_text, require_uint,
                     seq_items, symbolize)

_SIGNAL_TYPES = {
    "uint": "CANCESTRY_CODEC_SIGNAL_TYPE_UINT",
    "int": "CANCESTRY_CODEC_SIGNAL_TYPE_INT",
    "boolean": "CANCESTRY_CODEC_SIGNAL_TYPE_BOOLEAN",
    "enum": "CANCESTRY_CODEC_SIGNAL_TYPE_ENUM",
}

_ENDIANNESS = {
    "little": "CANCESTRY_CODEC_ENDIANNESS_LITTLE",
    "big": "CANCESTRY_CODEC_ENDIANNESS_BIG",
}

_LAYOUTS = {
    "contiguous": "CANCESTRY_CODEC_LAYOUT_CONTIGUOUS",
    "sawtooth": "CANCESTRY_CODEC_LAYOUT_SAWTOOTH",
}

# CAN FD payload lengths representable on the wire (SW-FR-CANFD-001).
_FD_PAYLOAD_LENGTHS = (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)


@dataclass
class ValueMappingModel:
    raw: int
    label: str


@dataclass
class SignalModel:
    name: str
    start_bit: int
    bit_length: int
    type_name: str
    endianness: str
    scale: float = 1.0
    offset: float = 0.0
    unit: Optional[str] = None
    values: List[ValueMappingModel] = field(default_factory=list)
    has_min: bool = False
    has_max: bool = False
    min: float = 0.0
    max: float = 0.0
    strict: bool = True
    layout: str = "contiguous"
    first_bit: int = 0
    last_bit: int = 0
    message_id: int = 0
    values_name: Optional[str] = None  # emit-time symbol


@dataclass
class MessageModel:
    id: int
    name: str
    dlc: int
    period_ms: int = 0
    description: Optional[str] = None
    signals: List[SignalModel] = field(default_factory=list)
    signals_name: Optional[str] = None  # emit-time symbol


@dataclass
class CodecMapModel:
    name: str
    version: str
    description: Optional[str] = None
    can_fd: bool = False
    messages: List[MessageModel] = field(default_factory=list)


def _mark(node):
    if node is not None and hasattr(node, "start_mark") and node.start_mark:
        return node.start_mark.line + 1, node.start_mark.column + 1
    return None, None


def _require_number(node, what: str) -> float:
    """A schema "number": int or float, but never NaN or infinity."""
    if node is None or not hasattr(node, "style") or \
            isinstance(getattr(node, "value", None), list):
        raise Yaml2cError("%s must be a number" % what, *_mark(node))
    value = node.value
    if isinstance(value, bool):
        raise Yaml2cError("%s must be a number" % what, *_mark(node))
    if isinstance(value, int):
        return float(value)
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            raise Yaml2cError("%s must be a finite number" % what, *_mark(node))
        return value
    text, line, column = require_text(node, what)
    try:
        result = float(text)
    except ValueError:
        raise Yaml2cError("%s must be a number" % what, line, column) from None
    if result != result or result in (float("inf"), float("-inf")):
        raise Yaml2cError("%s must be a finite number" % what, line, column)
    return result


def _c_uint64(value: int) -> str:
    """uint64_t constant that also survives values above 32 bits."""
    return "%dULL" % value if value > 0xFFFFFFFF else "%du" % value


class CodecMapReader:
    """Walks the composed YAML tree and validates it like the C loader."""

    def __init__(self, root) -> None:
        self.root = root
        self.model = CodecMapModel(name="", version="")

    def _text(self, node, what: str, maximum: int = CODEC_NAME_MAX) -> str:
        text, _, _ = require_text(node, what, maximum)
        return text

    def _node_pairs(self, node, what: str) -> dict:
        """Mapping pairs keyed by their text, refusing duplicate keys."""
        result = {}
        for key_node, value_node in map_pairs(node, what):
            key = getattr(key_node, "value", None)
            if not isinstance(key, str):
                raise Yaml2cError("%s key must be a string" % what,
                                  *_mark(key_node))
            if key in result:
                raise Yaml2cError("duplicate mapping key '%s'" % key,
                                  *_mark(key_node))
            result[key] = value_node
        return result

    _SIGNAL_KEYS = ("name", "start_bit", "bit_length", "type", "endianness",
                    "scale", "offset", "unit", "values", "min", "max",
                    "strict", "layout", "bit_layout")
    _MESSAGE_KEYS = ("id", "name", "dlc", "period_ms", "description",
                     "signals")

    def _check_keys(self, node, allowed, what: str) -> None:
        """Unknown fields are loader definition errors (fail closed)."""
        for key_node, _ in map_pairs(node, what):
            key_text, key_line, key_column = require_text(key_node,
                                                          "%s key" % what)
            if key_text not in allowed:
                raise Yaml2cError("unknown field '%s' in %s" % (key_text,
                                                                what),
                                  key_line, key_column)

    def _signal(self, node, message: MessageModel, seen_names: List[str],
                max_payload_bits: int) -> SignalModel:
        keys = self._node_pairs(node, "signal")
        self._check_keys(node, self._SIGNAL_KEYS, "signal")
        if "layout" in keys and "bit_layout" in keys:
            raise Yaml2cError("signal must not specify both 'layout' and "
                              "'bit_layout'", *_mark(node))
        name = self._text(keys.get("name"), "signal name")
        signal = SignalModel(
            name=name,
            start_bit=require_uint(keys.get("start_bit"),
                                   "signal '%s' start_bit" % name,
                                   0, max_payload_bits - 1),
            bit_length=require_uint(keys.get("bit_length"),
                                    "signal '%s' bit_length" % name, 1, 64),
            type_name=self._text(keys.get("type"), "signal type"),
            endianness=self._text(keys.get("endianness"), "signal endianness"))
        if name in seen_names:
            raise Yaml2cError("duplicate signal name '%s' in codec map '%s'"
                              % (name, self.model.name), *_mark(node))
        seen_names.append(name)
        if signal.type_name not in _SIGNAL_TYPES:
            raise Yaml2cError("signal type '%s' is not one of uint, int, "
                              "boolean, enum" % signal.type_name,
                              *_mark(keys.get("type")))
        if signal.endianness not in _ENDIANNESS:
            raise Yaml2cError("signal endianness '%s' is not one of little, big"
                              % signal.endianness, *_mark(keys.get("endianness")))
        if "scale" in keys:
            signal.scale = _require_number(keys["scale"],
                                           "signal '%s' scale" % name)
        if "offset" in keys:
            signal.offset = _require_number(keys["offset"],
                                            "signal '%s' offset" % name)
        if (signal.type_name in ("uint", "int")) and signal.scale == 0.0:
            raise Yaml2cError("signal '%s' has scale 0, which cannot be encoded"
                              % name, *_mark(node))
        if keys.get("unit") is not None:
            signal.unit = self._text(keys["unit"], "signal '%s' unit" % name,
                                     CODEC_UNIT_MAX)
        if "min" in keys:
            signal.min = _require_number(keys["min"],
                                         "signal '%s' min" % name)
            signal.has_min = True
        if "max" in keys:
            signal.max = _require_number(keys["max"],
                                         "signal '%s' max" % name)
            signal.has_max = True
        if keys.get("strict") is not None:
            signal.strict = require_bool(keys["strict"],
                                          "signal '%s' strict" % name)
        layout_node = keys.get("layout", keys.get("bit_layout"))
        if layout_node is not None:
            layout_text = self._text(layout_node, "signal layout")
            if layout_text not in _LAYOUTS:
                raise Yaml2cError("signal layout '%s' is not one of contiguous, "
                                  "sawtooth" % layout_text, *_mark(layout_node))
            signal.layout = layout_text

        # Value mappings: keys are non-negative integers fitting the signal.
        values_node = keys.get("values")
        if values_node is not None:
            for key_node, label_node in map_pairs(values_node,
                                                  "signal 'values'"):
                raw = require_uint(key_node, "signal 'values' key",
                                   0, 0xFFFFFFFFFFFFFFFF)
                if raw >= (1 << signal.bit_length):
                    raise Yaml2cError("signal 'values' key %d does not fit in "
                                      "%d bits" % (raw, signal.bit_length),
                                      *_mark(key_node))
                signal.values.append(ValueMappingModel(
                    raw=raw,
                    label=self._text(label_node,
                                     "signal '%s' value label" % name,
                                     CODEC_LABEL_MAX)))
        if signal.type_name == "enum" and not signal.values:
            raise Yaml2cError("enum signal '%s' requires a 'values' mapping"
                              % name, *_mark(node))
        if signal.type_name == "boolean" and signal.bit_length != 1:
            raise Yaml2cError("boolean signal '%s' must be exactly 1 bit wide"
                              % name, *_mark(node))

        # Bit-model sanity, with the loader's conditions and messages.
        if signal.layout == "sawtooth":
            idx = (signal.start_bit >> 3) * 8 + (7 - (signal.start_bit & 7))
            if idx + signal.bit_length > max_payload_bits:
                raise Yaml2cError("sawtooth signal '%s' exceeds the %d-bit "
                                  "payload" % (name, max_payload_bits),
                                  *_mark(node))
        elif signal.endianness == "little":
            if signal.start_bit + signal.bit_length > max_payload_bits:
                raise Yaml2cError("little-endian signal '%s' exceeds the %d-bit "
                                  "payload" % (name, max_payload_bits),
                                  *_mark(node))
        elif signal.start_bit < signal.bit_length - 1:
            raise Yaml2cError("big-endian signal '%s' extends below payload "
                              "bit 0" % name, *_mark(node))

        self._derive(signal, message.id)
        return signal

    @staticmethod
    def _derive(signal: SignalModel, message_id: int) -> None:
        """first_bit/last_bit/message_id, exactly as the loader computes them."""
        signal.message_id = message_id
        if signal.layout == "sawtooth":
            idx = (signal.start_bit >> 3) * 8 + (7 - (signal.start_bit & 7))
            positions = [((idx + k) >> 3) * 8 + (7 - ((idx + k) & 7))
                         for k in range(signal.bit_length)]
            signal.first_bit = min(positions)
            signal.last_bit = max(positions)
        elif signal.endianness == "little":
            signal.first_bit = signal.start_bit
            signal.last_bit = signal.start_bit + signal.bit_length - 1
        else:
            signal.first_bit = signal.start_bit - (signal.bit_length - 1)
            signal.last_bit = signal.start_bit

    def _message(self, node, seen_ids: List[int],
                 seen_names: List[str]) -> MessageModel:
        keys = self._node_pairs(node, "message")
        self._check_keys(node, self._MESSAGE_KEYS, "message")
        message_id = require_uint(keys.get("id"), "message id", 0, 536870911)
        name = self._text(keys.get("name"), "message name")
        if message_id in seen_ids:
            raise Yaml2cError("duplicate message id %d (message '%s')"
                              % (message_id, name), *_mark(keys.get("id")))
        if name in seen_names:
            raise Yaml2cError("duplicate message name '%s'" % name,
                              *_mark(keys.get("name")))
        seen_ids.append(message_id)
        seen_names.append(name)
        dlc = require_uint(keys.get("dlc"), "message '%s' dlc" % name, 0, 64)
        if self.model.can_fd:
            if dlc not in _FD_PAYLOAD_LENGTHS:
                raise Yaml2cError("message '%s' dlc %d is not a CAN FD payload "
                                  "length (0-8, 12, 16, 20, 24, 32, 48, 64)"
                                  % (name, dlc), *_mark(keys.get("dlc")))
        elif dlc > 8:
            raise Yaml2cError("message '%s' dlc %d exceeds the classic payload "
                              "(0-8)" % (name, dlc), *_mark(keys.get("dlc")))
        message = MessageModel(id=message_id, name=name, dlc=dlc)
        if keys.get("period_ms") is not None:
            message.period_ms = require_uint(keys["period_ms"],
                                             "message '%s' period_ms" % name,
                                             0, 0xFFFFFFFF)
        if keys.get("description") is not None:
            message.description = self._text(
                keys["description"], "message description",
                CODEC_DESCRIPTION_MAX)
        seen_signals: List[str] = []
        max_payload_bits = 512 if self.model.can_fd else 64
        for item in seq_items(keys.get("signals"), "message signals"):
            message.signals.append(self._signal(item, message, seen_signals,
                                                max_payload_bits))
        check_count(len(message.signals), "signal count of message '%s'" % name)
        return message

    def read(self) -> CodecMapModel:
        map_node = map_value(self.root, "codec_map")
        for key_node, _ in map_pairs(self.root, "the codec map file"):
            key, line, column = require_text(key_node, "codec map file key")
            if key not in ("schema_version", "codec_map"):
                raise Yaml2cError('unknown field "%s" in the codec map file'
                                  % key, line, column)
        for key_node, _ in map_pairs(map_node, "codec_map"):
            key, line, column = require_text(key_node, "codec_map key")
            if key not in ("name", "version", "description", "can_fd",
                           "messages"):
                raise Yaml2cError('unknown field "%s" in codec_map' % key,
                                  line, column)
        self.model.name = self._text(map_value(map_node, "name"),
                                     "codec map name")
        self.model.version = self._text(map_value(map_node, "version"),
                                        "codec map version", CODEC_VERSION_MAX)
        if map_value(map_node, "description") is not None:
            self.model.description = self._text(
                map_value(map_node, "description"), "codec map description",
                CODEC_DESCRIPTION_MAX)
        if map_value(map_node, "can_fd") is not None:
            self.model.can_fd = require_bool(map_value(map_node, "can_fd"),
                                              "codec map can_fd")
        seen_ids: List[int] = []
        seen_names: List[str] = []
        for item in seq_items(map_value(map_node, "messages"), "messages"):
            self.model.messages.append(self._message(item, seen_ids,
                                                     seen_names))
        check_count(len(self.model.messages), "message count")
        return self.model


class CodecMapWriter:
    """Emits the static const C definitions of a compiled codec map."""

    def __init__(self, model: CodecMapModel, prefix: str, source_name: str,
                 tool_version: str) -> None:
        self.model = model
        self.prefix = prefix
        self.source_name = source_name
        self.tool_version = tool_version
        # Artifact-kind tag: see FsmWriter for the collision rationale.
        self.strings = StringTable(prefix + "_map")
        self._array_counter = 0
        self.emit = Emitter()
        self.messages_name = ""

    def _array_name(self, what: str) -> str:
        name = "%s_%s_%03d" % (self.prefix, what, self._array_counter)
        self._array_counter += 1
        return name

    def _str(self, text: str) -> str:
        return self.strings.intern(text)

    def _str_or_null(self, text: Optional[str]) -> str:
        return self.strings.intern(text) if text is not None else "NULL"

    def _prepare(self) -> None:
        """Value mappings and signal arrays first, then the message array."""
        # Intern the top-level strings up front: the map initializer below
        # is rendered after the string table has already been emitted.
        self._str(self.model.name)
        self._str(self.model.version)
        if self.model.description is not None:
            self._str(self.model.description)
        for message in self.model.messages:
            for signal in message.signals:
                if not signal.values:
                    continue
                values_name = self._array_name("values")
                signal.values_name = values_name
                self.emit.comment("value mapping of signal '%s'" % signal.name)
                self.emit.open("static const cancestry_codec_value_mapping_t "
                               "%s[] = {" % values_name)
                for mapping in signal.values:
                    self.emit.line("{.raw = %s, .name = %s},"
                                   % (_c_uint64(mapping.raw),
                                      self._str(mapping.label)))
                self.emit.close("};")
                self.emit.blank()
            signals_name = self._array_name("signals")
            message.signals_name = signals_name
            self.emit.comment("signals of message '%s' (%d signal(s))"
                              % (message.name, len(message.signals)))
            self.emit.open("static const cancestry_codec_signal_t %s[] = {"
                           % signals_name)
            for signal in message.signals:
                self.emit.line("{.name = %s, .start_bit = %du, "
                               ".bit_length = %du,"
                               % (self._str(signal.name), signal.start_bit,
                                  signal.bit_length))
                self.emit.line(" .type = %s, .endianness = %s, .scale = %s, "
                               ".offset = %s,"
                               % (_SIGNAL_TYPES[signal.type_name],
                                  _ENDIANNESS[signal.endianness],
                                  c_double(signal.scale),
                                  c_double(signal.offset)))
                self.emit.line(" .unit = %s, .values = %s, .value_count = %du,"
                               % (self._str_or_null(signal.unit),
                                  signal.values_name or "NULL",
                                  len(signal.values)))
                self.emit.line(" .has_min = %s, .has_max = %s, .min = %s, "
                               ".max = %s, .strict = %s,"
                               % ("true" if signal.has_min else "false",
                                  "true" if signal.has_max else "false",
                                  c_double(signal.min), c_double(signal.max),
                                  "true" if signal.strict else "false"))
                self.emit.line(" .first_bit = %du, .last_bit = %du, "
                               ".message_id = %du, .layout = %s},"
                               % (signal.first_bit, signal.last_bit,
                                  signal.message_id, _LAYOUTS[signal.layout]))
            self.emit.close("};")
            self.emit.blank()
        self.messages_name = self._array_name("messages")
        self.emit.comment("messages (%d)" % len(self.model.messages))
        self.emit.open("static const cancestry_codec_message_t %s[] = {"
                       % self.messages_name)
        for message in self.model.messages:
            self.emit.line("{.id = %s, .name = %s, .dlc = %du, "
                           ".period_ms = %du,"
                           % (_c_uint64(message.id), self._str(message.name),
                              message.dlc, message.period_ms))
            self.emit.line(" .description = %s, .signals = %s, "
                           ".signal_count = %du},"
                           % (self._str_or_null(message.description),
                              message.signals_name, len(message.signals)))
        self.emit.close("};")
        self.emit.blank()

    def write(self, guard: Optional[str] = None) -> str:
        self._prepare()
        map_name = "%s_codec_map" % self.prefix
        if guard is None:
            guard = header_guard(map_name)
        head = Emitter()
        emit_prelude(head, guard, "codec map", "cancestry/codec/types.h",
                     self.source_name, self.tool_version)
        head.comment("interned strings (first-use order, deduplicated)")
        self.strings.emit(head)
        tail = Emitter()
        tail.comment("the codec map: register it with a codec namespace")
        tail.open("static const cancestry_codec_map_t %s = {" % map_name)
        tail.line(".name = %s," % self._str(self.model.name))
        tail.line(".version = %s," % self._str(self.model.version))
        tail.line(".description = %s,"
                  % self._str_or_null(self.model.description))
        tail.line(".messages = %s," % self.messages_name)
        tail.line(".message_count = %du," % len(self.model.messages))
        tail.line(".can_fd = %s" % ("true" if self.model.can_fd else "false"))
        tail.close("};")
        emit_epilogue(tail, guard)
        return head.text() + self.emit.text() + tail.text()


def generate_codec_header(composed_root, source_name: str, tool_version: str,
                          prefix: Optional[str] = None,
                          guard: Optional[str] = None) -> str:
    """Compile a composed codec-map YAML tree into C header text.

    The loader view is built first; see generate_fsm_header for why.
    """
    loader_view(composed_root)
    model = CodecMapReader(composed_root).read()
    if prefix is None:
        prefix = symbolize(source_name.replace("\\", "/").split("/")[-1]
                           .rsplit(".", 1)[0])
    return CodecMapWriter(model, prefix, source_name,
                          tool_version).write(guard)
