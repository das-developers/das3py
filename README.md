# das3py
Das servers typically provide data relevant to space plasma and magnetospheric
physics research. To retrieve data, an HTTP GET request is posted to a das
server by a client program and a self-describing stream of data values covering
the requested time range, at the requested time resolution, is provided in the
response body. This package, *das3py*, provides an efficient space physics data
client for python.  It reads both das2 and das3 streams, and das2 streams
will be supported indefinitely.  Streams are parsed and stored as NumPy
arrays using a C extension, avoiding data copies and conversions.

das3py was published as *das2py* through version 3.0.4.  See *Upgrading
from das2py* below.

## Installing from PyPI

```bash
python -m pip install das3py
```

The PyPI packages are pre-built for Linux on x86_64, for Python 2.7 and
3.9 through 3.15.  They need glibc 2.26 or newer (RHEL 8, Ubuntu 18.04
and later) and pip 20.3 or newer.  The 2.7 package is for UCS-4 builds of
Python, as Linux distributions ship; a 2.7 built from source needs
`--enable-unicode=ucs4`.  If pip is older, upgrade it first:

```bash
python -m pip install -U pip
```

On other Linux systems, build from source as described in *Building and
Installing from Source on Linux* below.  For Windows and macOS, see
*Windows and macOS*.

To check the install, run one of the examples.  It needs matplotlib and
an internet connection:

```bash
python -m pip install matplotlib
wget https://raw.githubusercontent.com/das-developers/das3py/main/examples/ex02_galileo_pws_spectra.py
python ex02_galileo_pws_spectra.py
```

It writes `ex02_galileo_pws_spectra.png`, which should look like this:

<img src="https://raw.githubusercontent.com/das-developers/das3py/main/examples/ex02_galileo_pws_spectra.png" width="660" height="379">

## First program

The following small program demonstrates how to query for data and generate a plot 
using das3py.

