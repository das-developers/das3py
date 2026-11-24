# Simple wrapper around PIP so that higher level tools can invoke using
# make, make test, make install etc.
#
# Variables you may set:
#
#  PY_BIN      The python to build and test with, default: python on the path
#
#  DAS2C       A das2C source tree that you build yourself, used in place of
#              the pinned one in deps/das2C.  Its library is expected in
#              $(DAS2C)/build.$(N_ARCH).
#
#  DAS_INCDIR  Where das2C's headers and library are, for an installed or
#  DAS_LIBDIR  packaged das2C.  Either one overrides DAS2C for its part.
#
#  CONTAINER_ENGINE  podman (default) or docker, for make pypi
#  PYPI_DIRTY        yes to let make pypi build from uncommitted changes

# Find a way to get this from the manifest.  Until then the version is
# also in pyproject.toml, setup.py (python 2 builds) and das3/__init__.py;
# keep all four in step.
DAS_PY_VER:=3.0.7

ifeq ($(PY_BIN),)
PY_BIN=$(shell which python)

ifeq ($(PY_BIN),)
PY_BIN=$(shell which python3)
endif

ifeq ($(PY_BIN),)
$(error Neither python nor python3 were found, set PY_BIN to the path to your python interpreter)
endif
endif

# das2C comes from the git submodule in deps/das2C, which pins the exact
# das2C commit this das3py was written and tested against.  A das2C checkout
# next door moves whenever someone works on it, so building against one by
# default meant a passing test said nothing about any das2C release.
#
# Naming a das2C of your own, by any of the three variables above, turns the
# bundled build off; building that das2C is then up to you.
DAS2C_SUB:=deps/das2C

ifeq ($(DAS2C)$(DAS_INCDIR)$(DAS_LIBDIR),)
DAS2C_BUNDLED:=yes
DAS2C:=$(CURDIR)/$(DAS2C_SUB)
else
DAS2C_BUNDLED:=no
endif

# Absolute paths, python -m build compiles in a temporary copy of the sources.
# abspath and not realpath: the bundled library directory doesn't exist until
# das2C is built, and realpath answers nothing for a missing path.
ifeq ($(DAS_INCDIR),)
DAS_INCDIR:=$(abspath $(DAS2C))
endif

ifeq ($(DAS_LIBDIR),)
DAS_LIBDIR:=$(abspath $(DAS2C)/build.$(N_ARCH))
endif

# Just depend on das2C providing a working libcdf.so and copy it in, don't 
# worry about CDF_LIB

# ########################################################################### #
# Try to predict the wheel name.  This is a fools game but most standard tools
# will skip this makefile anyway and jump straight to a python -m build style
# command.

PY_VER_TOK:=$(shell $(PY_BIN) -c "import sys; print('%d%d'%sys.version_info[0:2])")
PY_MAJ_VER_TOK:=$(shell $(PY_BIN) -c "import sys; print(sys.version_info[0])")

# The ABI suffix in the wheel tag comes from the interpreter, not from a
# guess: 'm' (pymalloc) through 3.7, nothing from 3.8 on, 'd' for a debug
# build.  2.7 has no sys.abiflags; wheel tags it 'm', plus 'u' for a UCS-4
# build (most distro pythons), which sys.maxunicode reveals.
PY_ABI_TOK:=$(shell $(PY_BIN) -c "import sys; f=getattr(sys,'abiflags',None); print(f if f is not None else 'm'+('u' if sys.maxunicode>65535 else ''))")

WHEEL_FILE:=das3py-$(DAS_PY_VER)-cp$(PY_VER_TOK)-cp$(PY_VER_TOK)$(PY_ABI_TOK)-linux_x86_64.whl

# One pair of venvs per interpreter (build_py312_venv, test_py27_venv, ...),
# not per major version.  A home directory shared between machines with
# different pythons would otherwise have two boxes rebuilding one venv.
VDIR:=py$(PY_VER_TOK)_venv

