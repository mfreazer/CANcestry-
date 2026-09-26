#!/usr/bin/env python3
"""Local-diagnosis replay of the BOR ModelExchange sweep.

Builds the BOR FMU with the sandbox-built omc (same buildModelFMU script
text as hw/tests/test_bor_physics.py), replays the per-microsecond
ModelExchange sequence over 0..20000 us in BOTH candidate call orders,
and reports the sampled values at the brownout window instants against
the exact reference:

  * "cis_first" - the ea86315 order:
      setTime -> completedIntegratorStep -> enterEventMode
      -> newDiscreteStates* -> enterContinuousTimeMode -> sample
  * "pulse"     - the order the green pulse evaluator uses:
      setTime -> enterEventMode -> newDiscreteStates*
      -> enterContinuousTimeMode -> sample -> completedIntegratorStep

Also dumps the generated C from the FMU for code inspection (which
function computes `v`, and whether the branch is an inline ternary or a
latched relation).

NOT evidence tooling (HwAGENTS.md rule 5). Run from the repo root:
    /tmp/venv-fmpy/bin/python .om-sandbox/replay_bor.py
"""
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.chdir(REPO)

from hw.tests.test_bor_physics import (  # noqa: E402
    bor_build_script_text, brownout_window_times, BorReference,
    BOR_SAMPLED_OUTPUTS, load_sim_case)

BUILD = Path("/tmp/bor-diag")
OMC = "/tmp/om-local/bin/omc"


