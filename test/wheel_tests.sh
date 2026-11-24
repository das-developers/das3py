#!/bin/bash
# The das3py test list, run against an installed das3py.
# Both 'make test' and cibuildwheel's test-command call this, so there is
# one list to keep up.
#
# Run from a directory holding test/ and das3/: the repository root, or
# cibuildwheel's test directory (see test-sources in pyproject.toml).  The
# scripts open test/... by relative path, and CheckPortable reads das3/ as
# source text.  The package under
# test is still the installed one: python puts a script's own directory on
# sys.path, not the current one.
#
# TestCatalog needs das2.org; a failure there fails the run, on purpose
# (das3py is a catalog client).
#
# PY selects the interpreter, default the python on PATH, which in
# cibuildwheel's test step is the test venv's.  das_verify and das_cdfinfo
# are taken from the same bin directory.
set -e -x

PY=${PY:-python}
BIN=$(dirname "$(command -v "$PY")")

# Proof that the installed package is what gets tested.  'python -c' puts
# the current directory first on sys.path, and the current directory may
# hold the das3/ sources, so it is dropped first.
"$PY" -c '
import sys, os
sys.path = [p for p in sys.path if p not in ("", ".", os.getcwd())]
import das3, _das3
print("das3  %s\n_das3 %s" % (das3.__file__, _das3.__file__))
'

for t in CheckPortable TestCatalog TestDasTime TestSortMinimal TestRead \
	TestComposite TestSource TestPkt TestProps TestQuantity TestPsd
do
	"$PY" test/$t.py
done

"$BIN/das_verify" -h > /dev/null
for s in ex05_waveform_extra.d3t ex40_rotation.d3t ex43_msc_complex_cal.d3b \
	ex16_mag_grid_doc.d3x ex20_tracers_aci.d3t
do
	"$BIN/das_verify" test/$s
done
"$BIN/das_cdfinfo" -h > /dev/null
"$BIN/das_cdfinfo" test/vg1_pws_wf_2023-10-24T03_v1.0.cdf

echo "wheel_tests.sh: all passed"