ifeq ($(PY_MAJ_VER_TOK),3)
VENV_MOD:=venv
PY_VER_WARN:=
else
VENV_MOD:=virtualenv
PY_VER_WARN:=--no-python-version-warning
endif

# ########################################################################### #

# The extension sources belong here too.  Without them a C-only edit leaves
# dist/ newer than everything make knows about, so the wheel is silently not
# rebuilt and you test the previous build.

SRC:= \
pyproject.toml \
setup.py \
src/_das3.c \
src/py_builder.h \
src/py_catalog.h \
src/py_dft.h \
das3/__init__.py \
das3/auth.py \
das3/cdf.py \
das3/cdfinfo.py \
das3/cli.py \
das3/container.py \
das3/das-basic-doc-ns-v3.0.xsd \
das3/das-basic-stream-ns-v3.0.xsd \
das3/das-basic-stream-v2.2.xsd \
das3/das-basic-stream-v3.0.xsd \
das3/dastime.py \
das3/dataset.py \
das3/dft.py \
das3/mpl.py \
das3/node.py \
das3/pkt.py \
das3/reader.py \
das3/source.py \
das3/streamsrc.py \
das3/toml.py \
das3/util.py \
das3/verify.py \
das3/pycdf/__init__.py \
das3/pycdf/const.py \
das3/pycdf/LICENSE.md


.PHONY: build dist test install clean distclean examples das2C pypi

build:dist/$(WHEEL_FILE)

# ########################################################################### #
# The bundled das2C

ifeq ($(DAS2C_BUNDLED),yes)

# An empty deps/das2C means the clone was made without its submodules
$(DAS2C_SUB)/Makefile:
	@echo "ERROR: $(DAS2C_SUB) is empty.  Fetch the pinned das2C with:"
	@echo "          git submodule update --init"
	@echo "       (git clone --recursive does this for a new clone), or name a"
	@echo "       das2C of your own with DAS2C=/path/to/das2C"
	@exit 3

# das2C's makefile decides what is out of date, so ask it every time: after
# 'git submodule update' moves the pin, the sources changed and nothing here
# can tell.  When nothing changed das2C leaves the library's time stamp alone
# and the wheel is not rebuilt.
#
# SPICE=no because das3py links none of it, which saves fetching and
# unpacking CSPICE.  CDF=yes because das2C then builds the CDF library and
# copies its libcdf.so up beside libdas3.a, where setup.py takes it for
# pycdf.  CDF_INC and CDF_LIB in the environment point das2C at a CDF you
# already have; without them it downloads the CDF sources from NASA.
#
# This is das2C's whole default build, programs and tests included, since
# its makefile has no library-only target.  It only costs time once.
das2C: | $(DAS2C_SUB)/Makefile
	$(MAKE) -C $(DAS2C_SUB) SPICE=no CDF=yes

$(DAS_LIBDIR)/libdas3.a: das2C ;

$(DAS_LIBDIR)/libcdf.so: das2C ;

else

das2C:
	@echo "Using the das2C library in $(DAS_LIBDIR), headers in $(if $(DAS_INCDIR),$(DAS_INCDIR),the compiler's default places); not built from here"

endif

# ########################################################################### #

# The static das2C library is linked into the extension, so a das2C rebuild
# must trigger a wheel rebuild too, or you test against the previous library.
# Likewise for the libcdf.so das2C copies up, it's packaged for pycdf.
#
# Wheels only (-w), no source distribution.  A das3py sdist can't be built
# by whoever downloads it without a das2C they supply themselves, and the
# ones current setuptools writes carry the libcdf.so that setup.py copies
# into das3/pycdf, a Linux binary in a "source" package.  With no sdist in
# dist/, 'twine upload dist/*' can't publish one by accident.  People who
# want to build get the git repository, which pins das2C.
#
# The price of -w is that setuptools compiles here in ./build, not in a
# fresh copy, and it rebuilds the extension only when src/_das3.c is newer.
# A changed py_*.h or a new libdas3.a would be linked stale.  So ./build is
# removed before and after.
#
# The build leaves das3py.egg-info in the source tree.  An old pip (9.x, as
# on python 3.6 hosts) runs with the current directory on sys.path, sees
# that egg-info, decides das3py is already installed here, and skips the
# wheel; the test venv then has numpy but no das3.  Remove it right away.
dist/$(WHEEL_FILE):$(SRC) $(DAS_LIBDIR)/libdas3.a $(DAS_LIBDIR)/libcdf.so build_$(VDIR)/bin/python
	rm -rf build
	DAS_INCDIR=$(DAS_INCDIR) DAS_LIBDIR=$(DAS_LIBDIR) build_$(VDIR)/bin/python -m build -w
	-rm -r build *.egg-info

