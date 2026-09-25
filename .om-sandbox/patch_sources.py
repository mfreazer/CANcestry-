#!/usr/bin/env python3
"""Idempotent source patches for the sandbox OM 1.24.0 build.

All changes are local-build adaptations for a dependency-poor sandbox
(no LAPACK, no autoconf, no curl/uuid dev headers). None of them change
the behaviour of the compiled compiler except removing LAPACK support
(never used by the BOR FMU build) — see .om-sandbox/om-build.sh header.
"""
import os
import shutil

ROOT = os.environ.get("OM_ROOT", "/tmp/OpenModelica-1.24.0")


def patch(path, pairs):
    p = os.path.join(ROOT, path)
    t = open(p).read()
    changed = False
    for old, new in pairs:
        if new in t:
            continue
        if old in t:
            t = t.replace(old, new)
            changed = True
    if changed:
        open(p, "w").write(t)
        print("patched ", path)
    else:
        print("ok      ", path)


def strip_lines(path, patterns):
    p = os.path.join(ROOT, path)
    lines = open(p).read().split("\n")
    kept = [l for l in lines if not any(pat in l for pat in patterns)]
    if len(kept) != len(lines):
        open(p, "w").write("\n".join(kept))
        print("stripped", path)
    else:
        print("ok      ", path)


# 1. sundials LAPACK-dense target: only exists when SUNDIALS_LAPACK_ENABLE
patch("OMCompiler/3rdParty/CMakeLists.txt", [
    ("target_link_libraries(sundials_sunlinsollapackdense_static PUBLIC sundials_interface_static)\n",
     "if(SUNDIALS_LAPACK_ENABLE)\ntarget_link_libraries(sundials_sunlinsollapackdense_static PUBLIC sundials_interface_static)\nendif()\n"),
    ("add_library(omc::3rd::sundials::sunlinsollapackdense ALIAS sundials_sunlinsollapackdense_static)\n",
     "if(SUNDIALS_LAPACK_ENABLE)\nadd_library(omc::3rd::sundials::sunlinsollapackdense ALIAS sundials_sunlinsollapackdense_static)\nendif()\n"),
])

# 2. runtime cmake: LAPACK optional; stub the two SUNLinSol_LapackDense
#    entry points when the lapackdense library is not built.
stub = (
    "/* CANcestry sandbox stub: SUNDIALS built without LAPACK, so the\n"
    " * sunlinsollapackdense library is absent. The KINSOL LAPACK path is\n"
    " * never exercised in headless FMU builds; the stub returns NULL so a\n"
    " * caller fails loudly. */\n"
    "#include <stddef.h>\n"
    "#include \"sunlinsol/sunlinsol_dense.h\"  /* pulls N_Vector/SUNMatrix/SUNLinearSolver/SUNDIALS_EXPORT; sunlinsol.h itself does not exist in SUNDIALS 5.4.0 (per-solver headers only) */\n"
    "SUNDIALS_EXPORT SUNLinearSolver SUNLinSol_LapackDense(N_Vector y, SUNMatrix A)\n"
    "{ (void)y; (void)A; return NULL; }\n"
    "SUNDIALS_EXPORT SUNLinearSolver SUNLapackDense(N_Vector y, SUNMatrix A)\n"
    "{ (void)y; (void)A; return NULL; }\n"
)
stub_path = os.path.join(ROOT, "OMCompiler/SimulationRuntime/c/lapackdense_stub.c")
if not os.path.exists(stub_path):
    open(stub_path, "w").write(stub)
    print("created lapackdense_stub.c")
patch("OMCompiler/SimulationRuntime/c/cmake_3.14.cmake", [
    ("find_package(LAPACK REQUIRED)",
     "find_package(LAPACK QUIET)  # sandbox: no system LAPACK"),
    ("target_link_libraries(SimulationRuntimeC PUBLIC omc::3rd::sundials::sunlinsollapackdense)\n",
     "if(SUNDIALS_LAPACK_ENABLE)\ntarget_link_libraries(SimulationRuntimeC PUBLIC omc::3rd::sundials::sunlinsollapackdense)\nelse()\ntarget_sources(SimulationRuntimeC PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lapackdense_stub.c)\nendif()\n"),
])

