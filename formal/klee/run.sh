#!/usr/bin/env sh
# KLEE proof driver for SW-FR-FSM-037 / SW-FR-FSM-046.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
KLEE_BIN=${KLEE:-klee}
CLANG_BIN=${CLANG:-clang}
OUT=${KLEE_OUT:-"$ROOT/build/klee"}
BITCODE_DIR="$OUT/bitcode"

if ! command -v "$KLEE_BIN" >/dev/null 2>&1; then
    echo "error: KLEE is required (set KLEE=... or install it)" >&2
    exit 2
fi
if ! command -v "$CLANG_BIN" >/dev/null 2>&1; then
    echo "error: clang is required to produce LLVM bitcode" >&2
    exit 2
fi
if ! command -v llvm-link >/dev/null 2>&1; then
    echo "error: llvm-link is required to link LLVM bitcode" >&2
    exit 2
fi

rm -rf "$BITCODE_DIR"
mkdir -p "$BITCODE_DIR"

COMMON_INCLUDES="-I$ROOT/core/event/include -I$ROOT/core/fsm/include -I$ROOT/core/fsm/src"
CFLAGS="-std=c99 -g -O0 -fno-builtin $COMMON_INCLUDES"

# expression.c is self-contained apart from the public event/FSM types and
# libc routines modelled by KLEE. Keeping it as one module makes the proof
# boundary explicit: loader and engine allocation paths are not in scope.
# shellcheck disable=SC2086
"$CLANG_BIN" $CFLAGS -emit-llvm -c "$ROOT/core/fsm/src/expression.c" \
    -o "$BITCODE_DIR/expression.bc"
# shellcheck disable=SC2086
"$CLANG_BIN" $CFLAGS -emit-llvm -c "$ROOT/formal/klee/expression_harness.c" \
    -o "$BITCODE_DIR/expression_harness.bc"
llvm-link "$BITCODE_DIR/expression.bc" "$BITCODE_DIR/expression_harness.bc" \
    -o "$BITCODE_DIR/expression_bundle.bc"

"$KLEE_BIN" --output-dir="$OUT/expression" \
    --exit-on-error-type=Assert --exit-on-error-type=Exec \
    "$BITCODE_DIR/expression_bundle.bc"

# The comparator has no dynamic dependencies, but linking types.c keeps the
# harness on exactly the production implementation rather than a test copy.
"$CLANG_BIN" -std=c99 -g -O0 -fno-builtin \
    -I"$ROOT/core/event/include" -emit-llvm -c "$ROOT/core/event/src/types.c" \
    -o "$BITCODE_DIR/event_types.bc"
"$CLANG_BIN" -std=c99 -g -O0 -fno-builtin \
    -I"$ROOT/core/event/include" -emit-llvm -c "$ROOT/formal/klee/event_order_harness.c" \
    -o "$BITCODE_DIR/event_order_harness.bc"
llvm-link "$BITCODE_DIR/event_types.bc" "$BITCODE_DIR/event_order_harness.bc" \
    -o "$BITCODE_DIR/event_order_bundle.bc"

"$KLEE_BIN" --output-dir="$OUT/event-order" \
    --exit-on-error-type=Assert --exit-on-error-type=Exec \
    "$BITCODE_DIR/event_order_bundle.bc"

echo "KLEE PASS: expression bounds, arithmetic fault handling, and event ordering"
