#!/usr/bin/env python3
"""Verify that every CANcestry JSON Schema is well-formed and self-consistent.

The schemas in ``schemas/`` are the source of truth the loaders enforce in C
(SW-FR-FSM-003, SW-FR-CODEC, agents.md "Schema is law"). Issue #13 requires
``schemas/fsm-0.3.0.schema.json`` to exist and be valid. This check guards the
whole directory: every file must parse as JSON, declare the 2020-12 dialect,
and carry an ``$id`` whose embedded version matches the file name.

Usage:
    python3 ci/check_schemas_valid.py <schemas-dir>

Exit codes:
    0  every schema is valid
    1  at least one schema failed a check
"""

import json
import os
import re
import sys

DIALECT = "https://json-schema.org/draft/2020-12/schema"


def check_file(path):
    """Return a list of problems for one schema file (empty when clean)."""
    problems = []
    try:
        with open(path, "r", encoding="utf-8") as handle:
            schema = json.load(handle)
    except ValueError as error:
        return ["does not parse as JSON: %s" % error]
    except OSError as error:
        return ["cannot be read: %s" % error]

    if not isinstance(schema, dict):
        return ["top-level value is not an object"]

    if schema.get("$schema") != DIALECT:
        problems.append("$schema is not the 2020-12 dialect")

    schema_id = schema.get("$id", "")
    if not isinstance(schema_id, str) or not schema_id:
        problems.append("missing $id")
    else:
        # The version embedded in the $id must match the file name, so a file
        # is never mistaken for a different schema generation.
        name_version = re.match(r"^[a-z-]+-(\d+\.\d+\.\d+)\.schema\.json$",
                                os.path.basename(path))
        id_version = re.search(r"-(\d+\.\d+\.\d+)\.schema\.json$", schema_id)
        if name_version and (id_version is None or
                             id_version.group(1) != name_version.group(1)):
            problems.append("$id version %r does not match file name version %r" %
                            (id_version.group(1) if id_version else None,
                             name_version.group(1)))
    return problems


def main(argv):
    if len(argv) != 2:
        print("usage: check_schemas_valid.py <schemas-dir>")
        return 1
    root = argv[1]
    if not os.path.isdir(root):
        print("FAIL: %s is not a directory" % root)
        return 1

    failures = 0
    for name in sorted(os.listdir(root)):
        if not name.endswith(".schema.json"):
            continue
        path = os.path.join(root, name)
        problems = check_file(path)
        if problems:
            failures += 1
            print("FAIL: %s" % name)
            for problem in problems:
                print("      %s" % problem)
        else:
            print("PASS: %s" % name)
    if failures:
        print("%d schema(s) failed validation" % failures)
        return 1
    print("all schemas are valid")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
