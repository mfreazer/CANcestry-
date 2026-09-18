#!/usr/bin/env python3
# Verify that every CANcestry JSON Schema is well-formed and logically valid.
# Test ids: SCHEMA-VALIDATION, PKG-VALIDATE-001, HAL-SCHEMA-001,
# SCHEMA-LOGIC-001 (Phase 8 logical validation).
# Reference: SW-FR-FSM-003, SW-FR-HAL-012, SW-FR-TOOL-008, agents.md "Schema is law".
"""Verify that every CANcestry JSON Schema is well-formed and logically valid.

The schemas in ``schemas/`` are the source of truth the loaders enforce in C
(SW-FR-FSM-003, agents.md "Schema is law"). Issue #13 requires
``schemas/fsm-0.3.0.schema.json`` to exist and be valid; issue #22 closes the
gap identified in issue #14 by upgrading the per-file check from "parses as
JSON and carries the right $schema/$id" to *full Draft 2020-12 logical
validation*: every schema document is validated against the Draft 2020-12
metaschema with the ``jsonschema`` library and its format checker, so a
schema with a logical violation - an invalid regex in ``pattern``, a
``required`` that is not an array of strings, a bad ``uniqueItemProbability``
style keyword value, non-finite boundaries - is rejected by CI instead of
sitting in the tree as unenforceable "law".

Per file, the check therefore runs:

1. JSON parsing (always).
2. Structural checks (always): the 2020-12 dialect declaration and an $id
   whose embedded version matches the file name.
3. Logical validation (when ``jsonschema`` is installed):
   ``Draft202012Validator.check_schema`` with the format checker. When the
   library is unavailable the check reports SKIP and the exit status stays
   green, so minimal environments keep working; CI installs jsonschema (see
   tests/unit/tools/requirements.txt) so the deep check runs there.

Usage:
    python3 ci/check_schemas_valid.py <schemas-dir>

Exit codes:
    0  every schema is valid (logical validation may have been skipped)
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

    problems.extend(check_schema_logically(schema))
    return problems


def check_schema_logically(schema):
    """Validate the schema against the Draft 2020-12 metaschema.

    Returns a list of problems; an empty list means the schema is logically
    valid (or that the deep check could not run, which is reported as a SKIP
    notice by the caller and does not fail the gate).
    """
    try:
        import jsonschema
    except ImportError:
        return []

    validator_cls = jsonschema.validators.validator_for(
        jsonschema.Draft202012Validator.META_SCHEMA)
    try:
        validator_cls.check_schema(
            schema, format_checker=validator_cls.FORMAT_CHECKER)
    except jsonschema.SchemaError as error:
        message = str(error).strip().splitlines()[0]
        return ["is not a valid Draft 2020-12 schema: %s" % message]
    return []


def jsonschema_available():
    """True when the logical validation depth is available."""
    try:
        import jsonschema  # noqa: F401
        return True
    except ImportError:
        return False


def main(argv):
    if len(argv) != 2:
        print("usage: check_schemas_valid.py <schemas-dir>")
        return 1
    root = argv[1]
    if not os.path.isdir(root):
        print("FAIL: %s is not a directory" % root)
        return 1

    deep = jsonschema_available()
    if not deep:
        print("SKIP: jsonschema not installed; logical (metaschema) "
              "validation is not run (SW-FR-TOOL-008)")

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
        elif deep:
            print("PASS: %s (parsed, structural, Draft 2020-12 metaschema)" %
                  name)
        else:
            print("PASS: %s (parsed, structural)" % name)
    if failures:
        print("%d schema(s) failed validation" % failures)
        return 1
    print("all schemas are valid")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main(sys.argv))
