#!/usr/bin/env sh
# Strict cppcheck/MISRA C:2012 driver for core/ and platform/.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CPPCHECK_BIN=${CPPCHECK:-cppcheck}
REPORT=${MISRA_REPORT:-"$ROOT/build/misra/cppcheck-report.txt"}
ADDON=${CPPCHECK_MISRA_ADDON:-}

if ! command -v "$CPPCHECK_BIN" >/dev/null 2>&1; then
    echo "error: cppcheck is required (set CPPCHECK=... or install it)" >&2
    exit 2
fi

if [ -z "$ADDON" ]; then
    for candidate in \
        /usr/share/cppcheck/addons/misra.py \
        /usr/local/share/cppcheck/addons/misra.py \
        "$(dirname "$(command -v "$CPPCHECK_BIN")")/../share/cppcheck/addons/misra.py"; do
        if [ -f "$candidate" ]; then
            ADDON=$candidate
            break
        fi
    done
fi

if [ -z "$ADDON" ] || [ ! -f "$ADDON" ]; then
    echo "error: cppcheck's MISRA C:2012 addon was not found" >&2
    echo "       set CPPCHECK_MISRA_ADDON to the path of misra.py" >&2
    exit 2
fi

mkdir -p "$(dirname "$REPORT")"

# Do not suppress MISRA findings here. The only environmental suppression is
# missingIncludeSystem: system headers are supplied by the target toolchain and
# are outside this repository's safety boundary. Every project finding must be
# fixed or have an entry in docs/safety/MISRA_Deviations.md.
if "$CPPCHECK_BIN" \
    --addon="$ADDON" \
    --enable=all \
    --inconclusive \
    --std=c99 \
    --language=c \
    --force \
    --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    -I"$ROOT/core/event/include" \
    -I"$ROOT/core/codec/include" \
    -I"$ROOT/core/recipe/include" \
    -I"$ROOT/core/fsm/include" \
    -I"$ROOT/core/hal/include" \
    -I"$ROOT/platform" \
    "$ROOT/core" "$ROOT/platform" >"$REPORT" 2>&1; then
    status=0
else
    status=$?
fi
cat "$REPORT"
exit "$status"
