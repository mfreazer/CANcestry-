"""FSM file -> static C definitions (issue #22, SW-FR-TOOL-001).

Consumes the composed YAML node tree of an FSM file validated against
schemas/fsm-0.3.0.schema.json and emits ``static const`` definitions that
initialize ``cancestry_fsm_set_t`` and everything it transitively points at,
1:1 with ``cancestry/fsm/types.h``.

The compile step mirrors what core/fsm/src/loader.c does at load time, so a
document the loader refuses is refused here too (fail closed):

  * names resolve to indexes (initial_index, target_index, machine_index),
  * unknown references (machine, state, timer, variable) are definition
    errors,
  * duplicate names within their scope are definition errors,
  * every guard, default and operand expression passes the grammar check of
    expression.c (ported in tools/yaml2c/expression.py),
  * name and text length limits match the C headers,
  * ``source_line`` of every action is the 1-based line in the YAML source.

Emission is bottom-up (strings, signal-value arrays, action arrays,
transition arrays, state arrays, machine arrays, instance arrays, the set) so
every pointer initializer refers to an already-defined ``static const``
object: no forward declarations, no runtime pointer fixup, zero dynamic
allocation (SW-FR-TOOL-003).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple

from . import expression
from .common import (Emitter, FSM_DESCRIPTION_MAX, FSM_EXPRESSION_MAX,
                     FSM_FAULT_CODE_MAX, FSM_LOG_MESSAGE_MAX, FSM_NAME_MAX,
                     StringTable, Yaml2cError, c_double, c_int, check_count,
                     emit_epilogue, emit_prelude, header_guard, loader_view,
                     map_pairs, map_value, operand_from_scalar, require_bool,
                     require_text, require_uint, seq_items, symbolize)

# ---------------------------------------------------------------------------
# Enum mappings (every entry mirrors an enum in the C headers).
# ---------------------------------------------------------------------------

_EVENT_TYPES = {
    "can_rx": "CANCESTRY_EVENT_TYPE_CAN_RX",
    "signal_changed": "CANCESTRY_EVENT_TYPE_SIGNAL_CHANGED",
    "timer_expired": "CANCESTRY_EVENT_TYPE_TIMER_EXPIRED",
    "state_entered": "CANCESTRY_EVENT_TYPE_STATE_ENTERED",
    "state_exited": "CANCESTRY_EVENT_TYPE_STATE_EXITED",
    "fault_raised": "CANCESTRY_EVENT_TYPE_FAULT_RAISED",
    "power_mode_changed": "CANCESTRY_EVENT_TYPE_POWER_MODE_CHANGED",
}

_VARIABLE_TYPES = {
    "boolean": "CANCESTRY_VALUE_KIND_BOOL",
    "integer": "CANCESTRY_VALUE_KIND_INT",
    "float": "CANCESTRY_VALUE_KIND_REAL",
}

_LOG_LEVELS = {
    "info": "CANCESTRY_FSM_LOG_INFO",
    "warning": "CANCESTRY_FSM_LOG_WARNING",
    "error": "CANCESTRY_FSM_LOG_ERROR",
}

_FAULT_SEVERITIES = {
    "warning": "CANCESTRY_FAULT_SEVERITY_WARNING",
    "error": "CANCESTRY_FAULT_SEVERITY_ERROR",
    "critical": "CANCESTRY_FAULT_SEVERITY_CRITICAL",
}

_ACTION_KINDS = {
    "send_message": "CANCESTRY_FSM_ACTION_SEND_MESSAGE",
    "set_signal": "CANCESTRY_FSM_ACTION_SET_SIGNAL",
    "set_variable": "CANCESTRY_FSM_ACTION_SET_VARIABLE",
    "start_timer": "CANCESTRY_FSM_ACTION_START_TIMER",
    "stop_timer": "CANCESTRY_FSM_ACTION_STOP_TIMER",
    "reset_timer": "CANCESTRY_FSM_ACTION_RESET_TIMER",
    "log": "CANCESTRY_FSM_ACTION_LOG",
    "raise_fault": "CANCESTRY_FSM_ACTION_RAISE_FAULT",
    "transition": "CANCESTRY_FSM_ACTION_TRANSITION",
}

_ACTION_FIELDS = {
    "send_message": ("interface", "message", "signals"),
    "set_signal": ("signal", "value"),
    "set_variable": ("variable", "value"),
    "start_timer": ("timer", "duration_ms", "repeat"),
    "stop_timer": ("timer",),
    "reset_timer": ("timer",),
    "log": ("level", "message"),
    "raise_fault": ("code", "severity"),
    "transition": ("target",),
}

_TRANSITION_FIELDS = ("event", "interface", "message", "signal", "timer",
                      "guard", "actions", "target")


# ---------------------------------------------------------------------------
# Model.
# ---------------------------------------------------------------------------

@dataclass
class OperandModel:
    kind: str  # "bool" | "int" | "real" | "expression"
    bool_value: bool = False
    int_value: int = 0
    real_value: float = 0.0
    expression: str = ""


@dataclass
class SignalValueModel:
    name: str
    operand: OperandModel


@dataclass
class ActionModel:
    kind: str
    source_line: int
    fields: dict = field(default_factory=dict)


@dataclass
class TransitionModel:
    event: str
    interface: Optional[str] = None
    message: Optional[str] = None
    signal: Optional[str] = None
    timer: Optional[str] = None
    guard: Optional[str] = None
    actions: List[ActionModel] = field(default_factory=list)
    target: str = ""
    target_index: int = 0
    target_mark: Optional[tuple] = None
    actions_name: Optional[str] = None  # emit-time: symbol of the action array


@dataclass
class StateModel:
    name: str
    entry: List[ActionModel] = field(default_factory=list)
    exit: List[ActionModel] = field(default_factory=list)
    transitions: List[TransitionModel] = field(default_factory=list)
    entry_name: Optional[str] = None
    exit_name: Optional[str] = None
    transitions_name: Optional[str] = None


@dataclass
class VariableDefModel:
    name: str
    type_name: str
    has_default: bool = False
    default: Optional[OperandModel] = None


@dataclass
class TimerDefModel:
    name: str
    duration_ms: int = 0
    repeat: bool = False
    auto_start: bool = False


@dataclass
class MachineModel:
    name: str
    description: Optional[str] = None
    initial: str = ""
    initial_index: int = 0
    variables: List[VariableDefModel] = field(default_factory=list)
    timers: List[TimerDefModel] = field(default_factory=list)
    states: List[StateModel] = field(default_factory=list)
    variables_name: Optional[str] = None
    timers_name: Optional[str] = None
    states_name: Optional[str] = None


@dataclass
class BindingModel:
    alias: str
    physical: str


@dataclass
class OverrideModel:
    name: str
    operand: OperandModel


@dataclass
class CanRxModel:
    interface: str
    can_id: Optional[int]
    message: Optional[str]


@dataclass
class SubscriptionsModel:
    can_rx: List[CanRxModel] = field(default_factory=list)
    has_can_rx: bool = False
    signals: List[str] = field(default_factory=list)
    has_signals: bool = False
    timers: bool = False
    has_timers: bool = False
    faults: bool = False
    has_faults: bool = False
    power_mode: bool = False
    has_power_mode: bool = False


@dataclass
class InstanceModel:
    id: str
    machine: str
    machine_index: int = 0
    enabled: bool = False
    bindings: List[BindingModel] = field(default_factory=list)
    variables: List[OverrideModel] = field(default_factory=list)
    subscriptions: SubscriptionsModel = field(default_factory=SubscriptionsModel)
    bindings_name: Optional[str] = None
    variables_name: Optional[str] = None
    signals_name: Optional[str] = None
    can_rx_name: Optional[str] = None


@dataclass
class SetModel:
    machines: List[MachineModel] = field(default_factory=list)
    instances: List[InstanceModel] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Model building (the compile step).
# ---------------------------------------------------------------------------

def _mark(node):
    if node is not None and hasattr(node, "start_mark") and node.start_mark:
        return node.start_mark.line + 1, node.start_mark.column + 1
    return None, None


class FsmReader:
    """Walks the composed YAML tree and validates it like the C loader."""

    def __init__(self, root) -> None:
        self.root = root
        self.set_model = SetModel()

    # -- helpers ------------------------------------------------------------

    def _operand(self, node, what: str) -> OperandModel:
        if node is None or not hasattr(node, "style") or \
                isinstance(getattr(node, "value", None), list):
            raise Yaml2cError("%s must be a number, a boolean or a string" % what,
                              *_mark(node))
        operand = operand_from_scalar(node.value, node.style, *_mark(node), what)
        if operand.kind == "expression":
            expression.validate_expression(operand.expression, what, *_mark(node),
                                           FSM_EXPRESSION_MAX)
        return OperandModel(kind=operand.kind, bool_value=operand.bool_value,
                            int_value=operand.int_value,
                            real_value=operand.real_value,
                            expression=operand.expression)

    def _text(self, node, what: str, maximum: int = FSM_NAME_MAX) -> str:
        text, _, _ = require_text(node, what, maximum)
        return text

    @staticmethod
    def _name_index(names: List[str], name: str) -> int:
        try:
            return names.index(name)
        except ValueError:
            return -1

    def _unique_name(self, node, seen: List[str], noun: str, scope: str) -> str:
        name = self._text(node, noun)
        if name in seen:
            raise Yaml2cError('duplicate %s "%s" (%s names must be unique within %s)'
                              % (noun, name, noun, scope), *_mark(node))
        seen.append(name)
        return name

    def _allowed_keys(self, node, allowed: Tuple[str, ...], what: str) -> None:
        for key_node, _ in map_pairs(node, what):
            key, line, column = require_text(key_node, "%s key" % what)
            if key not in allowed:
                raise Yaml2cError('unknown field "%s" in %s' % (key, what),
                                  line, column)

    # -- actions ------------------------------------------------------------

    def _action(self, node, machine: MachineModel) -> ActionModel:
        """One fsm_action: the schema's oneOf key selects the body."""
        pairs = map_pairs(node, "action")
        if len(pairs) != 1:
            raise Yaml2cError("an action must be a mapping with exactly one key",
                              *_mark(node))
        key, line, column = require_text(pairs[0][0], "action name")
        if key not in _ACTION_KINDS:
            raise Yaml2cError('unknown action "%s"' % key, line, column)
        body = pairs[0][1]
        self._allowed_keys(body, _ACTION_FIELDS[key], "action %s" % key)
        action = ActionModel(kind=key, source_line=line)
        bmark = _mark(body)

        if key == "send_message":
            values_node = map_value(body, "signals")
            values = map_pairs(values_node, "send_message.signals")
            if not values:
                raise Yaml2cError("send_message.signals must set at least one "
                                  "signal value", *_mark(values_node))
            signal_values = []
            for name_node, value_node in values:
                name = self._text(name_node, "send_message signal name")
                signal_values.append(SignalValueModel(
                    name=name,
                    operand=self._operand(value_node,
                                          'send_message signal "%s" value' % name)))
            action.fields = {
                "interface": self._text(map_value(body, "interface"),
                                        "send_message.interface"),
                "message": self._text(map_value(body, "message"),
                                      "send_message.message"),
                "values": signal_values,
            }
        elif key in ("set_signal", "set_variable"):
            field = "signal" if key == "set_signal" else "variable"
            name = self._text(map_value(body, field), "%s.%s" % (key, field))
            operand = self._operand(map_value(body, "value"), "%s.value" % key)
            if key == "set_variable" and \
                    self._name_index([v.name for v in machine.variables], name) < 0:
                raise Yaml2cError('set_variable names "%s", which the state '
                                  "machine does not declare" % name, *bmark)
            action.fields = {"name": name, "operand": operand}
        elif key == "start_timer":
            timer = self._text(map_value(body, "timer"), "start_timer.timer")
            if self._name_index([t.name for t in machine.timers], timer) < 0:
                raise Yaml2cError('start_timer names "%s", which the state '
                                  "machine does not declare" % timer, *bmark)
            fields = {"timer": timer}
            duration_node = map_value(body, "duration_ms")
            if duration_node is not None:
                fields["duration_ms"] = require_uint(
                    duration_node, "start_timer.duration_ms", 1, 0xFFFFFFFF)
                fields["has_duration_ms"] = True
            repeat_node = map_value(body, "repeat")
            if repeat_node is not None:
                fields["repeat"] = require_bool(repeat_node, "start_timer.repeat")
                fields["has_repeat"] = True
            action.fields = fields
        elif key in ("stop_timer", "reset_timer"):
            timer = self._text(map_value(body, "timer"), "timer")
            if self._name_index([t.name for t in machine.timers], timer) < 0:
                raise Yaml2cError('a timer action names "%s", which the state '
                                  "machine does not declare" % timer, *bmark)
            action.fields = {"timer": timer}
        elif key == "log":
            level_node = map_value(body, "level")
            level_text = self._text(level_node, "log.level")
            if level_text not in _LOG_LEVELS:
                raise Yaml2cError('log.level "%s" is not one of info, warning, '
                                  "error" % level_text, *_mark(level_node))
            action.fields = {
                "level": _LOG_LEVELS[level_text],
                "message": self._text(map_value(body, "message"), "log.message",
                                      FSM_LOG_MESSAGE_MAX),
            }
        elif key == "raise_fault":
            severity_node = map_value(body, "severity")
            severity_text = self._text(severity_node, "raise_fault.severity")
            if severity_text not in _FAULT_SEVERITIES:
                raise Yaml2cError('raise_fault.severity "%s" is not one of '
                                  "warning, error, critical" % severity_text,
                                  *_mark(severity_node))
            action.fields = {
                "code": self._text(map_value(body, "code"), "raise_fault.code",
                                   FSM_FAULT_CODE_MAX),
                "severity": _FAULT_SEVERITIES[severity_text],
            }
        else:  # transition action
            target = self._text(map_value(body, "target"), "transition.target")
            action.fields = {"target": target, "target_mark": bmark}
        return action

    def _action_list(self, node, machine: MachineModel,
                     what: str) -> List[ActionModel]:
        items = seq_items(node, what) if node is not None else []
        return [self._action(item, machine) for item in items]

    # -- transitions and states ---------------------------------------------

    def _transition(self, node, machine: MachineModel) -> TransitionModel:
        self._allowed_keys(node, _TRANSITION_FIELDS, "transition")
        event_node = map_value(node, "event")
        event_text = self._text(event_node, "transition.event")
        if event_text not in _EVENT_TYPES:
            raise Yaml2cError('transition.event "%s" is not a known event type'
                              % event_text, *_mark(event_node))
        transition = TransitionModel(event=event_text)

        def optional_filter(key: str, what: str) -> Optional[str]:
            value = map_value(node, key)
            return self._text(value, what) if value is not None else None

        transition.interface = optional_filter("interface", "transition.interface")
        transition.message = optional_filter("message", "transition.message")
        transition.signal = optional_filter("signal", "transition.signal")
        transition.timer = optional_filter("timer", "transition.timer")
        guard_node = map_value(node, "guard")
        if guard_node is not None:
            text = self._text(guard_node, "guard", FSM_EXPRESSION_MAX)
            expression.validate_expression(text, "guard", *_mark(guard_node),
                                           FSM_EXPRESSION_MAX)
            transition.guard = text
        transition.actions = self._action_list(map_value(node, "actions"),
                                               machine, "transition.actions")
        target_node = map_value(node, "target")
        transition.target = self._text(target_node, "transition.target")
        transition.target_mark = _mark(target_node)
        return transition

    def _state(self, node, machine: MachineModel, seen: List[str]) -> StateModel:
        self._allowed_keys(node, self._STATE_KEYS, "state")
        name = self._unique_name(map_value(node, "name"), seen, "state name",
                                 "a state machine")
        state = StateModel(name=name)
        state.entry = self._action_list(map_value(node, "entry"), machine,
                                        "state.entry")
        state.exit = self._action_list(map_value(node, "exit"), machine,
                                       "state.exit")
        transitions_node = map_value(node, "transitions")
        if transitions_node is not None:
            for item in seq_items(transitions_node, "state.transitions"):
                state.transitions.append(self._transition(item, machine))
        return state

    _MACHINE_KEYS = ("name", "description", "initial", "variables", "timers",
                     "states")
    _STATE_KEYS = ("name", "entry", "exit", "transitions")

    def _machine(self, node, seen_machines: List[str]) -> MachineModel:
        self._allowed_keys(node, self._MACHINE_KEYS, "state machine")
        name = self._unique_name(map_value(node, "name"), seen_machines,
                                 "machine name", "the file")
        machine = MachineModel(name=name)
        description_node = map_value(node, "description")
        if description_node is not None:
            machine.description = self._text(description_node,
                                             "machine description",
                                             FSM_DESCRIPTION_MAX)
        machine.initial = self._text(map_value(node, "initial"), "machine.initial")

        seen_variables: List[str] = []
        variables_node = map_value(node, "variables")
        if variables_node is not None:
            for item in seq_items(variables_node, "machine.variables"):
                variable = VariableDefModel(
                    name=self._unique_name(map_value(item, "name"), seen_variables,
                                           "variable name", "a state machine"),
                    type_name=self._text(map_value(item, "type"),
                                         "variable type"))
                if variable.type_name not in _VARIABLE_TYPES:
                    raise Yaml2cError('variable type "%s" is not one of boolean, '
                                      "integer, float" % variable.type_name,
                                      *_mark(map_value(item, "type")))
                default_node = map_value(item, "default")
                if default_node is not None:
                    variable.default = self._operand(
                        default_node,
                        'variable "%s" default' % variable.name)
                    variable.has_default = True
                machine.variables.append(variable)

        seen_timers: List[str] = []
        timers_node = map_value(node, "timers")
        if timers_node is not None:
            for item in seq_items(timers_node, "machine.timers"):
                machine.timers.append(TimerDefModel(
                    name=self._unique_name(map_value(item, "name"), seen_timers,
                                           "timer name", "a state machine"),
                    duration_ms=require_uint(map_value(item, "duration_ms"),
                                             "timer.duration_ms", 1, 0xFFFFFFFF),
                    repeat=require_bool(map_value(item, "repeat"),
                                        "timer.repeat"),
                    auto_start=require_bool(map_value(item, "auto_start"),
                                            "timer.auto_start")))

        seen_states: List[str] = []
        states_node = map_value(node, "states")
        for item in seq_items(states_node, "machine.states"):
            machine.states.append(self._state(item, machine, seen_states))

        initial_index = self._name_index([s.name for s in machine.states],
                                         machine.initial)
        if initial_index < 0:
            raise Yaml2cError('unknown initial state "%s"' % machine.initial,
                              *_mark(map_value(node, "initial")))
        machine.initial_index = initial_index
        return machine

    # -- instances ----------------------------------------------------------

    def _subscriptions(self, node) -> SubscriptionsModel:
        self._allowed_keys(node, ("can_rx", "signals", "timers", "faults",
                                  "power_mode"), "subscriptions")
        subs = SubscriptionsModel()
        can_rx_node = map_value(node, "can_rx")
        if can_rx_node is not None:
            subs.has_can_rx = True
            for item in seq_items(can_rx_node, "subscriptions.can_rx"):
                self._allowed_keys(item, ("interface", "id", "message"),
                                   "can_rx subscription")
                can_id_node = map_value(item, "id")
                message_node = map_value(item, "message")
                subs.can_rx.append(CanRxModel(
                    interface=self._text(map_value(item, "interface"),
                                         "can_rx.interface"),
                    can_id=require_uint(can_id_node, "can_rx.id", 0, 536870911)
                    if can_id_node is not None else None,
                    message=self._text(message_node, "can_rx.message")
                    if message_node is not None else None))
        signals_node = map_value(node, "signals")
        if signals_node is not None:
            subs.has_signals = True
            for item in seq_items(signals_node, "subscriptions.signals"):
                subs.signals.append(self._text(item, "subscription signal name"))
        timers_node = map_value(node, "timers")
        if timers_node is not None:
            subs.timers = require_bool(timers_node, "subscriptions.timers")
            subs.has_timers = True
        faults_node = map_value(node, "faults")
        if faults_node is not None:
            subs.faults = require_bool(faults_node, "subscriptions.faults")
            subs.has_faults = True
        power_node = map_value(node, "power_mode")
        if power_node is not None:
            subs.power_mode = require_bool(power_node,
                                           "subscriptions.power_mode")
            subs.has_power_mode = True
        return subs

    def _instance(self, node, seen_ids: List[str]) -> InstanceModel:
        self._allowed_keys(node, ("id", "machine", "enabled", "bindings",
                                  "variables", "subscriptions"), "instance")
        instance = InstanceModel(
            id=self._unique_name(map_value(node, "id"), seen_ids, "instance id",
                                 "the file"),
            machine=self._text(map_value(node, "machine"), "instance.machine"),
            enabled=require_bool(map_value(node, "enabled"), "instance.enabled"))
        machine = next((m for m in self.set_model.machines
                        if m.name == instance.machine), None)
        if machine is None:
            raise Yaml2cError('unknown machine "%s"' % instance.machine,
                              *_mark(map_value(node, "machine")))
        instance.machine_index = self.set_model.machines.index(machine)

        bindings_node = map_value(node, "bindings")
        if bindings_node is not None:
            pairs = map_pairs(bindings_node, "instance.bindings")
            if not pairs:
                raise Yaml2cError("instance.bindings must be a non-empty mapping",
                                  *_mark(bindings_node))
            for key_node, value_node in pairs:
                instance.bindings.append(BindingModel(
                    alias=self._text(key_node, "binding alias"),
                    physical=self._text(value_node,
                                        "binding physical interface")))

        variables_node = map_value(node, "variables")
        if variables_node is not None:
            pairs = map_pairs(variables_node, "instance.variables")
            if not pairs:
                raise Yaml2cError("instance.variables must be a non-empty "
                                  "mapping", *_mark(variables_node))
            for key_node, value_node in pairs:
                name = self._text(key_node, "variable name")
                if self._name_index([v.name for v in machine.variables],
                                    name) < 0:
                    raise Yaml2cError('instance "%s" initialises variable "%s", '
                                      'which machine "%s" does not declare'
                                      % (instance.id, name, machine.name),
                                      *_mark(key_node))
                instance.variables.append(OverrideModel(
                    name=name,
                    operand=self._operand(
                        value_node,
                        'instance variable initializer for "%s"' % name)))

        subscriptions_node = map_value(node, "subscriptions")
        if subscriptions_node is not None:
            instance.subscriptions = self._subscriptions(subscriptions_node)
        return instance

    # -- deferred reference resolution (the loader's compile pass) -----------

    def _resolve_targets(self) -> None:
        """Resolve transition targets to indexes now that all states exist.

        The C loader allows forward references (a transition may target a
        state declared later in the same machine); this pass mirrors that
        instead of resolving eagerly during parsing.
        """
        for machine in self.set_model.machines:
            state_names = [s.name for s in machine.states]
            for state in machine.states:
                for transition in state.transitions:
                    index = self._name_index(state_names, transition.target)
                    if index < 0:
                        raise Yaml2cError(
                            'unknown transition target "%s"' % transition.target,
                            *(transition.target_mark or (None, None)))
                    transition.target_index = index
                for action in (list(state.entry) + list(state.exit)
                               + [a for t in state.transitions
                                  for a in t.actions]):
                    if action.kind != "transition":
                        continue
                    index = self._name_index(state_names,
                                             action.fields["target"])
                    if index < 0:
                        raise Yaml2cError(
                            'a transition action targets "%s", which the '
                            "state machine does not define"
                            % action.fields["target"],
                            *(action.fields.get("target_mark") or (None, None)))
                    action.fields["target_index"] = index

    # -- entry point --------------------------------------------------------

    def read(self) -> SetModel:
        self._allowed_keys(self.root, ("schema_version", "state_machines",
                                       "instances"), "the FSM file")
        seen_machines: List[str] = []
        for item in seq_items(map_value(self.root, "state_machines"),
                              "state_machines"):
            self.set_model.machines.append(self._machine(item, seen_machines))
        self._resolve_targets()
        seen_ids: List[str] = []
        for item in seq_items(map_value(self.root, "instances"), "instances"):
            self.set_model.instances.append(self._instance(item, seen_ids))
        check_count(len(self.set_model.machines), "machine count")
        check_count(len(self.set_model.instances), "instance count")
        return self.set_model


