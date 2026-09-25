#!/usr/bin/env python3
"""Local-diagnosis replay of the BOR ModelExchange sweep.

Builds the BOR FMU with the sandbox-built omc (same buildModelFMU script
text as hw/tests/test_bor_physics.py), replays the EXACT per-microsecond
ModelExchange sequence of execute_bor() over 0..20000 us, and reports the
sampled values at the brownout window instants. Also dumps the generated
C from the FMU for code inspection (the inline-ternary vs relations-latch
question).

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
        if "fmi2GetReal" in text or re.search(r"\bv\b.*v_nominal|t_brownout", text):
            print("---- candidate generated file:", p)
        # the model C: find the equation for 'v'
        m = re.search(r"(.{0,200}t_brownout.{0,300})", text)
        if m:
            print(m.group(1))
            break


def replay(fmu):
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
                      instanceName="bor_diag")
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

    window_us = [int(round(t * 1e6)) for t in brownout_window_times()]
    samples = {}
    for time_us in range(0, stop_us + 1):
        slave.setTime(float(time_us) / 1e6)
        _, terminate = slave.completedIntegratorStep()
        assert not terminate, "terminate at %d us" % time_us
        slave.enterEventMode()
        for _ in range(30):
            needed, terminate, *_ = slave.newDiscreteStates()
            assert not terminate
            if not needed:
                break
        slave.enterContinuousTimeMode()
        values = slave.getReal([variables[n] for n in BOR_SAMPLED_OUTPUTS])
        samples[time_us] = dict(zip(BOR_SAMPLED_OUTPUTS,
                                    (float(x) for x in values)))
        if time_us % 2000 == 0:
            print("  ...%d us  v=%r" % (time_us, samples[time_us]["v"]),
                  flush=True)
    try:
        slave.terminate()
        print("terminate: OK")
    except Exception as e:  # noqa: BLE001
        print("terminate: %r" % e)
    slave.freeModelInstance()
    return samples, window_us


def main():
    fmu = build_fmu()
    dump_generated_c(fmu)
    samples, window_us = replay(fmu)
    ref = BorReference()
    print("\n== window instants (us) ==")
    print("reference window:", window_us)
    worst = 0.0
    for t in window_us:
        got = samples[t]
        want = ref.rail(t / 1e6)
        mark = "OK " if got["v"] == want else "MISMATCH"
        worst = max(worst, abs(got["v"] - want))
        print("%s t=%6d us  v=%-8r (want %r)  nrst=%r v_vbat=%r"
              % (mark, t, got["v"], want, got["nrst"], got["v_vbat"]))
    print("worst |v - rail| at window instants:", worst)
    # full-sweep mismatches of v against the reference rail
    mism = [(t, samples[t]["v"]) for t in samples
            if samples[t]["v"] != ref.rail(t / 1e6)]
    print("full-sweep v mismatches: %d of %d" % (len(mism), len(samples)))
    for t, v in mism[:12]:
        print("   t=%d us v=%r want=%r" % (t, v, ref.rail(t / 1e6)))


if __name__ == "__main__":
    main()
