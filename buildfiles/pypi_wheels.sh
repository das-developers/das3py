#!/bin/bash
# Builds every wheel that goes to PyPI: cp39 and up through cibuildwheel,
# and cp27mu through build_cp27mu.sh, all in the manylinux_2_28 image.
# Normally run as 'make pypi'.
#
#   buildfiles/pypi_wheels.sh OUT_DIR
#
#   CIBW         the cibuildwheel to run (make pypi installs one in the
#                build venv)
#   ENGINE       container engine, podman (default) or docker
#   IMAGE        image for the cp27mu build, default the current
#                manylinux_2_28 one (cibuildwheel uses a dated tag of the
#                same image, fixed by its own release)
#   PYPI_DIRTY   'yes' to build with uncommitted changes in the tree
#
# Wheels already in OUT_DIR are deleted first, so afterwards it holds
# exactly what this run built and 'twine upload OUT_DIR/*' can't pick up
# an older one.
#
# The containers get a staged copy of the tree, in build.pypi/, not the
# working directory: cibuildwheel copies its whole directory into the
# container, and host venvs or objects built against the host's C library
# must not go along.  The stage holds the tracked files and untracked ones
# that aren't git-ignored, as they are in the working tree, and the das2C
# submodule as committed.
set -e -o pipefail

if [ $# -ne 1 ]; then
	echo "usage: $0 OUT_DIR" >&2
	exit 2
fi

TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP"

OUT=$(mkdir -p "$1" && cd "$1" && pwd)
STAGE=$TOP/build.pypi/stage
ENGINE=${ENGINE:-podman}
IMAGE=${IMAGE:-quay.io/pypa/manylinux_2_28_x86_64}

if [ -z "$CIBW" ] || [ ! -x "$CIBW" ]; then
	echo "CIBW must name a cibuildwheel executable (make pypi sets it)" >&2
	exit 3
fi

# Wheels that go to PyPI should be reproducible from a commit
if [ "$PYPI_DIRTY" != "yes" ] && [ -n "$(git status --porcelain)" ]; then
	echo "The working tree has uncommitted changes:" >&2
	git status --short >&2
	echo "Commit first, or set PYPI_DIRTY=yes for a test build." >&2
	exit 4
fi
if [ -n "$(git -C deps/das2C status --porcelain)" ]; then
	echo "deps/das2C has uncommitted changes, the build would not include them" >&2
	exit 4
fi

echo "Building PyPI wheels from $(git describe --always --dirty) into $OUT"

stage() {
	rm -rf "$STAGE"
	mkdir -p "$STAGE/deps/das2C"
	git ls-files -z -c -o --exclude-standard | grep -z -v '^deps/das2C$' \
		| tar --null -T - -c | tar -x -C "$STAGE"
	git -C deps/das2C archive HEAD | tar -x -C "$STAGE/deps/das2C"
}

# Only wheels are removed, so a mistaken OUT_DIR loses nothing else
rm -f "$OUT"/*.whl

# Python 3.  cibuildwheel takes its settings from pyproject.toml.
stage
(
	cd "$STAGE"
	set -x
	CIBW_CONTAINER_ENGINE=$ENGINE "$CIBW" --platform linux --output-dir "$OUT"
)

# Python 2.7.  Staged again: this build writes into its tree.
stage
(
	set -x
	$ENGINE run --rm --security-opt label=disable \
		-v "$STAGE":/project -v "$OUT":/output \
		"$IMAGE" bash /project/buildfiles/build_cp27mu.sh
)

rm -rf "$STAGE"
echo "pypi_wheels.sh: wheels in $OUT"
ls -l "$OUT"