def build_fmu():
    BUILD.mkdir(parents=True, exist_ok=True)
    script = BUILD / "omc_build_bor.mos"
    script.write_text(bor_build_script_text(), encoding="utf-8")
    env = dict(os.environ)
    result = subprocess.run([OMC, "--showErrorMessages", str(script)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            cwd=str(BUILD), timeout=900, env=env)
    output = result.stdout.decode("utf-8", "replace")
    (BUILD / "omc_build.log").write_text(output)
    if result.returncode != 0:
        print("omc FAILED (rc=%d):\n%s" % (result.returncode, output[-4000:]))
        sys.exit(1)
    matches = re.findall(r'[\w./-]+\.fmu', output)
    assert matches, "no FMU name in omc output:\n" + output[-2000:]
    fmu = Path(matches[-1])
    if not fmu.is_absolute():
        fmu = BUILD / fmu
    assert fmu.is_file(), fmu
    print("FMU:", fmu, fmu.stat().st_size, "bytes")
    return fmu


def dump_generated_c(fmu):
    with zipfile.ZipFile(fmu) as zf:
        names = [n for n in zf.namelist() if n.endswith(".c")
                 or n.endswith(".h")]
        outdir = BUILD / "fmu-src"
        shutil.rmtree(outdir, ignore_errors=True)
        for n in names:
            if n.startswith("resources"):
                continue
            p = outdir / n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(zf.read(n))
    print("generated sources ->", outdir)
    for p in sorted(outdir.rglob("*.c")):
        text = p.read_text(errors="replace")
        if re.search(r"\bt_brownout\b", text):
            print("---- t_brownout appears in:", p)
    model_c = BUILD / "fmu-src" / "sources" / "model.c"
    if model_c.is_file():
        text = model_c.read_text(errors="replace")
        for m in re.finditer(r"v_nominal", text):
            s = max(0, m.start() - 260)
            print("---- context:\n" + text[s:m.end() + 260].strip() + "\n")
            break


def ref_outputs(ref, time_s):
    """Reference values for every sampled output at `time_s` seconds."""
    p = ref.p
    rail = ref.rail(time_s)
    asserted, nrst = ref.reset_state(time_s, rail)
    return {
        "v": rail,
        "nrst": nrst,
        "v_vbat": ref.vbat(time_s),
        "retention_preserved": ref.retention_preserved(time_s),
        "sram_preserved": ref.sram_preserved(time_s),
        "firmware_running": ref.firmware_running(time_s),
    }


def make_slave(fmu, instance):
    import fmpy
    from fmpy.fmi2 import FMU2Model

    description = fmpy.read_model_description(str(fmu))
    assert str(description.fmiVersion).startswith("2.0")
    assert description.modelExchange is not None
    assert description.numberOfContinuousStates == 0
    variables = {v.name: v.valueReference for v in description.modelVariables}
    params = dict(load_sim_case()["parameters"])
    directory = fmpy.extract(str(fmu))
    slave = FMU2Model(guid=description.guid, unzipDirectory=directory,
                      modelIdentifier=description.modelExchange.modelIdentifier,
                      instanceName=instance)
    slave.instantiate()
    stop_us = 20000
    slave.setupExperiment(startTime=0.0, stopTime=float(stop_us) / 1e6)
    slave.setReal([variables[n] for n in sorted(params)],
                  [float(params[n]) for n in sorted(params)])
    slave.enterInitializationMode()
    slave.exitInitializationMode()
    for _ in range(30):
        needed, terminate, *_ = slave.newDiscreteStates()
        assert not terminate
        if not needed:
            break
    slave.enterContinuousTimeMode()
    return slave, variables, stop_us


def replay(fmu, order):
    slave, variables, stop_us = make_slave(fmu, "bor_%s" % order)
    samples = {}
    try:
        for time_us in range(0, stop_us + 1):
            slave.setTime(float(time_us) / 1e6)
            if order == "cis_first":
                _, terminate = slave.completedIntegratorStep()
                assert not terminate, "terminate at %d us" % time_us
                slave.enterEventMode()
                for _ in range(30):
                    needed, terminate, *_ = slave.newDiscreteStates()
                    assert not terminate
                    if not needed:
                        break
                slave.enterContinuousTimeMode()
                values = slave.getReal(
                    [variables[n] for n in BOR_SAMPLED_OUTPUTS])
            elif order == "pulse":
                slave.enterEventMode()
                for _ in range(30):
                    needed, terminate, *_ = slave.newDiscreteStates()
                    assert not terminate
                    if not needed:
                        break
                slave.enterContinuousTimeMode()
                values = slave.getReal(
                    [variables[n] for n in BOR_SAMPLED_OUTPUTS])
                _, terminate = slave.completedIntegratorStep()
                assert not terminate, "terminate at %d us" % time_us
            else:
                raise ValueError(order)
            samples[time_us] = dict(zip(BOR_SAMPLED_OUTPUTS,
                                        (float(x) for x in values)))
        try:
            slave.terminate()
            print("  [%s] terminate: OK" % order)
        except Exception as e:  # noqa: BLE001
            print("  [%s] terminate: %r" % (order, e))
    finally:
        try:
            slave.freeModelInstance()
        except Exception:  # noqa: BLE001
            pass
    return samples


def report(order, samples):
    ref = BorReference()
    window_us = [int(round(t * 1e6)) for t in brownout_window_times()]
    print("\n== [%s] window instants ==" % order)
    for t in window_us:
        got = samples[t]
        want = ref_outputs(ref, t / 1e6)
        marks = []
        for name in BOR_SAMPLED_OUTPUTS:
            ok = got[name] == want[name]
            marks.append("%s%s" % (name, "" if ok else " MISMATCH(%r!=%r)"
                                   % (got[name], want[name])))
        print("  t=%6d us  %s" % (t, "  ".join(marks)))
    bad = 0
    for name in BOR_SAMPLED_OUTPUTS:
        mism = [(t, samples[t][name]) for t in samples
                if samples[t][name] != ref_outputs(ref, t / 1e6)[name]]
        bad += len(mism)
        if mism:
            print("  [%s] %s mismatches: %d of %d"
                  % (order, name, len(mism), len(samples)))
            for t, v in mism[:6]:
                print("     t=%d us got=%r want=%r"
                      % (t, v, ref_outputs(ref, t / 1e6)[name]))
    print("  [%s] TOTAL mismatched samples: %d" % (order, bad))
    return bad


def main():
    fmu = build_fmu()
    dump_generated_c(fmu)
    totals = {}
    for order in ("pulse", "cis_first"):
        print("\n##### replay order: %s" % order)
        samples = replay(fmu, order)
        totals[order] = report(order, samples)
    print("\n== summary ==")
    for order, bad in totals.items():
        print("  %s: %d mismatched samples" % (order, bad))


if __name__ == "__main__":
    main()