### Query a URI for reduced resolution data
```python
import das3
src = das3.get_source( 'tag:das2.org,2012:site:/uiowa/cassini/rpws/survey_keyparam/das2' )
header, datasets = src.get( {'time' : ('2017-09-02', '2017-09-03')} )
```
  * `get()` returns the stream header and a list of datasets.  This
    source sends one: a day of Cassini RPWS electric field spectra, one
    spectrum a minute at 73 frequencies.
  * `print(src.info())` lists the query parameters a source takes.
  * `site:/...` is short for `tag:das2.org,2012:site:/...`.
  * Servers come and go.  The federated catalog provides stability for 
    application code by mapping URIs to data sources.
  * Browse for URIs using a [das2 catalog](https://das2.org/browse) browser,
    or using das3py to get the root node (see [example 11](https://raw.githubusercontent.com/das-developers/das3py/main/examples/ex11_catalog_listings.py)).

### Access quantities by physical dimensions
```python
dataset = datasets[0]
print(dataset)

vX = dataset['time']['center']
vY = dataset['frequency']['center']
vZ = dataset['electric_specdens']['center']
```
  * Datasets contain dimensions. Dimensions contain variables. Each variable in a 
    dimension serves a purpose, the most common is to define the center point of a
    coordinate or measurement. 
  * Array dimensions are not confused with physical dimensions. Dataset meaning is
    not tied to any particular array morphology.

### Use Matplotlib to generate an image
```python
import das3.mpl   # das2 label formatting for matplotlib
import matplotlib.pyplot as pyplot
import matplotlib.colors as colors

fig, ax = pyplot.subplots()
im = ax.pcolormesh(vX.array, vY.array, vZ.array, norm=colors.LogNorm(), cmap='jet')
ax.set_yscale('log')
fig.colorbar(im, label=das3.mpl.label(dataset['electric_specdens'].props['label']))
fig.autofmt_xdate()

ax.set_title(das3.mpl.label(header['props']['title']))
ax.set_xlabel(das3.mpl.range_label(dataset['time']))
ax.set_ylabel(das3.mpl.label(dataset['frequency'].props['label']))
pyplot.show()
```
  * The title and axis labels come from the stream.  `das3.mpl.label()`
    converts das2 formatting codes, such as `!a` for superscript, to
    matplotlib's, and `das3.mpl.range_label()` describes the time range.
  * Times arrive as NumPy `datetime64` values, which matplotlib plots as
    dates.
  * Fill values arrive masked: `vZ.array` is a NumPy masked array.

### Vectors and other composites
das3 streams can carry vectors, rotation matrices and other multi-part
values as a single variable.  This query gets eight minutes of TRACERS-2
magnetometer data:
```python
src = das3.get_source('site:/uiowa/tracers/tracers-2/l2/mag_16sps_delta')
header, datasets = src.get( {'time' : ('2026-05-04T01:29:59', '2026-05-04T01:38:07')} )

dB = datasets[0]['ts2_l2_mag_16sps_fac_deltab']['center']

print(dB.array.shape)   # (7808, 3)
print(dB.extShape())    # (7808,)  one value per record
print(dB.intrShape())   # (3,)     each value is a 3-vector
print(dB.labels)        # ['dBx', 'dBy', 'dBz']
print(dB.ops)           # {'kind': 'vector', 'frame': 'TS2_FAC', 'system': 'cartesian', ...}
```
  * The external shape counts records in the dataset.  The internal shape
    is the shape of each value.
  * `dB.ops` says what the values are and which coordinate frame they
    are in.
  * The array is ordinary NumPy, so the field magnitude is
    `numpy.linalg.norm(dB.array, axis=1)`.
  * [Example 12](https://raw.githubusercontent.com/das-developers/das3py/main/examples/ex12_tracers_mag_delta.py)
    plots the three components.

## Anaconda Package

A das3py conda package for Linux, Windows and macOS is planned.  The
dasdevelopers channel still has the older das2py 3.0.4.

## What changed in 3.0

The short version, for people upgrading from 2.3:

* **The package is `das3` now.**  It reads das3 streams as well as das2
  streams. Code that says `import das2` needs to say `import das3`.  The
  pip name is now `das3py`.  Python 2.7 and 3.x are both supported,
  `das3` isn't a reference to the interpreter version.

* **Reads return the stream header too.**  `Source.get()` and the
  `read_*()` functions return a tuple, `(stream_header, datasets)`, not
  just a list of datasets, so their call sites change.

* **Composite values.**  Vectors, rotation matrices, quaternions and complex
  pairs come through as one variable, with an internal shape.  So a
  vector over N records is an (N, 3) array with the external shape (N,) in
  the dataset, and the internal shape (3,) within each value.  These
  can be retrieved via `var.extShape()` and `var.intrShape()`.

* **Mathematical Formalisms.**  Each variable is now tagged with metadata
  denoting which math operations it supports.  For example a `point`
  variable supports subtraction (a point - point = length) but not
  addition.  The dict `var.ops` provides information on operations.

* **Raw Storage.**  To get the compact raw storage for a variable, use
  `var.storage()`, which gives the values without the dataset's broadcast.

* **Sequences are materialized.**  A coordinate defined by a `<sequence>` in
  the stream header arrives as a real array.

* **Tidier names.**  `dir(das3)` shows the public API and nothing else.  A
  few methods were renamed to match the rest: `DasTime.fromString`,
  `DasTime.roundDoy`, `Dimension.hasProp` and `hasPropVal` (replacing
  `propEq`) and `Source.infoProto`.  The 2.3 names still work
  and warn.  `Quantity.to_value` keeps its
  snake_case on purpose, since it's source compatible with AstroPy's 
  Quantities.

* **das_verify checks more.**  Index counts against the dataset rank, one
  `units=` per composite, and `<sequence>` counts against the internal
  shape, none of which an XML schema can express.

* **Not yet:** ragged arrays (variable length records) still don't convert
  to ndarrays.  das2C reads them fully, but das3py stops at the conversion.

## Upgrading from das2py

As of version 3.0 the package is `das3`, named for the das3 stream format
it added, and as of 3.0.6 it is distributed as `das3py`.  It still reads
das2 streams, and runs on Python 2.7 and 3.x alike.

Most das2py code needs two changes:

1. **The module name.**  `import das2` becomes `import das3`, and
   `das2.DasTime` becomes `das3.DasTime`, and so on.  Don't change every
   "das2" in a file: catalog names such as
   `'tag:das2.org,2012:site:/...'` and das2.org URLs stay as they are.

2. **Reading data returns the stream header too.**
   ```python
   ds_list = src.get()                 # das2py 2.3
   stream_hdr, ds_list = src.get()     # das3py
   ```
   The same goes for `read_file()`, `read_http()` and `read_cmd()`, which
   now raise an exception on failure instead of returning `None`.

Code written for the das2py 3.0.4 conda package needs only the first
change.

A few 2.3 method names changed too.  The old names still work, with a
`PendingDeprecationWarning` that Python hides by default; see the
[CHANGELOG](https://github.com/das-developers/das3py/blob/main/CHANGELOG.md).  To find them, run a script with the warning promoted to an
error:

```bash
python -W error::PendingDeprecationWarning your_script.py
```

Data sources in the das catalog have also changed their query parameters
since 2.3.  `print(src.info())` lists the ones a source takes now.

Note that das2py and das3py both include the `das_verify` command.  If
you uninstall das2py, reinstall das3py to replace it:

```bash
python -m pip install --force-reinstall --no-deps das3py
```

The old das2py releases stay on PyPI, so scripts that pin `das2py<3` keep
working.

## Building and Installing from Source on Linux

Compilation and installation of das3py is regularly tested on Linux under
both Python 2 (still) and Python 3. The following packages are required
to build das3py and all dependencies:

1. Get system software packages.
   
   Red Hat Enterprise Linux or compatible (Rocky, Alma)
   ```bash
   dnf install git gcc make curl expat-devel fftw-devel openssl-devel python3-devel
   ```
   Debian or compatible (Ubuntu)
   ```bash
   apt install gcc git make curl libexpat-dev libfftw3-dev libssl-dev python3-dev python3-venv
   ```

2. Get the source, along with the pegged das2C version.
   ```bash
   git clone --recursive https://github.com/das-developers/das3py.git
   ```
   das2C is a git submodule in `deps/das2C`, pegged to the das2C release
   this das3py was written and tested against.  If you cloned without
   `--recursive` that directory is empty, so fill it with
   `git submodule update --init`.

3. Build and test sources against a *specific* version of python. The
   makefile will automatically find python if `PY_BIN` is not defined,
   but providing an explicit path avoids confusion down the road.  For
   old Python2 builds, PY_BIN is required, as Python3 will be built by
   default.

   ```bash
   cd das3py
   make PY_BIN=/path/to/python           # See PY_BIN note above
   make PY_BIN=/path/to/python test      # optional, but recommended
   make PY_BIN=/path/to/python examples  # optional, needs network access
   ```

   The first `make` also builds the bundled das2C, which downloads the
   sources of NASA's CDF library.  To use a CDF you already have, set
   `CDF_INC` and `CDF_LIB` as das2C's own build notes describe.

   To build against some other das2C, a development checkout for example,
   build that one yourself and reference it.  das3py does not alter an
   external das2C build, it just uses it.
   ```bash
   make DAS2C=/path/to/das2C PY_BIN=/path/to/python
   ```

4. Install into your desired python environment.
   ```bash
   /path/to/python -m pip install ./dist/das3py-*.whl

   # or
   
   make PY_BIN=/path/to/python install
   ```

The PyPI packages are built in a manylinux container with `make pypi`.
The Makefile explains what that needs.

## Windows and macOS

das3py has been built on Windows and macOS before, and `setup.py` has
branches for both, but this release has not been tested on either.  If
you need it there, please ask on the issue tracker.

## Reporting bugs
Please use the github.com [issue tracker](https://github.com/das-developers/das3py/issues) 
to report any problems with the library.  If you've fixed a bug, thanks!  Please send
a pull request so that your fix can be merged into the main project.




