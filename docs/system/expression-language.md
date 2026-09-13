# CANcestry Expression Language Specification

Version: 0.2.1

## 1. Namespaces

Expressions shall use explicit namespaces:

- sig.SignalName
- var.VariableName
- evt.EventField

Bare identifiers are not permitted.

Examples:

    sig.VehicleSpeed * 0.621371
    var.wake_count + 1
    evt.timestamp_us

## 2. Grammar

    expr        := or_expr
    or_expr     := and_expr ("or" and_expr)*
    and_expr    := not_expr ("and" not_expr)*
    not_expr    := "not" not_expr | cmp_expr
    cmp_expr    := add_expr (("==" | "!=" | "<" | "<=" | ">" | ">=") add_expr)?
    add_expr    := mul_expr (("+" | "-") mul_expr)*
    mul_expr    := unary (("*" | "/" | "%") unary)*
    unary       := "-" unary | postfix
    postfix     := primary | func_call
    func_call   := IDENT "(" args? ")"
    primary     := NUMBER | BOOLEAN | namespaced_ident | "(" expr ")"

## 3. Operator Precedence

Highest to lowest:

1. parentheses
2. function call
3. unary minus
4. *
5. /
6. %
7. +
8. -
9. comparison
10. not
11. and
12. or

## 4. Types

The runtime shall support:

- integer: signed 64-bit
- float: IEEE 754 binary64
- boolean

## 5. Failure Rules

The following shall raise expression faults:

- integer overflow,
- division by zero,
- modulo by zero,
- NaN result,
- Infinity result,
- invalid type coercion,
- undefined identifier,
- unauthorized signal access.

## 6. Type Coercion

- Integer and float operations promote to float.
- Comparisons return boolean.
- Boolean operators require boolean operands.
- Implicit string conversion is not allowed.

## 7. Built-in Functions

Allowed functions:

- min(a, b)
- max(a, b)
- clamp(value, low, high)
- abs(x)
- round(x)

Rules:

- clamp requires low <= high.
- round returns integer.
- If result is outside integer range, expression fault.