# ---------------------------------------------------------------------------
# C emission (two passes: assign array symbols bottom-up, then render).
# ---------------------------------------------------------------------------

class FsmWriter:
    """Emits the static const C definitions of a compiled FSM set.

    Arrays are created in dependency order, innermost first, so every
    initializer in the output refers to an object defined above it.
    """

    def __init__(self, model: SetModel, prefix: str, source_name: str,
                 tool_version: str, guard: Optional[str] = None) -> None:
        self.model = model
        self.prefix = prefix
        self.source_name = source_name
        self.tool_version = tool_version
        self.guard = guard
        # The string table symbols carry the artifact kind so the FSM and
        # codec headers of one package can be included in one translation
        # unit without collisions.
        self.strings = StringTable(prefix + "_fsm")
        self._array_counter = 0
        self.emit = Emitter()

    def _array_name(self, what: str) -> str:
        name = "%s_%s_%03d" % (self.prefix, what, self._array_counter)
        self._array_counter += 1
        return name

    def _str(self, text: str) -> str:
        return self.strings.intern(text)

    def _str_or_null(self, text: Optional[str]) -> str:
        return self.strings.intern(text) if text is not None else "NULL"

    # -- leaf rendering -------------------------------------------------------

    def _render_operand(self, operand: OperandModel) -> str:
        """One-line cancestry_fsm_operand_t initializer."""
        is_expression = "true" if operand.kind == "expression" else "false"
        kinds = {"bool": "BOOL", "int": "INT", "real": "REAL",
                 "expression": "UNSET"}
        if operand.kind == "bool":
            value = ".boolean = %s" % ("true" if operand.bool_value else "false")
        elif operand.kind == "int":
            value = ".integer = %s" % c_int(operand.int_value, "int64_t")
        elif operand.kind == "real":
            value = ".real = %s" % c_double(operand.real_value)
        else:
            value = ".unsigned_integer = 0u"
        expression_str = self._str(operand.expression) \
            if operand.kind == "expression" else "NULL"
        return ("{.is_expression = %s, .literal = {.kind = "
                "CANCESTRY_VALUE_KIND_%s, .value = {%s}}, .expression = %s}"
                % (is_expression, kinds[operand.kind], value, expression_str))

    def _emit_action_arrays(self, actions: List[ActionModel],
                            what: str) -> Optional[str]:
        """Emit the signal-value arrays and the action array of one list."""
        if not actions:
            return None
        # Signal-value arrays first: the action array references them.
        for action in actions:
            if action.kind != "send_message":
                continue
            values = action.fields["values"]
            if not values:  # pragma: no cover - defensive: signals >= 1
                continue
            values_name = self._array_name("sigvals")
            action.fields["values_name"] = values_name
            self.emit.comment("signal values of a send_message action")
            self.emit.open("static const cancestry_fsm_signal_value_t "
                           "%s[] = {" % values_name)
            for value in values:
                self.emit.line("{.name = %s, .operand = %s},"
                               % (self._str(value.name),
                                  self._render_operand(value.operand)))
            self.emit.close("};")
            self.emit.blank()
        name = self._array_name(what)
        self.emit.comment("%s (%d action(s))" % (what, len(actions)))
        self.emit.open("static const cancestry_fsm_action_t %s[] = {" % name)
        for action in actions:
            self.emit.line(self._render_action(action) + ",")
        self.emit.close("};")
        self.emit.blank()
        return name

    def _render_action(self, action: ActionModel) -> str:
        head = "{.kind = %s, .source_line = %du, .as = {" % (
            _ACTION_KINDS[action.kind], action.source_line)
        if action.kind == "send_message":
            values = action.fields["values"]
            body = (".send_message = {.interface = %s, .message = %s, "
                    ".values = %s, .value_count = %du}"
                    % (self._str(action.fields["interface"]),
                       self._str(action.fields["message"]),
                       action.fields.get("values_name", "NULL"), len(values)))
        elif action.kind in ("set_signal", "set_variable"):
            body = (".%s = {.%s = %s, .operand = %s}"
                    % (action.kind,
                       "signal" if action.kind == "set_signal" else "variable",
                       self._str(action.fields["name"]),
                       self._render_operand(action.fields["operand"])))
        elif action.kind == "start_timer":
            body = (".start_timer = {.timer = %s, .duration_ms = %du, "
                    ".has_duration_ms = %s, .repeat = %s, .has_repeat = %s}"
                    % (self._str(action.fields["timer"]),
                       action.fields.get("duration_ms", 0),
                       "true" if action.fields.get("has_duration_ms") else "false",
                       "true" if action.fields.get("repeat") else "false",
                       "true" if action.fields.get("has_repeat") else "false"))
        elif action.kind in ("stop_timer", "reset_timer"):
            # Both kinds share the union's `timer` member (types.h).
            body = ".timer = {.timer = %s}" % self._str(action.fields["timer"])
        elif action.kind == "log":
            body = (".log = {.level = %s, .message = %s}"
                    % (action.fields["level"],
                       self._str(action.fields["message"])))
        elif action.kind == "raise_fault":
            body = (".raise_fault = {.code = %s, .severity = %s}"
                    % (self._str(action.fields["code"]),
                       action.fields["severity"]))
        else:  # transition action
            body = (".transition = {.target = %s, .target_index = %du}"
                    % (self._str(action.fields["target"]),
                       action.fields["target_index"]))
        return "%s%s}}" % (head, body)

    # -- preparation pass: assign symbols in dependency order ------------------

    def _prepare(self) -> None:
        for machine in self.model.machines:
            # Innermost first: action arrays of every state and transition.
            for state in machine.states:
                for transition in state.transitions:
                    transition.actions_name = self._emit_action_arrays(
                        transition.actions, "actions")
            for state in machine.states:
                state.entry_name = self._emit_action_arrays(state.entry, "entry")
                state.exit_name = self._emit_action_arrays(state.exit, "exit")
                state.transitions_name = self._emit_transition_array(
                    state.transitions)
            machine.variables_name = self._emit_variable_defs(machine)
            machine.timers_name = self._emit_timer_defs(machine)
            machine.states_name = self._emit_state_array(machine)
        for instance in self.model.instances:
            instance.bindings_name = self._emit_bindings(instance)
            instance.variables_name = self._emit_overrides(instance)
            instance.signals_name = self._emit_signal_allowlist(instance)
            instance.can_rx_name = self._emit_can_rx(instance)
        # The machine and instance arrays come after everything they point at.
        self.machines_name = self._emit_machine_array()
        self.instances_name = self._emit_instance_array()

    def _emit_transition_array(self,
                               transitions: List[TransitionModel]) -> Optional[str]:
        if not transitions:
            return None
        name = self._array_name("trans")
        self.emit.comment("transitions (%d)" % len(transitions))
        self.emit.open("static const cancestry_fsm_transition_t %s[] = {" % name)
        for transition in transitions:
            self.emit.line("{.event = %s, .interface = %s, .message = %s, "
                           ".signal = %s, .timer = %s, .guard = %s,"
                           % (_EVENT_TYPES[transition.event],
                              self._str_or_null(transition.interface),
                              self._str_or_null(transition.message),
                              self._str_or_null(transition.signal),
                              self._str_or_null(transition.timer),
                              self._str_or_null(transition.guard)))
            self.emit.line(" .actions = %s, .action_count = %du, .target = %s, "
                           ".target_index = %du},"
                           % (transition.actions_name or "NULL",
                              len(transition.actions),
                              self._str(transition.target),
                              transition.target_index))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_variable_defs(self, machine: MachineModel) -> Optional[str]:
        if not machine.variables:
            return None
        name = self._array_name("vardefs")
        self.emit.comment('variables of machine "%s"' % machine.name)
        self.emit.open("static const cancestry_fsm_variable_def_t %s[] = {"
                       % name)
        for variable in machine.variables:
            if variable.has_default:
                default = self._render_operand(variable.default)
                has_default = "true"
            else:
                default = ("{.is_expression = false, .literal = "
                           "{.kind = CANCESTRY_VALUE_KIND_UNSET, "
                           ".value = {.unsigned_integer = 0u}}, "
                           ".expression = NULL}")
                has_default = "false"
            self.emit.line("{.name = %s, .type = %s, .default_value = %s, "
                           ".has_default = %s},"
                           % (self._str(variable.name),
                              _VARIABLE_TYPES[variable.type_name], default,
                              has_default))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_timer_defs(self, machine: MachineModel) -> Optional[str]:
        if not machine.timers:
            return None
        name = self._array_name("timerdefs")
        self.emit.comment('timers of machine "%s"' % machine.name)
        self.emit.open("static const cancestry_fsm_timer_def_t %s[] = {"
                       % name)
        for timer in machine.timers:
            self.emit.line("{.name = %s, .duration_ms = %du, .repeat = %s, "
                           ".auto_start = %s},"
                           % (self._str(timer.name), timer.duration_ms,
                              "true" if timer.repeat else "false",
                              "true" if timer.auto_start else "false"))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_state_array(self, machine: MachineModel) -> str:
        name = self._array_name("states")
        self.emit.comment('states of machine "%s" (%d state(s))'
                          % (machine.name, len(machine.states)))
        self.emit.open("static const cancestry_fsm_state_t %s[] = {" % name)
        for state in machine.states:
            self.emit.line("{.name = %s, .entry = %s, .entry_count = %du,"
                           % (self._str(state.name), state.entry_name or "NULL",
                              len(state.entry)))
            self.emit.line(" .exit = %s, .exit_count = %du, .transitions = %s, "
                           ".transition_count = %du},"
                           % (state.exit_name or "NULL", len(state.exit),
                              state.transitions_name or "NULL",
                              len(state.transitions)))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_bindings(self, instance: InstanceModel) -> Optional[str]:
        if not instance.bindings:
            return None
        name = self._array_name("bindings")
        self.emit.comment("interface bindings of instance \"%s\"" % instance.id)
        self.emit.open("static const cancestry_fsm_binding_t %s[] = {" % name)
        for binding in instance.bindings:
            self.emit.line("{.alias = %s, .physical = %s},"
                           % (self._str(binding.alias),
                              self._str(binding.physical)))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_overrides(self, instance: InstanceModel) -> Optional[str]:
        if not instance.variables:
            return None
        name = self._array_name("overrides")
        self.emit.comment("variable initializers of instance \"%s\"" % instance.id)
        self.emit.open("static const cancestry_fsm_variable_override_t %s[] = {"
                       % name)
        for override in instance.variables:
            self.emit.line("{.name = %s, .operand = %s},"
                           % (self._str(override.name),
                              self._render_operand(override.operand)))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_signal_allowlist(self, instance: InstanceModel) -> Optional[str]:
        subs = instance.subscriptions
        if not subs.signals:
            return None
        name = self._array_name("sigsubs")
        self.emit.comment("signal_changed allowlist of instance \"%s\""
                          % instance.id)
        self.emit.open("static const char *const %s[] = {" % name)
        for signal in subs.signals:
            self.emit.line("%s," % self._str(signal))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_can_rx(self, instance: InstanceModel) -> Optional[str]:
        subs = instance.subscriptions
        if not subs.can_rx:
            return None
        name = self._array_name("canrx")
        self.emit.comment("can_rx subscriptions of instance \"%s\"" % instance.id)
        self.emit.open("static const cancestry_fsm_can_rx_subscription_t "
                       "%s[] = {" % name)
        for sub in subs.can_rx:
            self.emit.line("{.interface = %s, .can_id = %du, .has_can_id = %s, "
                           ".message = %s},"
                           % (self._str(sub.interface), sub.can_id or 0,
                              "true" if sub.can_id is not None else "false",
                              self._str_or_null(sub.message)))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_machine_array(self) -> str:
        name = self._array_name("machines")
        self.emit.comment("state machines (%d)" % len(self.model.machines))
        self.emit.open("static const cancestry_fsm_machine_t %s[] = {" % name)
        for machine in self.model.machines:
            self.emit.line("{.name = %s, .description = %s, .initial = %s,"
                           % (self._str(machine.name),
                              self._str_or_null(machine.description),
                              self._str(machine.initial)))
            self.emit.line(" .initial_index = %du, .variables = %s, "
                           ".variable_count = %du, .timers = %s, "
                           ".timer_count = %du,"
                           % (machine.initial_index,
                              machine.variables_name or "NULL",
                              len(machine.variables),
                              machine.timers_name or "NULL",
                              len(machine.timers)))
            self.emit.line(" .states = %s, .state_count = %du},"
                           % (machine.states_name, len(machine.states)))
        self.emit.close("};")
        self.emit.blank()
        return name

    def _emit_instance_array(self) -> str:
        name = self._array_name("instances")
        self.emit.comment("instances (%d)" % len(self.model.instances))
        self.emit.open("static const cancestry_fsm_instance_def_t %s[] = {"
                       % name)
        for instance in self.model.instances:
            subs = instance.subscriptions
            self.emit.line("{.id = %s, .machine = %s, .machine_index = %du, "
                           ".enabled = %s,"
                           % (self._str(instance.id),
                              self._str(instance.machine),
                              instance.machine_index,
                              "true" if instance.enabled else "false"))
            self.emit.line(" .bindings = %s, .binding_count = %du, "
                           ".variables = %s, .variable_count = %du,"
                           % (instance.bindings_name or "NULL",
                              len(instance.bindings),
                              instance.variables_name or "NULL",
                              len(instance.variables)))
            self.emit.line(" .subscriptions = {.can_rx = %s, .can_rx_count = %du, "
                           ".has_can_rx = %s,"
                           % (instance.can_rx_name or "NULL",
                              len(subs.can_rx),
                              "true" if subs.has_can_rx else "false"))
            self.emit.line("  .signals = %s, .signal_count = %du, "
                           ".has_signals = %s,"
                           % (instance.signals_name or "NULL",
                              len(subs.signals),
                              "true" if subs.has_signals else "false"))
            self.emit.line("  .timers = %s, .has_timers = %s, .faults = %s, "
                           ".has_faults = %s, .power_mode = %s, "
                           ".has_power_mode = %s}},"
                           % ("true" if subs.timers else "false",
                              "true" if subs.has_timers else "false",
                              "true" if subs.faults else "false",
                              "true" if subs.has_faults else "false",
                              "true" if subs.power_mode else "false",
                              "true" if subs.has_power_mode else "false"))
        self.emit.close("};")
        self.emit.blank()
        return name

    def write(self) -> str:
        """Render the complete header: prelude, strings, arrays, the set.

        ``_prepare()`` rendered the arrays into a scratch emitter (they may
        intern new strings); the final text lays the interned strings above
        them so every C initializer only references objects defined earlier.
        """
        self._prepare()
        set_name = "%s_fsm_set" % self.prefix
        guard = self.guard or header_guard(set_name)
        head = Emitter()
        emit_prelude(head, guard, "FSM file", "cancestry/fsm/types.h",
                     self.source_name, self.tool_version)
        head.comment("interned strings (first-use order, deduplicated)")
        self.strings.emit(head)
        tail = Emitter()
        tail.comment("the FSM set: pass its address as engine_config.sets")
        tail.open("static const cancestry_fsm_set_t %s = {" % set_name)
        tail.line(".machines = %s," % self.machines_name)
        tail.line(".machine_count = %du," % len(self.model.machines))
        tail.line(".instances = %s," % self.instances_name)
        tail.line(".instance_count = %du," % len(self.model.instances))
        tail.close("};")
        emit_epilogue(tail, guard)
        return head.text() + self.emit.text() + tail.text()


def generate_fsm_header(composed_root, source_name: str, tool_version: str,
                        prefix: Optional[str] = None,
                        guard: Optional[str] = None) -> str:
    """Compile a composed FSM YAML tree and return the C header text.

    The loader view is built first: it refuses duplicate mapping keys and
    non-scalar keys for the whole document, exactly like the C loaders,
    independent of whether the schema layer ran.
    """
    loader_view(composed_root)
    model = FsmReader(composed_root).read()
    if prefix is None:
        prefix = symbolize(source_name.replace("\\", "/").split("/")[-1]
                           .rsplit(".", 1)[0])
    return FsmWriter(model, prefix, source_name, tool_version,
                     guard).write()
