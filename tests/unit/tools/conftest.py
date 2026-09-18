"""Shared fixtures for the CANcestry Python tool tests (issue #22).

The suites under this directory cover tools/yaml2c, tools/trace_viewer and
the Phase 8 upgrade of ci/check_schemas_valid.py with pytest. C-compilation
checks (the generated headers must compile and stay allocation-free) run
when a C compiler is available and are skipped otherwise, mirroring the
opendbc parity harness conventions (tests/integration/opendbc-parity).

Requirements traced: SW-FR-TOOL-001..010. Test ids: YAML2C-*, TRACE-VIEW-*,
SCHEMA-LOGIC-*, TOOLS-PYTEST-001.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]

# Repo-root imports for the tools under test ("tools" is a namespace package).
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

SCHEMA_DIR = REPO_ROOT / "schemas"
YAML2C_MAIN = REPO_ROOT / "tools" / "yaml2c" / "yaml2c.py"
TRACE_VIEWER_MAIN = REPO_ROOT / "tools" / "trace_viewer" / "trace_viewer.py"
CHECK_SCHEMAS = REPO_ROOT / "ci" / "check_schemas_valid.py"
CHECK_NO_ALLOC = REPO_ROOT / "ci" / "check_no_alloc.py"

CORE_EVENT_SRC = REPO_ROOT / "core" / "event" / "src"
CORE_CODEC_SRC = REPO_ROOT / "core" / "codec" / "src"
CORE_FSM_SRC = REPO_ROOT / "core" / "fsm" / "src"
INCLUDES = [
    REPO_ROOT / "core" / "event" / "include",
    REPO_ROOT / "core" / "codec" / "include",
    REPO_ROOT / "core" / "fsm" / "include",
]


@pytest.fixture(scope="session")
def compiler() -> str:
    """A usable C compiler, or skip (the C checks need one)."""
    cc = shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available to compile generated headers")
    return cc


def compile_c(compiler: str, source: Path, output: Path,
              include_dirs: list[Path]) -> subprocess.CompletedProcess:
    """Compile one C file with the repository's strict host flags."""
    command = [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
               "-Wpedantic", "-Wconversion",
               "-I%s" % include_dirs[0], "-I%s" % include_dirs[1],
               "-I%s" % include_dirs[2], "-I%s" % str(source.parent),
               "-c", str(source), "-o", str(output)]
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)


def run_yaml2c(*args: str, cwd: Path = REPO_ROOT
               ) -> subprocess.CompletedProcess:
    """Run the yaml2c CLI exactly like a user would."""
    command = [sys.executable, str(YAML2C_MAIN)]
    command.extend(args)
    return subprocess.run(command, cwd=str(cwd), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)


def run_trace_viewer(*args: str) -> subprocess.CompletedProcess:
    command = [sys.executable, str(TRACE_VIEWER_MAIN)]
    command.extend(args)
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)


def check_no_alloc(target: Path) -> subprocess.CompletedProcess:
    command = [sys.executable, str(CHECK_NO_ALLOC), str(target)]
    return subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)


def compose(tmp_path: Path, text: str):
    """Compose a YAML document from text using the generator's own loader."""
    import yaml

    from tools.yaml2c.yaml2c import compose_document

    path = tmp_path / "input.yaml"
    path.write_text(text, encoding="utf-8")
    return compose_document(str(path))


@pytest.fixture()
def workspace(tmp_path: Path) -> Path:
    """Per-test scratch directory with the canonical schemas linked in."""
    schemas = tmp_path / "schemas"
    schemas.mkdir()
    for name in os.listdir(SCHEMA_DIR):
        if name.endswith(".schema.json"):
            shutil.copy(SCHEMA_DIR / name, schemas / name)
    return tmp_path
