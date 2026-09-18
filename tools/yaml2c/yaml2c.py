#!/usr/bin/env python3
"""yaml2c - compile CANcestry v0.3.0 YAML into static C definitions.

Phase 8, issue #22. The tool consumes an FSM file (schemas/
fsm-0.3.0.schema.json) or a codec map (schemas/codec-map-0.3.0.schema.json)
and writes a self-contained C header of ``static const`` definitions that
initializes ``cancestry_fsm_set_t`` / ``cancestry_codec_map_t`` 1:1 with the
runtime types, eliminating the runtime YAML loader on the target
(SW-FR-TOOL-001..004).

Requirements traced: SW-FR-TOOL-001, SW-FR-TOOL-002, SW-FR-TOOL-003,
SW-FR-TOOL-004. Test ids: YAML2C-FSM-001, YAML2C-CODEC-001,
YAML2C-NOALLOC-001, YAML2C-SCHEMA-001.

Usage:
    python3 tools/yaml2c/yaml2c.py -o gateway_fsm.h examples/gateway_real/gateway_fsm.yaml
    python3 tools/yaml2c/yaml2c.py -o gateway_codec.h --kind codec gateway_codec.yaml
    python3 tools/yaml2c/yaml2c.py --stdout --set-symbol my_set fsm.yaml

The document kind defaults to auto-detection from the top-level keys
(``state_machines`` -> FSM, ``codec_map`` -> codec map) and is pinned by
``--kind``. Input is validated against the canonical schema before anything
is generated; ``--schema-dir`` points at the schema directory (default:
``schemas`` next to the repository root of this tool).

Exit codes: 0 success, 1 the document was refused (schema violation,
compile error), 2 usage error.
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional

try:  # The generator walks the composed node tree (marks, quoting style).
    import yaml
except ImportError as error:  # pragma: no cover - environment-dependent
    sys.exit("yaml2c requires PyYAML (`pip install pyyaml`): %s" % error)

if __package__ in (None, ""):  # pragma: no cover - script mode only
    _REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if _REPO_ROOT not in sys.path:
        sys.path.insert(0, _REPO_ROOT)
    from tools.yaml2c import __version__  # type: ignore  # noqa: E402
    from tools.yaml2c import codec_codegen, fsm_codegen  # type: ignore  # noqa: E402
    from tools.yaml2c.common import (Yaml2cError,  # type: ignore  # noqa: E402
                                     loader_view, symbolize)
    from tools.yaml2c.schema import (DEFAULT_SCHEMA_DIR,  # type: ignore  # noqa: E402
                                     validate_document)
else:
    from . import __version__
    from . import codec_codegen, fsm_codegen
    from .common import Yaml2cError, loader_view, symbolize
    from .schema import DEFAULT_SCHEMA_DIR, validate_document


def compose_document(path: str):
    """Compose the YAML node tree, keeping marks, styles and duplicate keys.

    Unlike ``safe_load``, the composed tree preserves what the C loaders see:
    original scalar text, quoting style and per-node source positions (the
    loaders store the action line in ``cancestry_fsm_action_t.source_line``).
    Duplicate mapping keys are refused like the hand-written loaders do.
    """
    with open(path, "r", encoding="utf-8") as handle:
        try:
            return yaml.compose(handle)
        except yaml.MarkedYAMLError as error:
            raise Yaml2cError("YAML syntax error: %s" % error.problem,
                              error.problem_mark.line + 1,
                              error.problem_mark.column + 1) from None
        except yaml.YAMLError as error:
            raise Yaml2cError("YAML syntax error: %s" % error) from None
        except UnicodeDecodeError as error:
            raise Yaml2cError("input is not valid UTF-8: %s" % error.reason,
                              1, 1) from None


def detect_kind(composed_root) -> str:
    """Auto-detect the document kind from the top-level keys."""
    is_mapping = (composed_root is not None and
                  hasattr(composed_root, "tag") and
                  str(composed_root.tag).endswith(":map"))
    if not is_mapping:
        raise Yaml2cError("document is empty or not a mapping")
    keys = {key.value for key, _ in composed_root.value
            if hasattr(key, "value")}
    if "state_machines" in keys:
        return "fsm"
    if "codec_map" in keys:
        return "codec"
    raise Yaml2cError("cannot detect the document kind: expected top-level "
                      "'state_machines' (FSM file) or 'codec_map' (codec map)")


def generate(composed_root, kind: str, source_name: str, prefix: str,
             guard: Optional[str], schema_dir: str, check_schema: bool,
             tool_version: str) -> str:
    """Validate (optional) and compile one document into C header text."""
    if check_schema:
        # The schema check runs on the "loader view" of the document: plain
        # data derived from the same composed tree the generator walks, so
        # quoting styles, YAML 1.1 booleans ("OFF", "yes") and the loader
        # number grammar all agree between schema check and code generation.
        validate_document(loader_view(composed_root), kind, schema_dir,
                          source_name)
    if kind == "fsm":
        return fsm_codegen.generate_fsm_header(composed_root, source_name,
                                               tool_version, prefix, guard)
    return codec_codegen.generate_codec_header(composed_root, source_name,
                                               tool_version, prefix, guard)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="yaml2c",
        description="Compile a CANcestry v0.3.0 FSM file or codec map into "
                    "static const C definitions (no runtime loader needed).")
    parser.add_argument("input", help="YAML file to compile")
    parser.add_argument("-o", "--output",
                        help="output C header path (default: <input>.h next "
                             "to the input; '-' writes to stdout)")
    parser.add_argument("--kind", choices=("fsm", "codec"),
                        help="document kind (default: auto-detect)")
    parser.add_argument("--set-symbol",
                        help="symbol prefix for the generated top-level "
                             "object (<prefix>_fsm_set / <prefix>_codec_map); "
                             "default: derived from the input file name")
    parser.add_argument("--guard",
                        help="include guard macro name (default: derived "
                             "from the output file name)")
    parser.add_argument("--schema-dir", default=DEFAULT_SCHEMA_DIR,
                        help="directory of the canonical JSON schemas "
                             "(default: %(default)s)")
    parser.add_argument("--no-schema-validate", action="store_true",
                        help="skip Draft 2020-12 schema validation "
                             "(not recommended; the generator still applies "
                             "its own loader-compatible checks)")
    parser.add_argument("--stdout", action="store_true",
                        help="write the generated C to stdout")
    parser.add_argument("--version", action="version",
                        version="yaml2c %s" % __version__)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    input_path = args.input
    if not os.path.isfile(input_path):
        print("yaml2c: error: no such file: %s" % input_path, file=sys.stderr)
        return 2

    stem = os.path.basename(input_path).rsplit(".", 1)[0]
    prefix = args.set_symbol
    if prefix is None:
        prefix = symbolize(stem)

    try:
        composed = compose_document(input_path)
        kind = args.kind or detect_kind(composed)
        header = generate(composed, kind, input_path, prefix, args.guard,
                          args.schema_dir, not args.no_schema_validate,
                          __version__)
    except Yaml2cError as error:
        print("yaml2c: %s: %s" % (input_path, error), file=sys.stderr)
        return 1
    except OSError as error:
        print("yaml2c: error: %s" % error, file=sys.stderr)
        return 2

    output = args.output or (os.path.splitext(input_path)[0] + ".h")
    if args.stdout or output == "-":
        sys.stdout.write(header)
    else:
        parent = os.path.dirname(output)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(output, "w", encoding="utf-8") as handle:
            handle.write(header)
        print("yaml2c: wrote %s (%s, symbol prefix '%s')"
              % (output, kind, prefix))
    return 0


if __name__ == "__main__":  # pragma: no cover  # pragma: no cover
    sys.exit(main())