# 3. FMUS buildproject: needs autoconf (absent); it only installs a helper
#    configure script, not needed for omc. When autoconf fails, CMake still
#    runs install(PROGRAMS configure), so write a dummy configure to keep
#    `cmake --install` working.
patch("OMCompiler/SimulationRuntime/fmi/export/buildproject/CMakeLists.txt", [
    ('message(FATAL_ERROR "autoconf failed configuring for FMUS.")',
     'message(STATUS "autoconf missing - skipping FMUS buildproject (not needed for omc)")\n      file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/configure "#!/bin/sh\\nexit 0\\n")  # sandbox: dummy so install(PROGRAMS configure) succeeds'),
])

# 4. The BUNDLED (older-generation) bootstrap interpreter silently HALTS a
#    script at `echo(false)` (legacy special-case, not the 1.24.0 builtin
#    echo(Boolean)->Boolean). Every bomc-run template starts with it.
for f in ["OMCompiler/Compiler/.cmake/mm_check_interface.in.mos",
          "OMCompiler/Compiler/.cmake/mm_compile.in.mos",
          "OMCompiler/Compiler/Script/OpenModelicaScriptingAPI.mos"]:
    strip_lines(f, ["echo(false);"])

# 5. The same old bootstrap breaks on the PAIR of `//` comments below
#    (either alone is fine): the following `if` then trips
#    "Expected Boolean, got Boolean" in typechecking.
strip_lines("OMCompiler/Compiler/.cmake/mm_check_interface.in.mos", [
    '// print("Interface is the same',
    '// print("Interface has changed',
])

# 6. Stage OMBootstrapping@c289e97 (the OMBootstrapping@master that was
#    current on 2024-10-04, the v1.24.0 tag date - i.e. exactly what the
#    pinned docker image's CMake downloaded) into boot/bomc/ so CMake does
#    NOT download today's master.
#
#    The tree-bundled boot/bootstrap-sources and boot/tarball-include are
#    STALE (7-field Absyn.Class.CLASS, pre-2022). Pairing them with the
#    1.24.0 tree corrupts the build two ways:
#      - the parser (Parser/Modelica.g) builds 7-field CLASS boxes from the
#        stale tarball header while bomc codegen emits 9-field offsets
#        (read of class[10] past the end of a 7-field box) -> the final omc
#        segfaults in SymbolTable_updateUriMapping on ANY load;
#      - the c289e97 bootstrap C sources compiled against the stale header
#        (record/macro mismatch inside bomc) -> bomc itself segfaults.
#    The c289e97 tarball's OWN tarball-include header has the 9-field
#    CLASS matching the tree's Absyn.mo, making parser, bootstrap and
#    codegen mutually consistent.
import subprocess
OMBOOT_SHA = "c289e97"  # OMBootstrapping master as of 2024-10-04 (v1.24.0 tag)
boot = os.path.join(ROOT, "OMCompiler/Compiler/boot")
bomc = os.path.join(boot, "bomc")
pin_header = os.path.join(bomc, "tarball-include", "OpenModelicaBootstrappingHeader.h")
need_stage = (not os.path.exists(pin_header)) or (
    "commentsBeforeEnd" not in open(pin_header).read())
if need_stage:
    tgz = "/tmp/omboot-pin.tgz"
    if not os.path.exists(tgz):
        subprocess.run(["curl", "-sL",
                        "https://codeload.github.com/OpenModelica/OMBootstrapping/tar.gz/" + OMBOOT_SHA,
                        "-o", tgz], check=True)
    os.makedirs(bomc, exist_ok=True)
    for sub in ("bootstrap-sources", "tarball-include"):
        dst = os.path.join(bomc, sub)
        if os.path.exists(dst):
            shutil.rmtree(dst)
        # extract just that top-level entry
        subprocess.run(["tar", "xzf", tgz, "-C", bomc,
                        "--strip-components=1", "OMBootstrapping-%s/%s" % (OMBOOT_SHA, sub)],
                       check=True)
    # CMake skips its OMBootstrapping@master download when this exists
    open(os.path.join(bomc, "sources.tar.gz"), "wb").close()
    print("staged  OMBootstrapping@%s -> boot/bomc" % OMBOOT_SHA)
else:
    print("ok      boot/bomc staged (pinned header present)")

print("all patches done")
