#!/usr/bin/env sh
# Frama-C/WP proof driver for the Phase 9 proof boundary.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
FRAMA_C_BIN=${FRAMA_C:-frama-c}
PROVERS=${FRAMA_PROVERS:-alt-ergo,qed}

if ! command -v "$FRAMA_C_BIN" >/dev/null 2>&1; then
    echo "error: Frama-C is required (set FRAMA_C=... or install it)" >&2
    exit 2
fi

# Frama-C accepts include paths through the C preprocessor options. Keep the
# source list explicit so a new module cannot accidentally enter this safety
# proof boundary without review.
exec "$FRAMA_C_BIN" \
    -std-c99 \
    -cpp-extra-args="-I$ROOT/core/event/include -I$ROOT/core/codec/include -I$ROOT/core/codec/src" \
    -wp \
    -wp-rte \
    -wp-split \
    -wp-timeout 60 \
    -wp-prover "$PROVERS" \
    -wp-fct cancestry_event_queue_push,cancestry_event_queue_pop,cancestry_codec_encode_signal,cancestry_codec_decode_signal,cancestry_codec_decode_frame \
    "$ROOT/core/event/src/queue.c" \
    "$ROOT/core/event/src/types.c" \
    "$ROOT/core/codec/src/encoder.c" \
    "$ROOT/core/codec/src/decoder.c"
