"""Draft 2020-12 validation of yaml2c input documents (issue #22).

Implements: SW-FR-TOOL-004 ("schema is law", agents.md section 3). The
generator refuses to emit C for a document that does not validate against
the corresponding JSON Schema, because the generated header replaces the
loader that would otherwise enforce the schema on the target.

The check uses the ``jsonschema`` library with the Draft 2020-12 validator
and format checking, the same depth ci/check_schemas_valid.py applies to the
schema documents themselves. ``FORMAT_CHECKER`` resolves the ``format``
keywords the metaschema declares (date, uri, regex, ...).
"""

from __future__ import annotations

import json
import os
from typing import Optional

from .common import Yaml2cError

#: Default location of the canonical schemas in the repository.
DEFAULT_SCHEMA_DIR = os.path.join("schemas")

_SCHEMA_FILES = {
    "fsm": "fsm-0.3.0.schema.json",
    "codec": "codec-map-0.3.0.schema.json",
}


def schema_path(kind: str, schema_dir: Optional[str] = None) -> str:
    """Return the schema file path for a document kind ("fsm" or "codec")."""
    if kind not in _SCHEMA_FILES:
        raise Yaml2cError("unknown document kind '%s'" % kind)
    directory = schema_dir or DEFAULT_SCHEMA_DIR
    path = os.path.join(directory, _SCHEMA_FILES[kind])
    if not os.path.isfile(path):
        raise Yaml2cError("schema file not found: %s" % path)
    return path


def load_schema(kind: str, schema_dir: Optional[str] = None) -> dict:
    """Load and parse a canonical schema document."""
    with open(schema_path(kind, schema_dir), "r", encoding="utf-8") as handle:
        return json.load(handle)


def validate_document(document, kind: str, schema_dir: Optional[str] = None,
                      source_name: str = "<input>") -> None:
    """Validate a parsed YAML document, raising Yaml2cError on violation.

    The best available error (the deepest one by JSON path) is reported with
    the document name; the input is refused as a whole either way.
    """
    try:
        import jsonschema
    except ImportError as error:  # pragma: no cover - environment-dependent
        raise Yaml2cError(
            "schema validation requires the jsonschema package "
            "(pip install jsonschema): %s" % error) from None
    schema = load_schema(kind, schema_dir)
    validator_cls = jsonschema.validators.validator_for(schema)
    validator = validator_cls(schema,
                              format_checker=validator_cls.FORMAT_CHECKER)
    errors = sorted(validator.iter_errors(document),
                    key=lambda error: list(error.absolute_path))
    if not errors:
        return
    error = errors[0]
    location = "$"
    for part in error.absolute_path:
        location += "[%r]" % (part,) if isinstance(part, str) else "[%d]" % part
    raise Yaml2cError("%s violates %s at %s: %s"
                      % (source_name, os.path.basename(schema_path(kind,
                                                                   schema_dir)),
                         location, error.message))
