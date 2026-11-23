# das3py
Das servers typically provide data relevant to space plasma and magnetospheric
physics research. To retrieve data, an HTTP GET request is posted to a das
server by a client program and a self-describing stream of data values covering
the requested time range, at the requested time resolution, is provided in the
response body. This package, *das3py* provides an efficient space physics data
client for python.  Streams are parsed and stored as NumPy arrays using a C
extension, avoiding data copies and conversions.

das3py was published as *das2py* through version 3.0.4.  See *Upgrading
from das2py* below.

## Installing from PyPI

```bash
python -m pip install das3py
```

## Anaconda Package

[![Anaconda Package](https://anaconda.org/dasdevelopers/das3py/badges/version.svg)](https://anaconda.org/DasDevelopers/das3py)

## What changed in 3.0

The short version, for people upgrading from 2.3:

* **The package is `das3` now.**  It reads das3 streams, so it's named for
  them.  `import das2` keeps working as an alias, installed by the das2py
  transitional package, and points at the very same modules, so old and
  new code mix without a fuss.  The pip name is now
  `das3py`.  Python 2.7 and 3.x are both fine; `das3` says nothing about
  the interpreter.
* **Composite values.**  Vectors, rotation matrices, quaternions and complex
  pairs come through as one variable with a trailing internal shape, so a
  vector over N records is an (N, 3) array in a shape (N,) dataset.  Each
  variable carries its `<ops>` formalism as a dict (`var.ops`), one label
  per component (`var.labels`), and `var.extShape()` / `var.intrShape()`
  for the two halves of the shape, with `var.extRank` / `var.intrRank`
  counting them.  `var.storage()` gives the values without the dataset's
  broadcast.
* **Sequences are materialized.**  A coordinate defined by a `<sequence>` in
  the stream header arrives as a real array now instead of going missing.
* **Dimensions know what they are.**  `dim.kind` is `'coord'` or `'data'`,
  and `ds.coordKeys()` / `ds.dataKeys()` list one kind at a time.
* **Tidier names.**  `dir(das3)` shows the public API and nothing else.  A
  few methods were renamed to match the rest: `DasTime.fromString`,
  `DasTime.roundDoy`, `Dimension.hasProp` and `hasPropVal` (replacing
  `propEq`) and `Source.infoProto`.  The 2.3 names still work
  and warn, so old scripts keep running.  `Quantity.to_value` keeps its
  snake_case on purpose, since that's AstroPy's name for the same thing.
* **das_verify checks more.**  Index counts against the dataset rank, one
  `units=` per composite, and `<sequence>` counts against the internal
  shape, none of which an XML schema can express.
* **Not yet:** ragged arrays (variable length records) still don't convert
  to ndarrays.  das2C reads them; das3py stops at the conversion.

## Upgrading from das2py

As of version 3.0 the package is `das3`, named for the das3 stream format,
and as of 3.0.6 it is distributed as `das3py`.  It runs on Python 2.7 and
3.x alike.

The `das2py` name on PyPI is now a small transitional package.  Upgrading
it installs das3py and adds a `das2` alias, so `import das2` keeps working
and returns the very same modules as `import das3`.  das3py on its own does
not provide `import das2`.

```bash
python -m pip install -U das2py   # existing installs: das3py plus the alias
python -m pip install das3py      # new code: das3 only
```

If you install das3py next to an old das2py 2.x instead of upgrading it,
remove the old one first (`pip uninstall das2py`), as both provide a
`das_verify` command.

The alias raises a `PendingDeprecationWarning`, which Python hides by
default.  To find the places in a project that still use the old name, run
it with that warning promoted to an error:

```bash
python -W error::PendingDeprecationWarning your_script.py
```

Pre-build versions of das3py are available from Anaconda.  If you're working in an 
Anoconda or Miniconda python 3 environment these are easier to install as no C 
compiler is required.   To install the conda package run the command:
```bash
(base) $ conda install -c dasdevelopers das3py
```
The anaconda package automatically pulls in [das2C](https://anaconda.org/dasdevelopers/das2c), 
[pycdf](https://anaconda.org/dasdevelopers/pycdf), and [pthreads4w](https://anaconda.org/dasdevelopers/pthreads4w) as needed.

If this works then test using:
```bash
(base) $ wget https://raw.githubusercontent.com/das-developers/das3py/main/examples/ex02_galileo_pws_spectra.py
(base) $ python ex02_galileo_pws_spectra.py
```
If this command produces a plot similar to the following\.\.\.

<img src="https://raw.githubusercontent.com/das-developers/das3py/main/examples/ex02_galileo_pws_spectra.png" width="660" height="379">

\.\.\.then das3py is installed, and you can skip building the software and
head straight the example program below.

## Building and Installing from Source on Linux

Compilation and installation of das3py is regularly tested on Linux under
both Python 2 (still) and Python 3. Testing on Windows and MacOS is less
frequent, and then only under Python 3. The following packages are required
to build das3py and all dependencies:

1. Get system software packages.
   
   Rocky Linux or Compatable
   ```bash
   dnf install git gcc make curl expat-devel fftw-devel openssl-devel python3-devel
   ```
   Debian Linux or Compatable
   ```bash
   apt install gcc git make curl libexpat-dev libfftw3-dev libssl-dev python3-dev
   ```

2. Get the source, along with the das2C it goes with.
   ```bash
   git clone --recursive https://github.com/das-developers/das3py.git
   ```
   das2C is a git submodule in `deps/das2C`, pegged to the das2C release
   this das3py was written and tested against.  If you cloned without
   `--recursive` that directory is empty; fill it with
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
   build that one yourself and name it.  das3py then leaves it alone:
   ```bash
   make DAS2C=/path/to/das2C PY_BIN=/path/to/python
   ```

4. Install into your desired python environment.
   ```bash
   /path/to/python -m pip install ./dist/das3py-*.whl

   # or
   
   make PY_BIN=/path/to/python install
   ```

## Alternate builds

To build from source under anaconda, or for Windows or MacOS hosts and
to interact with PyPI see the alternate instructions in 
[buildfiles/ReadMe.md](buildfiles/AltBuild.md)

## First program

The following small program demonstrates how to query for data and generate a plot 
using das3py.

### Query a URI for reduced resolution data
```python
import das3
src = das3.get_source( 'tag:das2.org,2012:site:/uiowa/galileo/pws/survey_electric/das2' )
header, datasets = src.get( {'time' : ('1997-05-07T15:00', '1997-05-07T17:00', 4.0)} )
```
  * `get()` returns the stream header and a list of datasets.  This
    instrument sends three, one for each of its frequency bands.
  * Servers come and go.  The federated catalog provides stability for 
    application code by maping URIs to data sources.
  * Browse for URIs using a [das2 catalog](https://das2.org/browse) browser,
    or using das3py to get the root node (see [example 11](https://raw.githubusercontent.com/das-developers/das3py/main/examples/ex11_catalog_listings.py)).

### Access quantities by physical dimensions
```python
dataset = datasets[0]
print(dataset)

vX = dataset['time']['center']
vY = dataset['frequency']['center']
vZ = dataset['electric']['center'] 
```
  * Datasets contain dimensions. Dimensions contain variables. Each variable in a 
    dimension serves a purpose, the most common is to define the center point of a
    coordinate or measurement. 
  * Array dimensions are not confused with physical dimensions. Dataset meaning is
    not tied to any particular array morphology.

### Use Matplotlib to generate an image.
```python
import matplotlib.pyplot as pyplot
import matplotlib.colors as colors

fig, ax = pyplot.subplots()
scaleZ = colors.LogNorm(vmin=1e-17, vmax=1e-4)   # One color scale for all bands

for dataset in datasets:
   vX = dataset['time']['center']
   vY = dataset['frequency']['center']
   vZ = dataset['electric']['center']
   ax.pcolormesh(vX.array, vY.array, vZ.array, norm=scaleZ, cmap='jet')

ax.set_yscale('log')
pyplot.show() 
```
## Reporting bugs
Please use the github.com [issue tracker](https://github.com/das-developers/das3py/issues) 
report any problems with the library.  If you've fixed a bug, 1) thanks!, 2) please send
a pull request so that your updates can be merged into the main project.