build_$(VDIR)/bin/python:
	$(PY_BIN) -m $(VENV_MOD) build_$(VDIR)
	build_$(VDIR)/bin/python -m pip install $(PY_VER_WARN) build

test:dist/$(WHEEL_FILE)
	# Creating temporary environment for testing
	$(PY_BIN) -m $(VENV_MOD) test_$(VDIR)
	# A reused venv may hold a das2py, which owns das_verify (and das3/
	# through 3.0rc7); removing it after das3py goes in deletes those files
	./test_$(VDIR)/bin/python -m pip uninstall -y --isolated $(PY_VER_WARN) das2py das3py
	./test_$(VDIR)/bin/python -m pip install --isolated $(PY_VER_WARN) dist/$(WHEEL_FILE)
	@./test_$(VDIR)/bin/python -c 'import numpy;print("===================================");print("  Numpy Runtime Version is %s"%numpy.__version__);		print("===================================")'
	# The test list itself is shared with the cibuildwheel builds
	PY=$(CURDIR)/test_$(VDIR)/bin/python bash test/wheel_tests.sh
	@echo "All tests ran without returning an error code"

# Every example writes a PNG and none opens a window, so the file backend is
# the right one on a build box with no display and on an interpreter built
# without Tk.  matplotlib honors this variable since 1.5.
examples: export MPLBACKEND = Agg

# test/ex08_dynaspec_namespace.d3t is aspirational and left out on purpose: it
# is the only namespaced stream, and its width list is 6 short of index="-;152".
examples:
	# Creating temporary environment for testing, verify more streams, re-gen all example plots
	$(PY_BIN) -m $(VENV_MOD) test_$(VDIR)
	./test_$(VDIR)/bin/python -m pip uninstall -y --isolated $(PY_VER_WARN) das2py das3py
	./test_$(VDIR)/bin/python -m pip install --isolated $(PY_VER_WARN) dist/$(WHEEL_FILE)
	./test_$(VDIR)/bin/python -m pip install --isolated $(PY_VER_WARN)  matplotlib
	@./test_$(VDIR)/bin/python -c 'import numpy;print("===================================");print("  Numpy Runtime Version is %s"%numpy.__version__);		print("===================================")'
	./test_$(VDIR)/bin/das_verify test/ex18_juno_waves_wfrm.d3b
	./test_$(VDIR)/bin/das_verify test/ex03_galileo_pws_lrs.d2t
	./test_$(VDIR)/bin/das_verify test/ex12_sounder_xyz.d3t
	./test_$(VDIR)/bin/das_verify test/ex13_object_annotation.d3t
	./test_$(VDIR)/bin/das_verify test/ex14_object_tfcat.d3t
	./test_$(VDIR)/bin/das_verify test/ex15_vector_frame.d3t
	./test_$(VDIR)/bin/das_verify test/ex16_mag_grid_doc.d3x
	./test_$(VDIR)/bin/das_verify test/ex17_vector_noframe.d3b
	./test_$(VDIR)/bin/das_verify test/ex22_mag_grid_vec.d3t
	./test_$(VDIR)/bin/das_verify test/ex40_rotation.d3b
	./test_$(VDIR)/bin/das_verify test/ex41_quaternion.d3t
	./test_$(VDIR)/bin/das_verify test/ex42_plain_tensor.d3t
	./test_$(VDIR)/bin/das_verify test/ex43_msc_complex_cal.d3t
	./test_$(VDIR)/bin/das_verify test/ex96_yscan_multispec.d2t
	./test_$(VDIR)/bin/python examples/c_module/galileo_pws_e-survey.py
	./test_$(VDIR)/bin/python examples/c_module/juno_hfwbr_cdf.py
	./test_$(VDIR)/bin/python examples/ex01_source_queries.py
	./test_$(VDIR)/bin/python examples/ex02_galileo_pws_spectra.py	
	./test_$(VDIR)/bin/python examples/ex03_cassini_rpws_multimode.py
	./test_$(VDIR)/bin/python examples/ex04_voyager_pws_query_by_time.py
	./test_$(VDIR)/bin/python examples/ex08_juno_waves_wfrm_to_cdf.py
	./test_$(VDIR)/bin/python examples/ex09_cassini_fce_ephem_ticks.py 2017-01-02
	./test_$(VDIR)/bin/python examples/ex10_manual_datasets.py
	./test_$(VDIR)/bin/python examples/ex11_catalog_listings.py
	./test_$(VDIR)/bin/python examples/ex12_tracers_mag_delta.py
	@echo "All examples ran without returning an error code"


