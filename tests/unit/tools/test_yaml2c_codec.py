"""yaml2c codec-map generator tests (Phase 8, issue #22).

Requirements traced: SW-FR-TOOL-002, SW-FR-TOOL-003, SW-FR-TOOL-004.
Test ids: YAML2C-CODEC-001 (compilable header, derived fields 1:1 with the
C loader), YAML2C-CODEC-REFUSE-001..006.

The derived-field assertions (first_bit/last_bit, sawtooth walk) mirror
core/codec/src/loader.c; the compile-parity C check additionally compares a
generated map against cancestry_codec_map_load() output at runtime.
"""

from __future__ import annotations

import textwrap
import subprocess
import sys

import pytest

from tools.yaml2c import codec_codegen
from tools.yaml2c.common import Yaml2cError

from conftest import (CORE_CODEC_SRC, CORE_EVENT_SRC, INCLUDES, REPO_ROOT,
                      check_no_alloc, compile_c, compose, run_yaml2c)

CLASSIC = """
schema_version: "0.3.0"
codec_map:
  name: lab
  version: 1.2.3
  messages:
    - id: 0x100
      name: M
      dlc: 8
      signals:
        - name: little_u16
          start_bit: 0
          bit_length: 16
          type: uint
          endianness: little
          scale: 0.5
          offset: -10.0
          unit: "m/s"
          min: -10.0
          max: 100.0
          strict: false
        - name: big_s12
          start_bit: 23
          bit_length: 12
          type: int
          endianness: big
        - name: saw
          start_bit: 7
          bit_length: 12
          type: uint
          endianness: big
          layout: sawtooth
        - name: flag
          start_bit: 40
          bit_length: 1
          type: boolean
          endianness: little
        - name: mode
          start_bit: 41
          bit_length: 2
          type: enum
          endianness: little
          values:
            0: "OFF"
            1: "ON"
            2: "AUTO"
"""


def generate(text: str, tmp_path, prefix: str = "lab") -> str:
    return codec_codegen.generate_codec_header(compose(tmp_path, text),
                                               "map.yaml", "test", prefix)


def test_codec_header_compiles_with_derived_fields(tmp_path, workspace,
                                                   compiler):
    header = generate(CLASSIC, workspace)
    (workspace / "lab.h").write_text(header, encoding="utf-8")
    (workspace / "use.c").write_text(textwrap.dedent("""
        #include "lab.h"
        #include <string.h>
        int use(void) {
            const cancestry_codec_signal_t *sigs =
                lab_codec_map.messages[0].signals;
            /* Derived fields mirror the loader: little [0,15], big MSB
             * first [12,23], sawtooth min/max walk, per-message id. */
            return (lab_codec_map.message_count == 1u &&
                    strcmp(sigs[0].name, "little_u16") == 0 &&
                    sigs[0].first_bit == 0u && sigs[0].last_bit == 15u &&
                    sigs[0].message_id == 0x100u &&
                    sigs[1].first_bit == 12u && sigs[1].last_bit == 23u &&
                    sigs[2].layout == CANCESTRY_CODEC_LAYOUT_SAWTOOTH &&
                    sigs[3].bit_length == 1u &&
                    sigs[4].value_count == 3u &&
                    lab_codec_map.can_fd == false &&
                    strcmp(lab_codec_map.version, "1.2.3") == 0) ? 0 : 1;
        }
    """), encoding="utf-8")
    result = compile_c(compiler, workspace / "use.c", workspace / "use.o",
                       INCLUDES)
    assert result.returncode == 0, result.stderr


def test_codec_header_is_allocation_free(tmp_path, workspace, compiler):
    """YAML2C-NOALLOC-001 for the codec artifact."""
    (workspace / "lab.h").write_text(generate(CLASSIC, workspace),
                                     encoding="utf-8")
    (workspace / "use.c").write_text(
        '#include "lab.h"\nint use(void) { return lab_codec_map.message_count; }\n',
        encoding="utf-8")
    result = compile_c(compiler, workspace / "use.c", workspace / "use.o",
                       INCLUDES)
    assert result.returncode == 0, result.stderr
    gate = check_no_alloc(workspace / "use.o")
    assert gate.returncode == 0, gate.stdout + gate.stderr


