#!/bin/bash
# CANcestry sandbox: build OpenModelica 1.24.0 (headless omc) from source.
#
# WHY: the CI-pinned openmodelica/openmodelica:v1.24.0-minimal image is not
# pullable from this sandbox (Docker Hub blocked). This script rebuilds the
# same toolchain from the pinned v1.24.0 commit using only reachable hosts
# (codeload.github.com, github.com git, registry.npmjs.org, pypi.org).
#
# This is LOCAL-DIAGNOSIS tooling only — it must never be used for evidence
# (HwAGENTS.md rule 5: the pinned Docker image is the evidence toolchain).
#
# Re-run after any /tmp wipe; every stage is idempotent. ~40-60 min at -j2.
set -e
S=/tmp
cd $S

# ---------------------------------------------------------------- sources ---
[ -f OpenModelica-1.24.0/OMCompiler/Compiler/FrontEnd/Absyn.mo ] || {
  echo "== download OM v1.24.0 (904c4c78 = tag v1.24.0)"
  rm -rf OpenModelica-1.24.0
  curl -sL "https://codeload.github.com/openmodelica/OpenModelica/tar.gz/904c4c783a5fa6eb9e99e4a98bdb0cca1d619303" -o om-src.tgz
  tar xzf om-src.tgz
  mv OpenModelica-904c4c783a5fa6eb9e99e4a98bdb0cca1d619303 OpenModelica-1.24.0
}
# OMCompiler/3rdParty submodule (FMIL, expat 2.1.0, sundials, gc, antlr 3.2...)
[ -d OpenModelica-1.24.0/OMCompiler/3rdParty/FMIL ] || {
  echo "== download OMCompiler-3rdParty (82e892e)"
  curl -sL "https://codeload.github.com/openmodelica/OMCompiler-3rdParty/tar.gz/82e892ece107787e9ff17780bf5ac8c3f6bc39ba" -o 3rdparty.tgz
  mkdir -p OpenModelica-1.24.0/OMCompiler/3rdParty
  tar xzf 3rdparty.tgz -C OpenModelica-1.24.0/OMCompiler/3rdParty --strip-components=1
}

# ------------------------------------------------------------------- JRE ----
# ANTLR 3.2 (parser codegen) needs a JVM. Zulu 8.15.0.2 committed in a public
# repo (codeloadable). Runs fine for ANTLR 3.2.
[ -x zjre/bin/java ] || {
  echo "== download Zulu JRE 8 (docker-zulu repo)"
  curl -sL "https://codeload.github.com/delitescere/docker-zulu/tar.gz/refs/heads/master" -o docker-zulu.tgz
  tar xzf docker-zulu.tgz
  mkdir -p zjre
  tar xzf docker-zulu-master/zre8.15.0.2-cp3-jre8.0.92-linux_x64.tar.gz -C zjre --strip-components=1
}
zjre/bin/java -version

# ------------------------------------------------------------------ venv ----
[ -x venv-om/bin/cmake ] || {
  echo "== python venv (cmake 3.28.3 + ninja)"
  python3 -m venv venv-om
  venv-om/bin/pip install -q "pip>=23" cmake==3.28.3 ninja
}

# ---------------------------------------------------------- header shims ----
# No dev headers for curl/uuid on the sandbox; minimal API shims are enough
# (om_curl.c uses a standard subset; runtime links the system shared libs).
mkdir -p fakeinc/curl fakeinc/uuid
if [ ! -f fakeinc/curl/curl.h ]; then python3 "$(dirname "$0")/make_header_shims.py"; fi

# ------------------------------------------------------------- source patch
python3 "$(dirname "$0")/patch_sources.py"