# Removing an old das2py first matters: it owns a das_verify script, which
# das3py installs too.
install:
	$(PY_BIN) -m pip uninstall -y --isolated --no-python-version-warning das2py das3py
	$(PY_BIN) -m pip install --isolated --no-python-version-warning ./dist/$(WHEEL_FILE)

# ########################################################################### #
# Wheels for PyPI
#
# The wheels above are tagged linux_x86_64 and linked against this machine's
# C library; PyPI refuses them.  These are built in the manylinux_2_28
# container image (AlmaLinux 8), one per python from 3.9 up plus a cp27mu
# one, and repaired by auditwheel, so they run on glibc 2.26 and newer.
# They go in their own directory so they can't be mixed up with the above:
#
#   make pypi             (from a committed tree; PYPI_DIRTY=yes to override)
#   twine upload dist/pypi/*
#
# Needs a python 3 PY_BIN, for cibuildwheel, and podman or docker
# (CONTAINER_ENGINE).  Takes about 15 minutes; das2C and expat are built
# twice, once for the python 3 wheels and once for 2.7.

PYPI_DIST:=dist/pypi
CONTAINER_ENGINE?=podman

# Pinned to the release line this was tested with.  Each cibuildwheel
# release also pins the dated manylinux image it pulls, so a newer one
# changes the build environment too.
build_$(VDIR)/bin/cibuildwheel:build_$(VDIR)/bin/python
	build_$(VDIR)/bin/python -m pip install "cibuildwheel>=4.3.5,<5"

pypi:
ifneq ($(PY_MAJ_VER_TOK),3)
	@echo "ERROR: make pypi needs a python 3 PY_BIN, to run cibuildwheel"
	@exit 3
else
	$(MAKE) build_$(VDIR)/bin/cibuildwheel
	CIBW=$(CURDIR)/build_$(VDIR)/bin/cibuildwheel ENGINE=$(CONTAINER_ENGINE) \
	 PYPI_DIRTY=$(PYPI_DIRTY) bash buildfiles/pypi_wheels.sh $(PYPI_DIST)
endif

# clean leaves das2C alone, it doesn't depend on the python in use.
# distclean removes the bundled das2C's build as well, but not the CDF
# sources it downloaded; for that: make -C deps/das2C distclean
clean:
	-rm -r dist test_*_venv build *.egg-info LICENSES

distclean:
	-rm -r dist test_*_venv build_*_venv build build.pypi *.egg-info LICENSES
ifeq ($(DAS2C_BUNDLED),yes)
	-if [ -f $(DAS2C_SUB)/Makefile ] ; then $(MAKE) -C $(DAS2C_SUB) clean ; fi
endif