def test_codec_runtime_parity_with_loader(tmp_path, workspace, compiler):
    """YAML2C-CODEC-001: the generated map is field-identical to the map
    the C loader builds from the same YAML (decode uses it unchanged)."""
    map_yaml = workspace / "map.yaml"
    map_yaml.write_text(CLASSIC, encoding="utf-8")
    header = codec_codegen.generate_codec_header(
        compose(workspace, CLASSIC), str(map_yaml), "test", "par")
    (workspace / "par.h").write_text(header, encoding="utf-8")

    harness = workspace / "parity.c"
    harness.write_text(textwrap.dedent("""
        #include "par.h"
        #include "cancestry/codec/loader.h"
        #include <stddef.h>
        #include <stdio.h>
        #include <string.h>

        extern const char map_yaml_text[];
        extern const size_t map_yaml_length;

        static int fail = 0;
        #define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\\n", \\
            __FILE__, __LINE__, #cond); fail = 1; } } while (0)

        static void compare_signal(const cancestry_codec_signal_t *a,
                                   const cancestry_codec_signal_t *b)
        {
            CHECK(strcmp(a->name, b->name) == 0);
            CHECK(a->start_bit == b->start_bit);
            CHECK(a->bit_length == b->bit_length);
            CHECK(a->type == b->type);
            CHECK(a->endianness == b->endianness);
            CHECK(a->scale == b->scale);
            CHECK(a->offset == b->offset);
            CHECK(a->value_count == b->value_count);
            CHECK(a->has_min == b->has_min && a->has_max == b->has_max);
            CHECK(a->min == b->min && a->max == b->max);
            CHECK(a->strict == b->strict);
            CHECK(a->first_bit == b->first_bit);
            CHECK(a->last_bit == b->last_bit);
            CHECK(a->message_id == b->message_id);
            CHECK(a->layout == b->layout);
            CHECK(((a->unit == NULL) == (b->unit == NULL)) &&
                  (a->unit == NULL || strcmp(a->unit, b->unit) == 0));
            for (uint16_t v = 0; v < a->value_count; ++v) {
                CHECK(a->values[v].raw == b->values[v].raw);
                CHECK(strcmp(a->values[v].name, b->values[v].name) == 0);
            }
        }

        int main(void)
        {
            cancestry_codec_load_error_t error;
            cancestry_codec_map_t *loaded = cancestry_codec_map_load(
                (const char *)map_yaml_text, (size_t)map_yaml_length, &error);
            CHECK(loaded != NULL);
            if (loaded == NULL) { return 1; }
            CHECK(strcmp(loaded->name, par_codec_map.name) == 0);
            CHECK(strcmp(loaded->version, par_codec_map.version) == 0);
            CHECK(loaded->can_fd == par_codec_map.can_fd);
            CHECK(loaded->message_count == par_codec_map.message_count);
            for (uint16_t m = 0; m < loaded->message_count; ++m) {
                const cancestry_codec_message_t *a = &loaded->messages[m];
                const cancestry_codec_message_t *b =
                    &par_codec_map.messages[m];
                CHECK(a->id == b->id);
                CHECK(a->dlc == b->dlc);
                CHECK(a->period_ms == b->period_ms);
                CHECK(a->signal_count == b->signal_count);
                CHECK(strcmp(a->name, b->name) == 0);
                CHECK(((a->description == NULL) == (b->description == NULL)) &&
                      (a->description == NULL ||
                       strcmp(a->description, b->description) == 0));
                for (uint16_t s = 0; s < a->signal_count; ++s) {
                    compare_signal(&a->signals[s], &b->signals[s]);
                }
            }
            cancestry_codec_map_free(loaded);
            printf("%s\\n", fail ? "PARITY FAIL" : "PARITY OK");
            return fail;
        }
    """), encoding="utf-8")
    # Expose the YAML text to the harness.
    (workspace / "map_yaml.c").write_text(
        '#include <stddef.h>\n'
        'const char map_yaml_text[] =\n' +
        "".join('    "%s\\n"\n' % line.replace("\\", "\\\\").replace('"', '\\"')
                for line in CLASSIC.strip().splitlines()) +
        ';\nconst size_t map_yaml_length = sizeof(map_yaml_text) - 1;\n',
        encoding="utf-8")
    objects = []
    for source in (harness, workspace / "map_yaml.c"):
        target = source.with_suffix(".o")
        result = compile_c(compiler, source, target, INCLUDES)
        assert result.returncode == 0, result.stderr
        objects.append(target)
    for module in sorted(CORE_CODEC_SRC.glob("*.c")):
        if module.name in ("loader.c",):
            target = workspace / ("ccl_" + module.name + ".o")
            result = compile_c(compiler, module, target, INCLUDES)
            assert result.returncode == 0, result.stderr
            objects.append(target)
    for module in sorted(CORE_EVENT_SRC.glob("*.c")):
        target = workspace / ("cce_" + module.name + ".o")
        result = compile_c(compiler, module, target, INCLUDES)
        assert result.returncode == 0, result.stderr
        objects.append(target)
    binary = workspace / "parity"
    link = subprocess.run(
        [compiler, "-std=c99"] + [str(o) for o in objects] +
        ["-o", str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True)
    assert link.returncode == 0, link.stderr
    run = subprocess.run([str(binary)], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, universal_newlines=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "PARITY OK" in run.stdout


def test_can_fd_map(tmp_path, workspace):
    header = generate("""
schema_version: "0.3.0"
codec_map:
  name: fdmap
  version: 1.0.0
  can_fd: true
  messages:
    - id: 0x1F0
      name: Radar
      dlc: 64
      signals:
        - name: Tail
          start_bit: 504
          bit_length: 8
          type: uint
          endianness: little
""", workspace)
    assert ".can_fd = true" in header
    assert ".dlc = 64u" in header
    assert ".first_bit = 504u, .last_bit = 511u" in header


def test_refusals_mirror_the_loader(tmp_path, workspace):
    def refuse(text, fragment, prefix="lab"):
        with pytest.raises(Yaml2cError) as info:
            generate(text, tmp_path, prefix)
        assert fragment in str(info.value), str(info.value)

    good_signal = """        - name: s
          start_bit: 0
          bit_length: 8
          type: uint
          endianness: little
"""
    # duplicate message id
    refuse(CLASSIC + """    - id: 0x100
      name: M2
      dlc: 8
      signals:
        - name: x
          start_bit: 0
          bit_length: 1
          type: boolean
          endianness: little
""", "duplicate message id 256")
    # duplicate signal name
    refuse(CLASSIC.replace('name: big_s12', 'name: little_u16'),
           "duplicate signal name 'little_u16'")
    # scale zero on a numeric signal
    refuse(CLASSIC.replace("scale: 0.5", "scale: 0.0"),
           "has scale 0, which cannot be encoded")
    # little-endian beyond the classic payload
    refuse(CLASSIC.replace("start_bit: 0\n          bit_length: 16",
                           "start_bit: 56\n          bit_length: 16"),
           "exceeds the 64-bit payload")
    # big-endian below payload bit 0
    refuse(CLASSIC.replace("start_bit: 23\n          bit_length: 12",
                           "start_bit: 5\n          bit_length: 12"),
           "extends below payload bit 0")
    # enum value key too wide
    refuse(CLASSIC.replace('2: "AUTO"', '4: "AUTO"'),
           "does not fit in 2 bits")
    # enum without values is schema-rejected, generator refuses too
    no_values = """
schema_version: "0.3.0"
codec_map:
  name: e
  version: 1.0.0
  messages:
    - id: 1
      name: M
      dlc: 1
      signals:
        - name: mode
          start_bit: 0
          bit_length: 2
          type: enum
          endianness: little
          values: {}
"""
    refuse(no_values, "requires a 'values' mapping")
    # boolean wider than one bit
    refuse(CLASSIC.replace("""        - name: flag
          start_bit: 40
          bit_length: 1""", """        - name: flag
          start_bit: 40
          bit_length: 2"""), "must be exactly 1 bit wide")
    # both layout spellings
    refuse(CLASSIC.replace("layout: sawtooth",
                           "layout: sawtooth\n          bit_layout: sawtooth"),
           "must not specify both")
    # unknown field
    refuse(CLASSIC.replace('unit: "m/s"', 'unit: "m/s"\n          gain: 2'),
           "unknown field 'gain' in signal")


def test_fd_dlc_vocabulary_refused_on_classic(tmp_path, workspace):
    text = """
schema_version: "0.3.0"
codec_map:
  name: m
  version: 1.0.0
  messages:
    - id: 1
      name: M
      dlc: 12
      signals:
        - name: s
          start_bit: 0
          bit_length: 1
          type: boolean
          endianness: little
"""
    with pytest.raises(Yaml2cError) as info:
        generate(text, tmp_path)
    # The schema rejects dlc 12 on a classic map before the generator runs.
    assert "dlc" in str(info.value)


def test_sawtooth_derivation_matches_loader(tmp_path, workspace):
    """sawtooth first_bit/last_bit use the loader's min/max walk."""
    header = generate(CLASSIC, workspace)
    # start_bit 7, length 12: sawtooth positions 7..0,15..12 -> [0, 15]
    assert ".first_bit = 0u, .last_bit = 15u" in header


def test_cli_writes_codec_header_with_custom_guard(tmp_path, workspace):
    source = workspace / "map.yaml"
    source.write_text(CLASSIC, encoding="utf-8")
    result = run_yaml2c("--schema-dir", str(workspace / "schemas"),
                        "--kind", "codec", "--stdout",
                        "--guard", "MY_GUARD_H", str(source))
    assert result.returncode == 0, result.stderr
    assert "#ifndef MY_GUARD_H" in result.stdout
    assert result.stdout.rstrip().endswith("#endif /* MY_GUARD_H */")
