# CANcestry hardware toolchain image (issue #33, H-01; HW-PLAN §7, C5).
#
# OpenModelica (omc, headless FMU build) + FMPy (FMU execution) + Python
# (schema validation, ledger gate, tests). The base is pinned by digest;
# ci/docker/base-image.digest is the single source of the pin and is
# re-verified against the registry by .github/workflows/hw-fast.yml.
FROM openmodelica/openmodelica:v1.24.0-minimal@sha256:ab60ed68b4ba3aafa3cddb6319b31ead0346416fb0f2b62bdd7a9e19b486227b

ENV PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PIP_NO_CACHE_DIR=1

# The OM minimal image ships omc; add a FMPy-capable Python (>= 3.10) and
# the pinned Python toolchain. Pins are deterministic (HwAGENTS.md rule 5);
# hw/tests/test_power_sim.py verifies the measured fmpy/numpy/OpenModelica
# versions against hw/tests/evidence/holdup_001.json on every run.
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 python3-pip python3-venv \
    && python3 -m venv /opt/venv \
    && rm -rf /var/lib/apt/lists/*

# The OpenModelica minimal image intentionally omits the Modelica Standard
# Library. Install the pinned MSL into the image so the FMU build is offline
# and reproducible at docker-run time (the container runs as root, matching
# this build layer's OpenModelica library home).
RUN printf '%s\n' \
    'updatePackageIndex();' \
    'installPackage(Modelica, "4.0.0");' \
    'getErrorString();' \
    > /tmp/install-modelica.mos \
    && omc /tmp/install-modelica.mos \
    && rm -f /tmp/install-modelica.mos

ENV PATH="/opt/venv/bin:${PATH}"

RUN python3 -m pip install \
    capellambse==0.6.17 \
    fmpy==0.3.24 \
    numpy==2.1.3 \
    jsonschema==4.23.0 \
    pytest==8.3.2 \
    pytest-cov==5.0.0
