#!/bin/bash
# Builds, repairs and tests the python 2.7 (cp27mu) wheel.  Runs INSIDE the
# manylinux_2_28 image with the source tree at /project, the way
# cibuildwheel would, which can't do this itself: it dropped 2.7.
#
#   podman run --rm -v "$PWD":/project -v "$OUT":/output \
#      quay.io/pypa/manylinux_2_28_x86_64 bash /project/buildfiles/build_cp27mu.sh
#
# The interpreter is EL8's own python2 (AppStream module python27), a
# UCS-4 build, so the wheel is tagged cp27mu: the ABI of every distro 2.7
# and of a source build configured --enable-unicode=ucs4.  A default
# source build (UCS-2, cp27m) can't install it.
#
# The repaired wheel goes to /output.
set -e -x -o pipefail

PROJ=${PROJ:-/project}
OUT=${OUT:-/output}
WORK=$(mktemp -d)

# Static expat and das2C, the same as for the python 3 wheels.  Done before
# python2-devel is installed so nothing that package pulls in can be what
# das2C finds.
bash "$PROJ/buildfiles/cibw_before_all.sh"

# The rpm python2 compiles extensions with Red Hat's specs, one of which
# loads the annobin gcc plugin.  The image's gcc is a gcc-toolset one that
# lacks it ("inaccessible plugin file plugin/gcc-annobin.so"), so install
# the plugin for whichever toolset provides the gcc on PATH.
TOOLSET=$(rpm -qf --qf '%{NAME}' "$(command -v gcc)")
dnf install -y python2 python2-devel "${TOOLSET%-gcc}-gcc-plugin-annobin"
python2 -c 'import sys; assert sys.maxunicode > 0xFFFF, "not a UCS-4 python"'

# EL8 ships pip 9, which predates pyproject.toml builds and manylinux_X_Y
# tags.  20.3.4 is the last pip for 2.7.  A venv keeps the rpm's files
# alone; virtualenv is the only way to make one on 2.7, and pip picks the
# last release that still runs there.
python2 -m pip install --user --upgrade "pip>=20.3.4,<21"
python2 -m pip install --user virtualenv
python2 -m virtualenv "$WORK/bld"
BPY=$WORK/bld/bin/python
"$BPY" -m pip install --no-python-version-warning --upgrade "pip>=20.3.4,<21"

# Build isolation takes setuptools and numpy from pyproject.toml's
# build-system table; on 2.7 that resolves to setuptools 44 and numpy
# 1.16.6, the last releases for it.
export DAS_EXPAT_LIB=/usr/local/lib/libexpat.a
"$BPY" -m pip wheel --no-python-version-warning --no-deps -w "$WORK/dist" "$PROJ"

auditwheel repair -w "$OUT" "$WORK"/dist/das3py-*-cp27-cp27mu-linux_x86_64.whl
WHL=$(ls "$OUT"/das3py-*-cp27-cp27mu-manylinux*.whl)

# Test in a second, clean venv, as make test does
python2 -m virtualenv "$WORK/tst"
TPY=$WORK/tst/bin/python
"$TPY" -m pip install --no-python-version-warning --upgrade "pip>=20.3.4,<21"
"$TPY" -m pip install --no-python-version-warning "$WHL"
cd "$PROJ"
PY=$TPY bash test/wheel_tests.sh

rm -rf "$WORK"
echo "build_cp27mu.sh: $(basename "$WHL") built, repaired and tested"
