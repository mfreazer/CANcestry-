"""Shared fixtures for the opendbc parity harness.

The harness is host-side: it builds the CANcestry C decoder into a small CLI
(`codec_cli`), runs the `dbc2codec` compiler, and drives both against
comma.ai `opendbc`'s reference parser.

Requirements traced: SYS-FR-018, SYS-NF-008, QA-H02, QA-v0.2-R01.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]

CODEC_CLI_SRC = HERE / "codec_cli.c"
EVENT_SRC = REPO_ROOT / "core" / "event" / "src"
EVENT_INC = REPO_ROOT / "core" / "event" / "include"
CODEC_SRC = REPO_ROOT / "core" / "codec" / "src"
CODEC_INC = REPO_ROOT / "core" / "codec" / "include"

DBC2CODEC_DIR = REPO_ROOT / "tools" / "dbc2codec"
SCHEMA_PATH = REPO_ROOT / "schemas" / "codec-map-0.2.0.schema.json"


@pytest.fixture(scope="session")
def compiler() -> str:
    """Return a usable C compiler (gcc or clang), or skip the whole session."""
    cc = shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available to build the CANcestry codec CLI")
    return cc


@pytest.fixture(scope="session")
def codec_cli(tmp_path_factory: pytest.TempPathFactory, compiler: str) -> Path:
    """Build the CANcestry codec CLI once for the session."""
    out = tmp_path_factory.mktemp("codec-cli") / "codec_cli"
    srcs = [CODEC_CLI_SRC]
    srcs += sorted((EVENT_SRC).glob("*.c"))
    srcs += sorted((CODEC_SRC).glob("*.c"))
    cmd = [
        compiler, "-std=c99", "-O2", "-Wall", "-Wextra",
        "-I", str(EVENT_INC), "-I", str(CODEC_INC),
        *[str(s) for s in srcs], "-o", str(out),
    ]
    subprocess.run(cmd, check=True, cwd=REPO_ROOT)
    return out


@pytest.fixture(scope="session")
def dbc_paths():
    """Diverse vehicle DBC files from the pinned opendbc checkout.

    Selected to exercise different endianness, sign, scaling and value-mapping
    edge cases:

      tesla_model3_vehicle  all little-endian (Intel), enums/booleans, scaling
      hyundai_i30_2014      all little-endian, heavy scaling
      toyota_prius_2010_pt  all big-endian (Motorola), single- and multi-byte
    """
    try:
        import opendbc
    except ImportError as exc:  # pragma: no cover
        pytest.skip("opendbc not installed (see requirements.txt): %s" % exc)

    names = ["tesla_model3_vehicle", "hyundai_i30_2014", "toyota_prius_2010_pt"]
    paths = []
    for name in names:
        path = Path(opendbc.DBC_PATH) / (name + ".dbc")
        if not path.exists():
            pytest.skip("pinned opendbc checkout is missing %s.dbc" % name)
        paths.append((name, path))
    return paths


@pytest.fixture(scope="session")
def run_dbc2codec(tmp_path_factory: pytest.TempPathFactory):
    """Compile a DBC to a codec-map YAML, returning (yaml_path, warnings)."""
    def _run(dbc_path: Path, name: str, **extra):
        out = tmp_path_factory.mktemp("dbc2codec") / (name + ".yaml")
        argv = [
            sys.executable, str(DBC2CODEC_DIR / "dbc2codec.py"),
            str(dbc_path), "-o", str(out), "--name", name,
            "--source-url", "https://github.com/commaai/opendbc",
            "--commit", "pinned", "--license", "MIT (Copyright (c) comma.ai)",
            "--schema", str(SCHEMA_PATH),
        ]
        for k, v in extra.items():
            argv.append("--%s" % k.replace("_", "-"))
            argv.append(str(v))
        proc = subprocess.run(argv, capture_output=True, text=True, cwd=REPO_ROOT)
        assert proc.returncode == 0, "dbc2codec failed:\n%s" % proc.stderr
        return out, proc.stderr
    return _run
