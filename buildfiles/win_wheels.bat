@echo off
rem Builds the Windows wheels that go to PyPI, CPython 3.9 to 3.13 for x64.
rem The counterpart of pypi_wheels.sh, which builds the Linux ones.
rem
rem   buildfiles\win_wheels.bat [OUT_DIR]       OUT_DIR defaults to dist\pypi
rem
rem Needs:
rem
rem   Visual Studio's C compiler and nmake.  Run this after vcvars64.bat, or
rem   let it find the newest Visual Studio itself.
rem
rem   VCPKG_ROOT set to a vcpkg clone that has the baseline commit named in
rem   deps\das2C\vcpkg.json.  das2C's makefile has vcpkg build expat, fftw3,
rem   openssl, pthreads and zlib from it.
rem
rem   Git for Windows.  Its bash runs test\wheel_tests.sh.
rem
rem   A python 3.11 or newer, to run cibuildwheel.  The pythons the wheels
rem   are built for are downloaded by cibuildwheel itself.
rem
rem Environment:
rem
rem   PY_BIN       the python that runs cibuildwheel, default: py -3
rem   PYPI_DIRTY   yes to build with uncommitted changes in the tree
rem
rem Windows wheels already in OUT_DIR are deleted first, so afterwards it
rem holds exactly the ones this run built.  Wheels for other systems that
rem were copied there for an upload are left alone.

setlocal
cd /d "%~dp0.."

set "OUT=%~1"
if "%OUT%"=="" set "OUT=dist\pypi"
if "%PY_BIN%"=="" set "PY_BIN=py -3"

rem The Microsoft Store python won't do.  Windows redirects what a Store
rem program writes under AppData into a folder private to that program.
rem cibuildwheel keeps nuget.exe there, and started from that folder nuget
rem fails with a .NET error box instead of fetching the pythons.
%PY_BIN% -c "import sys; sys.exit(7 if 'WindowsApps' in sys.executable else 0)"
if %ERRORLEVEL%==7 (
	echo ERROR: %PY_BIN% is the Microsoft Store python, which can't run cibuildwheel.
	echo        Set PY_BIN to a python 3.11 or newer from python.org or conda.
	exit /b 3
)
if errorlevel 1 (
	echo ERROR: %PY_BIN% doesn't run.  Set PY_BIN to a python 3.11 or newer.
	exit /b 3
)

if not exist deps\das2C\buildfiles\windows.mak (
	echo ERROR: deps\das2C is empty.  Fetch the pinned das2C with:
	echo           git submodule update --init
	exit /b 3
)

rem Wheels that go to PyPI should be reproducible from a commit
set "DIRTY="
for /f "delims=" %%i in ('git status --porcelain') do set "DIRTY=1"
for /f "delims=" %%i in ('git -C deps\das2C status --porcelain') do set "DIRTY=1"
if defined DIRTY if /i not "%PYPI_DIRTY%"=="yes" (
	echo The working tree or deps\das2C has uncommitted changes:
	git status --short
	git -C deps\das2C status --short
	echo Commit first, or set PYPI_DIRTY=yes for a test build.
	exit /b 4
)

rem The compiler.  vcvars64.bat only lasts for one cmd session, so it is
rem called here when this window doesn't have it yet.
where nmake >nul 2>&1
if errorlevel 1 (
	for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
where nmake >nul 2>&1
if errorlevel 1 (
	echo ERROR: nmake isn't on the PATH and no Visual Studio was found.
	echo        Run vcvars64.bat in this window first.
	exit /b 3
)

rem Git's own bash, ahead of everything else on the PATH.  A plain 'bash'
rem may otherwise be the Windows Subsystem for Linux launcher in System32,
rem which would run the tests with some Linux python instead.
set "GIT_EXE="
for /f "delims=" %%i in ('where git 2^>nul') do if not defined GIT_EXE set "GIT_EXE=%%i"
if not defined GIT_EXE (
	echo ERROR: git isn't on the PATH.
	exit /b 3
)
for %%i in ("%GIT_EXE%") do set "GIT_BIN=%%~dpi..\bin"
if not exist "%GIT_BIN%\bash.exe" (
	echo ERROR: bash.exe not found in %GIT_BIN%, is this Git for Windows?
	exit /b 3
)
set "PATH=%GIT_BIN%;%PATH%"

echo Building Windows wheels into %OUT%
git describe --always --dirty

rem das2C: the static library, its dependencies and the CDF DLL.  None of
rem it depends on the python version, so once serves every wheel.  'clean'
rem first because nmake can't tell that a dependency was rebuilt, and a
rem library compiled against the previous one's headers would be linked.
rem It keeps the vcpkg packages and the CDF download.
rem
rem DFT=no leaves das2C's transform code out, and with it the FFTW library,
rem which is under the GPL.  setup.py expects that on Windows (see DAS_DFT
rem there), and das3 then computes power spectral density with numpy.
pushd deps\das2C
nmake /nologo /f buildfiles\windows.mak clean
nmake /nologo /f buildfiles\windows.mak CDF=yes DFT=no build_dep || goto :fail_das2c
nmake /nologo /f buildfiles\windows.mak CDF=yes DFT=no static || goto :fail_das2c
popd

rem cibuildwheel, pinned to the release line this was tested with.  Each
rem release also pins the exact pythons it downloads.
if not exist build_cibw_venv\Scripts\cibuildwheel.exe (
	%PY_BIN% -m venv build_cibw_venv || goto :fail
	build_cibw_venv\Scripts\python -m pip install "cibuildwheel>=4.3.5,<5" || goto :fail
)

rem setuptools compiles in .\build and only looks at the time on _das3.c,
rem so objects left from an earlier build could be linked stale
if exist build rmdir /s /q build
if not exist "%OUT%" mkdir "%OUT%"
del /q "%OUT%\*-win_amd64.whl" 2>nul

rem cibuildwheel takes its settings from pyproject.toml
build_cibw_venv\Scripts\cibuildwheel --platform windows --output-dir "%OUT%" || goto :fail

if exist build rmdir /s /q build
if exist das3py.egg-info rmdir /s /q das3py.egg-info

echo win_wheels.bat: wheels in %OUT%
dir /b "%OUT%\*-win_amd64.whl"
exit /b 0

:fail_das2c
popd
:fail
echo win_wheels.bat: FAILED
exit /b 1
