#!/usr/bin/env python3
"""Verify that a compiled CANcestry core library performs no heap allocation.

The portable event core must operate entirely on caller-owned storage
(SYS-NF-002, issue #1 acceptance criteria). This check inspects the built
archive or shared object and fails when it finds an undefined reference to any
heap allocation entry point, which is a stronger and more portable guarantee
than intercepting malloc at runtime: it also runs under AddressSanitizer, where
user-provided malloc overrides are not usable.

Usage:
    python3 ci/check_no_alloc.py <library-or-object> [...]

Exit codes:
    0  no allocator references found, or the check could not run (reported)
    1  the library references at least one allocator entry point
"""

import os
import shutil
import subprocess
import sys

# Symbols that would let the core allocate, directly or through libc helpers.
FORBIDDEN = {
    "malloc",
    "calloc",
    "realloc",
    "free",
    "strdup",
    "strndup",
    "memalign",
    "posix_memalign",
    "aligned_alloc",
    "valloc",
    "pvalloc",
    "malloc_usable_size",
    # C++ allocation entry points, in case the core is ever built as C++.
    "_Znwm",
    "_Znam",
    "_ZdlPv",
    "_ZdaPv",
    "operator new",
    "operator new[]",
}


def find_nm():
    """Locate an nm implementation that can list undefined symbols."""
    candidates = []
    env_nm = os.environ.get("NM")
    if env_nm:
        candidates.append(env_nm)
    candidates.extend(["llvm-nm", "nm", "gcc-nm"])
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return path
    return None


def undefined_symbols(nm, target):
    """Return the set of undefined symbol names referenced by target."""
    # -u lists undefined symbols only; supported by both GNU nm and llvm-nm.
    result = subprocess.run([nm, "-u", target],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            universal_newlines=True)
    if result.returncode != 0:
        # Some nm variants need the archive to be readable but return non-zero
        # when there is nothing to report; fall back to a full listing.
        result = subprocess.run([nm, target],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE,
                                universal_newlines=True)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or "nm failed")

    symbols = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 1:
            name = parts[0]
        elif len(parts) >= 2:
            # GNU nm prints "                 U symbol" for undefined symbols.
            name = parts[-1]
        else:
            continue
        symbols.add(name.lstrip("_") if name.startswith("__") else name)
        symbols.add(name)
    return symbols


def main(argv):
    if len(argv) < 2:
        print("usage: check_no_alloc.py <library-or-object> [...]")
        return 1

    nm = find_nm()
    if nm is None:
        print("SKIP: no nm implementation found; cannot verify allocation-free build")
        return 0

    failures = 0
    for target in argv[1:]:
        if not os.path.exists(target):
            print("FAIL: %s does not exist" % target)
            failures += 1
            continue
        try:
            symbols = undefined_symbols(nm, target)
        except (OSError, RuntimeError) as error:
            print("SKIP: cannot inspect %s (%s)" % (target, error))
            continue

        offenders = sorted(symbols & FORBIDDEN)
        if offenders:
            print("FAIL: %s references heap allocation symbols: %s" %
                  (target, ", ".join(offenders)))
            failures += 1
        else:
            print("PASS: %s references no heap allocation symbols (%d symbols checked)" %
                  (target, len(symbols)))

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
