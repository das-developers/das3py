from setuptools import Extension, setup
import os
import shutil
from os.path import dirname as dname
from os.path import basename as bname
from os.path import join as pjoin
# import numpy  # <-- forced to be hacky with custom build_ext
from setuptools.command.build_ext import build_ext as _build_ext
import sys

# Unless told otherwise das2C taken from the submodule in deps/das2C.
# N_ARCH is used to separate operation systems that have libc differences.
# Windows is rather stable and ignores N_ARCH
# CMake builds need DAS_LIBDIR.
#
# The path is taken from this file's location, not the current directory.
# It only exists in a git clone: an sdist doesn't carry das2C, and
# 'python -m build' compiles in an unpacked sdist, so there DAS_LIBDIR and
# DAS_INCDIR have to be set.
sHere = os.path.dirname(os.path.abspath(__file__))
sSubMod = pjoin(sHere, 'deps', 'das2C')

sCLibDir = os.getenv("DAS_LIBDIR")
if not sCLibDir:
	if sys.platform == 'win32':
		sCLibDir = pjoin(sSubMod, "build.windows")
	else:
		sCLibDir = pjoin(sSubMod, "build.%s"%os.getenv("N_ARCH", ""))

sCHdrDir = os.getenv("DAS_INCDIR")
if not sCHdrDir:
	sCHdrDir = sSubMod

print('(setup.py) DAS_LIBDIR = %s'%sCLibDir)
print('(setup.py) DAS_INCDIR = %s'%sCHdrDir)

# Package is not complete without the CDF shared library, include it.  Only
# take it from das2C, which copies up the one matching the CDF its programs
# link, so pycdf and das3_cdf can't disagree about the CDF version.

dName = {
	'win32':'dllcdf.dll','linux':'libcdf.so','linux2':'libcdf.so',
	'darwin': 'libcdf.dylib'
}
sCdfSo = pjoin(sCLibDir, dName[sys.platform])

if os.path.isfile(sCdfSo):
	print('(setup.py) Adding %s to module'%sCdfSo)
	shutil.copy2(sCdfSo, pjoin('.','das3','pycdf'))
else:
	print('(setup.py) ERROR: %s missing.'%sCdfSo)
	print('(setup.py)   das2C has to be built first, with CDF support.  In a git')
	print('(setup.py)   checkout "make" does that for the das2C in deps/das2C (if that')
	print('(setup.py)   directory is empty: git submodule update --init).  For a das2C')
	print('(setup.py)   kept elsewhere set DAS_INCDIR and DAS_LIBDIR.')
	sys.exit(3)


# Under conda we can dependend on shared objects, under system builds we can't
if os.getenv("CONDA_PREFIX"):
	bLinkStatic=False
else:
	bLinkStatic=True

# The oldest numpy C API the das3py may assume, and so the oldest numpy
# it will import under.  
# 
# 1.19 is the first numpy for python 3.9, our oldest python 3 wheel.  Keep
# it matching the numpy floor in pyproject.toml and install_requires below.
#
# numpy before 1.25 doesn't read the macro (2.7 builds against 1.16.6, the
# from-git 3.6 to 3.8 builds against a 1.x), so it is ignored there: those
# builds' floor is the numpy they compiled against.
lDefs = [('NPY_TARGET_VERSION', 'NPY_1_19_API_VERSION')]

# DAS_DFT says whether the das2C being linked has its transform code,
# das3/dft.c, and so needs the FFTW library.  It has to match how that das2C
# was built: DFT=no in das2C's makefiles goes with DAS_DFT=no here, and a
# mismatch either way stops at the link with unresolved names.
#
# With no, _das3 has no Dft and Psd types, das3 uses the numpy ones in
# das3/dft.py, and FFTW stays out of the wheel.  FFTW is under the GPL, and
# a wheel that contains it is distributed under the GPL's terms.
#
# The default is no for the static Windows build, which is where the PyPI
# packages for Windows come from, and yes everywhere else.
sDft = os.getenv("DAS_DFT")
if sDft:
	bDft = (sDft.lower() == 'yes')
else:
	bDft = not (sys.platform == 'win32' and bLinkStatic)
print('(setup.py) DAS_DFT = %s'%('yes' if bDft else 'no'))
if not bDft:
	lDefs.append( ('DAS_NO_DFT', None) )