# ------------------------------------------------------------ configure ----
EXPAT_INC="-I$S/OpenModelica-1.24.0/OMCompiler/3rdParty/FMIL/ThirdParty/Expat/expat-2.1.0/lib"
rm -rf om-build
venv-om/bin/cmake -G Ninja \
  -DCMAKE_MAKE_PROGRAM=$S/venv-om/bin/ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOM_ENABLE_GUI_CLIENTS=OFF \
  -DOM_OMC_ENABLE_CPP_RUNTIME=OFF \
  -DOM_OMC_ENABLE_FORTRAN=OFF \
  -DOM_OMC_ENABLE_IPOPT=OFF \
  -DOM_OMC_USE_LAPACK=OFF \
  -DOM_USE_CCACHE=OFF \
  -DSUNDIALS_LAPACK_ENABLE=OFF \
  -DJava_JAVA_EXECUTABLE=$S/zjre/bin/java \
  -DCURL_INCLUDE_DIR=$S/fakeinc \
  -DCURL_LIBRARY=/usr/lib/x86_64-linux-gnu/libcurl.so.4 \
  -DUUID_LIB=/usr/lib/x86_64-linux-gnu/libuuid.so.1 \
  # NOTE: no -DOMC_BOOTSTRAPPING (OM's cmake build never defines it; the two
  # header choices it toggles are identical files in this tree). The box
  # layout corruption seen earlier came from a STALE boot/bomc staging, not
  # from this flag - see patch_sources.py section 6.
  -DCMAKE_C_FLAGS="-I$S/fakeinc $EXPAT_INC" \
  -DCMAKE_CXX_FLAGS="-I$S/fakeinc $EXPAT_INC" \
  -S $S/OpenModelica-1.24.0 -B $S/om-build

# ----------------------------------------------------------------- build ----
cd $S/om-build
$S/venv-om/bin/ninja -j2
echo "== BUILD OK"
ls -la omc 2>/dev/null || true

# --------------------------------------------------------------- install ----
# Install under a sandbox-local prefix (no root needed). omc finds its
# library dir from OPENMODELICAHOME/getInstallationDirectoryPath.
$S/venv-om/bin/cmake --install . --prefix $S/om-local
mkdir -p $S/om-local/share/omlibrary/libraries

# Modelica Standard Library 4.0.0 (same major as the CI docker image's
# installPackage(Modelica,"4.0.0")). The OM source tarball has no MSL
# submodule; the package server is unreachable, so take the 4.0.x
# maintenance branch from GitHub. Pre-generated .mo files: no make needed.
MSL_SHA=d2dcb39e055ceb88fef5328a26255cdd094389bd   # MA/maint/4.0.x
if [ ! -d "$S/msl-$MSL_SHA/Modelica" ]; then
  echo "== download ModelicaStandardLibrary (MA/maint/4.0.x)"
  curl -sL "https://codeload.github.com/OpenModelica/OpenModelica-ModelicaStandardLibrary/tar.gz/$MSL_SHA" -o msl.tgz
  tar xzf msl.tgz
  mv "OpenModelica-ModelicaStandardLibrary-$MSL_SHA" "$S/msl-$MSL_SHA"
fi
# Canonical installPackage layout: <libraries>/Modelica 4.0.0/package.mo
mkdir -p "$HOME/.openmodelica/libraries"
rm -rf "$HOME/.openmodelica/libraries/Modelica 4.0.0"
cp -r "$S/msl-$MSL_SHA/Modelica" "$HOME/.openmodelica/libraries/Modelica 4.0.0"
ln -sfn "Modelica 4.0.0" "$HOME/.openmodelica/libraries/Modelica"
# The package resolver consults this index BEFORE falling back to the
# (unreachable) package server; without it, loadModel(Modelica) tries
# curl and segfaults inside om_curl_multi_download.
cat > "$HOME/.openmodelica/libraries/index.json" <<'JSON'
{
  "libs": {
    "Modelica": {
      "versions": {
        "4.0.0": {
          "provides": ["Modelica 4.0.0"],
          "support": "fullSupport"
        }
      }
    }
  }
}
JSON
echo "== MSL installed to ~/.openmodelica/libraries"

echo "== omc smoke test"
$S/om-local/bin/omc --version || true
mkdir -p /tmp/om-smoke/T
printf 'within ;\npackage T "smoke"\n  model A\n    Real x;\n  end A;\nend T;\n' > /tmp/om-smoke/T/package.mo
printf 'loadFile("/tmp/om-smoke/T/package.mo"); getErrorString();' > /tmp/om-smoke/t1.mos
$S/om-local/bin/omc /tmp/om-smoke/t1.mos || true
printf 'loadModel(Modelica); getErrorString();' > /tmp/om-smoke.mos
$S/om-local/bin/omc /tmp/om-smoke.mos || true
