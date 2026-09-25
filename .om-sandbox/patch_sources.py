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
    "#include \"sunlinsol/sunlinsol.h\"\n"
    "#include \"sundials/sundials_nvector.h\"\n"
    "#include \"sundials/sundials_matrix.h\"\n"
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
#    configure script, not needed for omc.
patch("OMCompiler/SimulationRuntime/fmi/export/buildproject/CMakeLists.txt", [
    ('message(FATAL_ERROR "autoconf failed configuring for FMUS.")',
     'message(STATUS "autoconf missing - skipping FMUS buildproject (not needed for omc)")'),
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

# 6. Stage the tarball's OWN bootstrap sources into bomc/ so CMake does NOT
#    download OMBootstrapping@master (newer OMC_-naming generation; pairing
#    it with the 1.24.0 runtime via name shims corrupts box layouts and
#    segfaults in AbsynToSCode). The bundled sources are the matching pair.
boot = os.path.join(ROOT, "OMCompiler/Compiler/boot")
if not os.path.exists(os.path.join(boot, "bomc", "bootstrap-sources")):
    os.makedirs(os.path.join(boot, "bomc"), exist_ok=True)
    shutil.copytree(os.path.join(boot, "bootstrap-sources"),
                    os.path.join(boot, "bomc", "bootstrap-sources"))
    open(os.path.join(boot, "bomc", "sources.tar.gz"), "w").close()
    shutil.copytree(os.path.join(boot, "tarball-include"),
                    os.path.join(boot, "bomc", "tarball-include"))
    print("staged  boot/bomc (bundled bootstrap-sources)")
else:
    print("ok      boot/bomc staged")

print("all patches done")