# License texts for the code compiled into the wheel, as pairs of (file,
# name to give it in LICENSES/).  THIRD-PARTY.txt says what each is for.
# A wheel is a binary distribution of all of it, so the texts travel with
# the wheel, in its dist-info/licenses directory.
#
# pycdf's is copied like the rest, not listed by its path in the package:
# setuptools takes a license file inside das3/pycdf for a data file of das3
# and then warns that das3.pycdf is not a configured package, which it is.
#
# das2C's own are in its source tree, or beside include/ in one of its
# binary packages.  A das2C installed without them (headers in /usr/include)
# gives a wheel without them, which is fine for local use.
lLicSrc = [ (pjoin(sHere, 'das3', 'pycdf', 'LICENSE.md'), 'pycdf.txt') ]
for sDir in (sCHdrDir, dname(sCHdrDir)):
	lTop = [s for s in ('LICENSE', 'LICENSE.txt') if os.path.isfile(pjoin(sDir, s))]
	if lTop and os.path.isdir(pjoin(sDir, 'LICENSES')):
		lLicSrc.append( (pjoin(sDir, lTop[0]), 'das2C.txt') )
		for s in sorted(os.listdir(pjoin(sDir, 'LICENSES'))):
			# Code that is only compiled into das2C on Windows
			if s in ('dirent.txt', 'wcwidth.txt') and sys.platform != 'win32':
				continue
			if s.endswith('.txt'):
				lLicSrc.append( (pjoin(sDir, 'LICENSES', s), s) )
		break

if sCLibDir: lLibDirs = [sCLibDir]
else: lLibDirs = []

if sCHdrDir: lInc = [ sCHdrDir ]
else: lInc = [ ]


lSrc = ["src/_das3.c"]

if sys.platform == 'win32':

	if bLinkStatic:  # currently a synonym for not-conda 
		# A windows hack, since we're not allowed to import numpy
		# inside setup.py.  Any help on gitting rid of this hack
		# would be highly appreciated
		#sPyDir = dname(sys.executable)
		#lInc.append(
		#	pjoin(sPyDir, "lib", "site-packages", "numpy", "core", "include")
		#)

		# das2C's nmake build (buildfiles\windows.mak) has vcpkg put expat,
		# fftw3, openssl, pthreads and zlib under its build directory, as
		# static libraries compiled for the DLL C runtime (/MD).  That is
		# the runtime python extensions use, so these can be linked into
		# _das3 and the wheel needs no DLLs of its own besides the CDF one.
		# The fully static triplet, x64-windows-static, is /MT and would
		# put a second C runtime in the process.
		#
		# DAS_DEPS_LIBDIR names some other directory holding them.
		sDepLibDir = os.getenv("DAS_DEPS_LIBDIR")
		if not sDepLibDir:
			sDepLibDir = pjoin(
				sCLibDir, "vcpkg_installed", "x64-windows-static-md", "lib"
			)
		print('(setup.py) DAS_DEPS_LIBDIR = %s'%sDepLibDir)

		# Given by full path, not by name, so that a library of the same
		# name elsewhere on the linker's search path can't be taken in
		# place of the one das2C was compiled against.  The expat and zlib
		# file names changed between vcpkg releases, take the first found.
		lExObjs = [pjoin(sCLibDir, 'libdas3.lib')]
		lDeps = [
			("libexpatMD.lib", "libexpat.lib"), ("libssl.lib",),
			("libcrypto.lib",), ("zs.lib", "zlib.lib"), ("pthreadVC3.lib",)
		]
		lDepPkgs = ["expat", "openssl", "pthreads", "zlib"]
		sDftArg = ' DFT=no'
		if bDft:
			lDeps.append( ("fftw3.lib",) )
			lDepPkgs.append("fftw3")
			sDftArg = ''

		for tNames in lDeps:
			lFound = [
				pjoin(sDepLibDir, s) for s in tNames
				if os.path.isfile(pjoin(sDepLibDir, s))
			]
			if not lFound:
				print('(setup.py) ERROR: %s missing from %s'%(
					' or '.join(tNames), sDepLibDir
				))
				print('(setup.py)   Build das2C first, in deps\\das2C after vcvars64.bat:')
				print('(setup.py)     nmake /f buildfiles\\windows.mak CDF=yes%s build_dep'%sDftArg)
				print('(setup.py)     nmake /f buildfiles\\windows.mak CDF=yes%s static'%sDftArg)
				sys.exit(3)
			lExObjs.append(lFound[0])

		# The license text of each one, since they become part of _das3.
		# vcpkg installs it as share/<package>/copyright.  Taken from the
		# same tree as the libraries so that the text matches the version
		# linked.  A das2C binary package already has these in its LICENSES
		# directory, under the same names.  vcpkg installs fftw3 whether it
		# is linked or not, so its text is taken only when it is.
		sShare = pjoin(dname(sDepLibDir), "share")
		for s in lDepPkgs:
			if (s + '.txt') not in [t[1] for t in lLicSrc]:
				lLicSrc.append( (pjoin(sShare, s, "copyright"), s + '.txt') )
		if not bDft:
			lLicSrc = [t for t in lLicSrc if t[1] != 'fftw3.txt']

		# Windows libraries that OpenSSL and das2C's sockets call
		lLibs = ["Advapi32", "User32", "Crypt32", "ws2_32"]
	else:
		# Do as much static linking as you can. Removes run-time dependencies
		lTmp = [ 
			"libdas3", "fftw3", "expat", "libssl", "libcrypto", "zlib", 
			"libpthreadVC3"
		]
		if not bDft: lTmp.remove("fftw3")
		lExObjs = [ '%s/%s.lib'%(sCLibDir, s) for s in lTmp ]
		lLibs = ["ws2_32"]

	print("(setup.py) Using Headers from %s"%lInc)
	print("(setup.py) Using Libs from %s"%lLibDirs)

	ext = Extension(
		"_das3"
		,sources=lSrc
		,include_dirs=lInc
		,define_macros=lDefs
		,library_dirs=lLibDirs
		,libraries=lLibs
		,extra_objects=lExObjs
	)
