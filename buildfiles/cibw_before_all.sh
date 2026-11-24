#!/bin/bash
# cibuildwheel before-all for the manylinux images: runs once per container,
# before any python version is built.  Leaves a static expat in /usr/local
# and das2C built in deps/das2C/build. where setup.py looks for it.
#
# das2C does not depend on the python version, so building it here, once,
# serves every wheel built in this container.
#
# Environment:
#
#   EXPAT_URL  where to get the expat tarball, default the upstream release.
#              The department mirror has the same file:
#              https://space.physics.uiowa.edu/spdr/packages/source/expat-2.11.0.tar.gz
#
# The sha256 below is checked whichever URL is used.
set -e -x -o pipefail

EXPAT_VER=2.11.0
EXPAT_SHA256=b3e3fcbb629e4d357a9cc0af5b146b85959bcf1cefe87e529d2c0a9234be737f
EXPAT_DIST=expat-$EXPAT_VER
EXPAT_URL=${EXPAT_URL:-https://github.com/libexpat/libexpat/releases/download/R_${EXPAT_VER//./_}/$EXPAT_DIST.tar.gz}

TOP=$(cd "$(dirname "$0")/.." && pwd)

if [ ! -f "$TOP/deps/das2C/Makefile" ]; then
	echo "deps/das2C is empty, the checkout needs submodules" >&2
	exit 3
fi

cat /etc/redhat-release
gcc --version | head -1

# No expat-devel, on purpose.  Without it the only expat.h is the one in
# /usr/local/include and the only libexpat to link is the static one, so a
# build that strays to the system expat fails instead of quietly linking
# the shared library auditwheel won't bundle.
dnf install -y fftw-devel openssl-devel zlib-devel

# Expat: static only, -fPIC because it ends up inside the extension module.
# xmlwf, docs, examples and tests are not needed and xmlwf's docs want
# docbook.
WORK=$(mktemp -d)
curl -f -L "$EXPAT_URL" > "$WORK/$EXPAT_DIST.tar.gz"
echo "$EXPAT_SHA256  $WORK/$EXPAT_DIST.tar.gz" | sha256sum -c -
tar -xzf "$WORK/$EXPAT_DIST.tar.gz" -C "$WORK"
(
	cd "$WORK/$EXPAT_DIST"
	./configure --prefix=/usr/local --enable-static --disable-shared \
		--with-pic --without-xmlwf --without-docbook --without-examples \
		--without-tests
	make
	make install
)
rm -rf "$WORK"

# das2C.  distclean first: a local cibuildwheel run copies the working tree
# into the container, and objects or a CDF build left there by a host build
# are linked against the host's C library.  The CDF sources are fetched
# again from spdf.gsfc.nasa.gov as a result.
#
# das2C's own programs say -lexpat; LIBRARY_PATH lets gcc find the static
# one in /usr/local/lib.  Not LDFLAGS=... on the make command line: command
# line variables are passed down to sub-makes and override what CDF's
# makefile sets, which loses the flags that make libcdf.so a shared library.
unset N_ARCH
export LIBRARY_PATH=/usr/local/lib
make -C "$TOP/deps/das2C" distclean
make -C "$TOP/deps/das2C" SPICE=no CDF=yes

# setuptools' build cache, for the same reason as the distclean
rm -rf "$TOP/build"