elif sys.platform == 'darwin':

	# A macOS hack
	#lInc.append(
	#	"/opt/homebrew/lib/python%d.%d/site-packages/numpy/core/include"%(
	#		sys.version_info.major, sys.version_info.minor
	#	)
	#)

	if bLinkStatic: 
		# Try to find homebrew openssl
		sBrewDir=None
		for sDir in ("/opt/homebrew","/usr/local"):
			if os.path.isdir(pjoin(sDir, "opt/openssl")):
				sBrewDir = sDir
				break
		if not sBrewDir:
			print("Error: Failed to find homebrew openssl")
			sys.exit(7)

		lExObjs = [
			'%s/libdas3.a'%sCLibDir,
			'%s/opt/openssl/lib/libssl.a'%sBrewDir,
			'%s/opt/openssl/lib/libcrypto.a'%sBrewDir,
		]
		if bDft: lExObjs.append('%s/lib/libfftw3.a'%sBrewDir)
		lLibs = ["expat", "z"]
	else:
		lExObjs = ['%s/libdas3.a'%sCLibDir]
		lLibs   = ["fftw3", "expat", "ssl", "crypto", "z"]
		if not bDft: lLibs.remove("fftw3")


	ext = Extension(
		"_das3", sources=lSrc 
		,include_dirs=lInc
		,define_macros=lDefs
		,library_dirs=lLibDirs
		,libraries=lLibs
		,extra_compile_args=['-std=c99', '-ggdb', '-O0']
		,extra_objects=lExObjs
		,extra_link_args=['-Wl,-no_compact_unwind']
	)	
else:
	# Linux, also works for anaconda on macos
	lLibs = ["fftw3", "expat", "ssl", "crypto", "z"]
	if not bDft: lLibs.remove("fftw3")
	lExObjs = ['%s/libdas3.a'%sCLibDir]
	lLinkArgs = []

	# DAS_EXPAT_LIB, the path of a libexpat.a built with -fPIC, links expat
	# into the extension.  The manylinux wheels need this: auditwheel counts
	# libexpat.so.1 as a system library and won't bundle it, but slim and
	# distroless images don't have it, so import fails.
	#
	# --exclude-libs keeps its symbols local to _das3. Note expat's symbols
	# aren't versioned, so when the process already has libexpat.so.1 loaded
	# (a distro python links one for pyexpat) the dynamic linker would
	# otherwise bind our calls to that copy and not to the version we built
	# locally.
	sExpatA = os.getenv("DAS_EXPAT_LIB")
	if sExpatA:
		if not os.path.isfile(sExpatA):
			print('(setup.py) ERROR: DAS_EXPAT_LIB=%s does not exist'%sExpatA)
			sys.exit(3)
		print('(setup.py) Linking expat statically from %s'%sExpatA)
		lLibs.remove("expat")
		lExObjs.append(sExpatA)
		lLinkArgs.append('-Wl,--exclude-libs,%s'%bname(sExpatA))

	ext = Extension(
		"_das3", sources=lSrc
		,include_dirs=lInc
		,define_macros=lDefs
		,library_dirs=lLibDirs
		,libraries=lLibs
		,extra_compile_args=['-std=c99', '-ggdb', '-O0']
		,extra_objects=lExObjs
		,extra_link_args=lLinkArgs
	)

# Collect the license texts in LICENSES/, made new on each build so that it
# can't keep a text from an earlier build against other libraries.  It has
# to be inside the source tree: setuptools only takes license files from
# there, and stores them under the same relative path.
sLicDir = pjoin(sHere, 'LICENSES')
if os.path.isdir(sLicDir):
	shutil.rmtree(sLicDir)
os.mkdir(sLicDir)
for (sSrc, sName) in lLicSrc:
	if not os.path.isfile(sSrc):
		print('(setup.py) ERROR: license text %s missing.'%sSrc)
		print('(setup.py)   The library it covers is linked into _das3, so the wheel')
		print('(setup.py)   has to carry it.')
		sys.exit(3)
	shutil.copyfile(sSrc, pjoin(sLicDir, sName))
print('(setup.py) License texts for the wheel: %s'%', '.join(
	[t[1] for t in lLicSrc]
))

lLicFiles = ['LICENSE', 'THIRD-PARTY.txt', 'LICENSES/*.txt']

# ... because import numpy at top level doesn't work anymore (yay)
# Solution from:
# https://stackoverflow.com/questions/54117786/add-numpy-get-include-argument-to-setuptools-without-preinstalled-numpy
class build_ext(_build_ext):
	def finalize_options(self):
		_build_ext.finalize_options(self)
		# Prevent numpy from thinking it is still in its setup process:
		try:
			__builtins__.__NUMPY_SETUP__ = False
		except AttributeError:
			pass # Newer numpy versions don't define this
		import numpy
		print("===================================")
		print("   Numpy Build Version is %s"%numpy.__version__)
		print("===================================")

		# Force numpy include to the front, might be some old version
		# backed by the system python that is there too.  Ignore it.
		self.include_dirs.insert(0, numpy.get_include())


# The project description for python 2 builds.  setuptools 44, the last for
# 2.7, ignores pyproject.toml's [project] table, and without these the 2.7
# wheel's metadata says UNKNOWN for its summary, page text and license.
# That is more than cosmetic: PyPI takes a release's description from the
# first file uploaded for it, and cp27 sorts first.  Python 3 builds read
# [project], and newer setuptools objects to the same fields given here as
# well, so these are passed for python 2 only.  Keep them in step with
# pyproject.toml.
dPy2Meta = {}
if sys.version_info[0] < 3:
	import io
	with io.open(pjoin(sHere, 'README.md'), encoding='utf-8') as fIn:
		sReadme = fIn.read()
	dPy2Meta = dict(
		description="An efficient space physics data client for python",
		long_description=sReadme,
		long_description_content_type="text/markdown",
		license="MIT",
		python_requires=">=2.7, !=3.0.*, !=3.1.*, !=3.2.*, !=3.3.*, !=3.4.*, !=3.5.*",
		keywords=["space-physics", "magnetosphere", "das2", "das3"],
		project_urls={
			"Repository": "https://github.com/das-developers/das3py",
			"Changelog": "https://github.com/das-developers/das3py/blob/main/CHANGELOG.md",
		},
	)
else:
	# The license texts gathered above.  Python 3 only: setuptools 44 doesn't
	# know this keyword, so the 2.7 wheel carries das3py's LICENSE alone.
	# pyproject.toml leaves license-files out so that this list is used.
	dPy2Meta = dict(license_files=lLicFiles)

setup(
	cmdclass={'build_ext':build_ext},
	name="das3py",
	version="3.0.7",
	ext_modules=[ext],
	packages=['das3', 'das3.pycdf'],
	author="C Piker",
	author_email="das-developers@uiowa.edu",
	url="https://github.com/das-developers/das3py",
	# Same commands as [project.scripts] in pyproject.toml, which the
	# setuptools releases for python 2.7 don't read
	entry_points={'console_scripts': [
		'das_verify = das3.verify:main',
		'das_cdfinfo = das3.cdfinfo:main',
	]},
	include_package_data=True,
	#package_data={'das2':['xsd/*.xsd']}, # <-- in das2C now
	package_data={'das3.pycdf':[bname(sCdfSo)]},
	# Same as dependencies in pyproject.toml.  The 2.7 wheel's metadata comes
	# from here: setuptools 44, the last for 2.7, ignores [project].
	install_requires=[
		'lxml',
		'numpy>=1.19.3; python_version>="3.9"',
		'numpy>=1.16.6; python_version<"3.9"',
	],
	**dPy2Meta
)
