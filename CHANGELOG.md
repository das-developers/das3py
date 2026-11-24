# Changelog

Notable changes to das3py (published as das2py through 3.0.4).  Newest
release first.  This is a summary, not a full history; `git log` has the
rest.

## [3.0.7] - unreleased

Adds Windows packages.

### Added

- **Binary wheels on PyPI for 64-bit Windows**, Python 3.9 through 3.13.
  das2C, OpenSSL 3.6.5, expat 2.10.0, zlib and PThreads4W are linked into
  the extension, so nothing else has to be installed.  NASA's CDF 3.9.1
  library comes with it for `das3.pycdf` and `das_cdfinfo`.  They run on
  numpy 1.19.3 through 2.x.  `buildfiles\win_wheels.bat` builds them.

- **`das3.dft`**, the `Psd` and `Dft` calculators written on NumPy.  They
  take the same arguments as the FFTW ones in the C module and give the
  same results to rounding error.  `das3.PSD` is the NumPy one wherever the
  extension was built without FFTW.

- **Builds without FFTW.**  `DAS_DFT=no` builds the extension against a
  das2C made with `DFT=no`, without FFTW.  The Windows wheels are built
  this way, so they contain nothing under the GPL.

- **License texts in the wheels.**  A wheel now carries the license of
  each library compiled into it, and `THIRD-PARTY.txt`, which lists them.

- `test/TestPsd.py`, in the standard test list.

### Changed

- **Built on das2C v3.0.7.**  2018 catalog nodes (HttpStreamSrc 0.6 and
  earlier, and 2018 Collections) are no longer read.  das2.org serves only
  current nodes.

- `setup.py` on Windows links the static libraries that das2C's nmake
  build provides.  It used to look for an older vcpkg layout.

- `examples/c_module/galileo_pws_e-survey.py` plots all three frequency
  bands.  The server now sends one dataset per band, and the example drew
  only the first.

- `examples/ex03_cassini_rpws_multimode.py` draws a wider plot that fits
  its title and date labels, and starts the frequency axis at 0.8 Hz.

### Fixed

- `_das3.Dft.calculate()` raised `TypeError` for a real signal, that is,
  whenever the imaginary input was left out.

- `examples/c_module/juno_hfwbr_cdf.py` put colons in the name of the CDF
  file it writes, which Windows does not allow.

## [3.0.6] - 2026-11-24

**The first release built on a stable das2C v3 (v3.0.6), and the first
under the name das3py.**

These notes cover everything since the last github release for das2py 
which was 2.3.1 (2023-11-21). Some conda and PyPI packages were released
from the pre-3.0 codebase; see *Upgrading from das2py* below.

das2py was the first v3 style module, written ahead of das2C v3.0.  das2C
has since caught up, so most of the application interface from 2.3 is
unchanged.  The bigger differences are the package name, a handful of
method renames, and what you can read: das3 streams and composite values.
das2 streams read as before, and will be supported indefinitely.

### Upgrading from das2py

- **The import package is `das3`** and the C extension is `_das3`.  The
  name is for the das3 stream format it added, not the Python version.
  das2 streams are still read.

- **The pip and conda name is `das3py`.**  There is no `das2` alias:
  `import das2` becomes `import das3`, and `das2.` becomes `das3.`.
  Catalog names and URLs that contain `das2.org` stay as they are.

- **Reading data returns the stream header too.**  `Source.get()`,
  `read_file()`, `read_http()` and `read_cmd()` return
  `(stream_hdr, ds_list)`; 2.3 returned only the list.  They raise an
  exception on failure instead of returning `None`.  (das2py 3.0.4 on
  conda already worked this way.)

- **das2py and das3py both include `das_verify`.**  If you uninstall
  das2py, reinstall das3py to replace it:
  `python -m pip install --force-reinstall --no-deps das3py`.

- **Python 2.7 and 3.6 and up are supported by das3py**, with NumPy 1.x or 2.x. 
  There is *no* plan to drop python2.7 for the foreseeable future.
  Planetary missions are long, budgets are tight, and Python 2.7 is stable-by-default.

- **Method renames.**  Methods are now camelCase and module functions
  snake_case.  The 2.3 names that application code is likely to use still
  work, but each raises a `PendingDeprecationWarning`, and they are planned
  to be removed in 3.1:

  | Class       | 2.3 name (warns) | 3.0 name                  |
  |-------------|------------------|---------------------------|
  | `DasTime`   | `.from_string()` | `.fromString()`           |
  | `DasTime`   | `.round_doy()`   | `.roundDoy()`             |
  | `Dimension` | `.propEq()`      | `.hasPropVal()`, see also `.hasProp()` |
  | `Source`    | `.protoGet()`    | none, see "Two ways to ask for data" below |
  | `Source`    | `.protoInfo()`   | `.infoProto()`            |

  To find them, run:
  ```bash
  python -W error::PendingDeprecationWarning your_script.py
  ```

  Each module now has an `__all__`, so helpers and stdlib names no longer
  leak into the package namespace.  Code that reached them through
  `das2.*` needs a direct import.

### Added

- **das3 streams**, in both text (`.d3t`) and binary (`.d3b`) form, next
  to das2 streams.  Reading goes through das2C's v3 reader, which also
  up-converts das2 headers to datasets.

- **Composite values.**  Vectors, rotation matrices, quaternions and
  complex pairs arrive as one variable with a trailing internal shape.
  A `Variable` carries `ops` (its formalism, unknown ones included) and
  `labels` (one per component).  `extShape()` and `extRank` give the
  dataset extents and their count, `intrShape()` and `intrRank` the extents
  of one value and theirs.  `storage()` gives the values as stored, without
  the dataset's broadcast, which is an indexing trick and costs no memory.
  Vector frames are supported, and variables gain a `vector-norm` role.

- **All thirteen das3 property types**, including `bool` and every
  `array` form.  Before, only the das 2.2 property types converted.

- **Dimension kinds.**  `Dimension.kind` is `'coord'` or `'data'`.
  `Dataset.coordKeys()` and `Dataset.dataKeys()` list one kind each, in the
  order added.  `in` accepts the same spellings as `ds[...]`.

- **Packet writers speak das3.**  `HdrBuf`, `PktBuf` and the `send*`
  helpers take a format token (das2 by default).  Exception types
  translate between the das2 and das3 vocabularies.  `EXCEPT_NOMATCHING`
  and `EXCEPT_QUERYERR` are new.

- **`das_verify` checks more than the schema can.**  It validates das2
  and das3 streams against the bundled schemas.  For das3 streams it also
  checks things no XML schema can express: `index=` position counts, one
  `units=` per composite, and `<sequence>` counts against the internal
  shape.

- **`das_cdfinfo`** is now an installed script.  It lists a CDF's
  contents, including compression.

- **Nanosecond time.**  TT2000 values convert to `datetime64[ns]` with
  integer arithmetic, and `DasTime` holds TT2000 to the nanosecond.

- **Current catalog nodes.**  Works with `Catalog` 0.5, `SourceSet` 0.2
  and `HttpStreamSrc` 0.7 nodes.  A path that ends at a `SourceSet` gives
  the source das3py reads best.  `das3.Collection`, the 2.3 class for a
  dataset's set of sources, is now another name for `das3.SourceSet`, so
  `isinstance(node, das3.Collection)` in old code still works.

- **Catalog session calls.**  `das3.mount()`, `unmount()` and `mounts()`
  put a branch from another server or a local file ahead of the das
  catalog.  `das3.seed()` and `seed_info()` choose where paths start, and
  `das3.check()` loads the seed and mounts at once.

- **`das3.node_dict(path)`** returns any catalog node as a plain
  dictionary.

- **More query forms.**  `Source.get()` also takes das2C's dotted keys, a
  single time for the span it denotes (`{'time': '2026-05-04'}`), and a
  list or tuple for a property that holds several values.

- **`examples/ex12_tracers_mag_delta.py`** plots a composite vector
  variable by component.

### Changed

- **Values and their descriptions agree.**  das3py converts time points
  to `datetime64[ns]` and durations to `timedelta64[ns]`.  It also turns
  sequences and constants into arrays named `<dim>.<role>`.  A variable's
  `units` and `expression`, and a dataset's `info` text, now describe the
  values as Python holds them (`ns1970`, `ns`) rather than as the stream
  sent them.

- **Errors propagate.**  `read_file()`, `read_cmd()` and `read_http()` let
  das3 errors reach application code.  Before, some failures came back as
  `SystemError` or as `None` with no exception set.  Messages from das2C
  now say where in das2C the problem was found.

- **Stricter units.**  Malformed unit strings (`"cows^"`) raise
  `ValueError` instead of being taken as is.  `convertible()` and
  `can_merge()` raise on units they can't parse instead of answering
  `False`, so a typo like `tt2000` for `TT2000` doesn't pass silently.

- **Out-of-range times raise.**  Converting a UTC time with out-of-range
  fields to TT2000 raises `ValueError`.  Before, it returned the CDF
  illegal-value sentinel, which callers rarely checked.

- **The catalog never writes to disk.**  Catalog lookups go through one
  das2C catalog session per process.  It keeps the nodes it loads in
  memory and leaves your home directory alone.

- **das2C reads data sources.**  Source descriptions, query checking and
  request URLs come from das2C, so new node layouts arrive with a das2C
  rebuild.  A rejected query raises `ValueError` naming each bad entry.
  Query keys are the Key column of `src.info()`.

- **Two ways to ask for data.**  `Source.get()` goes through the source's
  interface and `das3.read_http()` reads a URL; `Source.baseUrls()` is
  new for building one.  `Source.protoGet()` mixed the two.  It warns and
  is planned to be removed in 3.1.  `getProto()`, its name in the 3.0
  pre-releases, is gone.

- **A list in a query is a list of values.**  The 2.3 shortcut
  `{'electric': ['rm_spikes']}` is written `{'electric': {'rm_spikes': True}}`.

- **Examples are numbered.**  `Source.examples()` returns
  `(number, label)` pairs and `get()` takes either.  With no query,
  `get()` uses the first example that produces a das stream.

- **Logins go over https only**, a das2C v3.0.5 policy.  A server asking
  for a login over plain http fails without prompting.  A redirect to
  another host, port or scheme drops the login.  Stored credentials are
  keyed by URL, scheme included, so save them under `https://` addresses.

- **The low-level read functions return `(dict, list)`.**  This changes
  `read_file`, `read_cmd` and `read_server` in the `_das3` C module, which
  only code calling that module directly sees.  The stream-level context
  dict is gone.

- **Build.**  das3py requires das2C v3.0.6 or later and no longer builds
  against das2C v2.3.  The extension links libdas3 statically and uses
  the same libcdf as das2C.  das2C is now a git submodule in `deps/das2C`,
  pinned to the release das3py is tested against, and `make` builds it:
  clone with `git clone --recursive`.  `make DAS2C=/path` uses another.
  Only wheels are built and published, there is no source distribution;
  to build das3py yourself, start from the git repository.

- **Binary wheels on PyPI** for Linux x86_64: Python 2.7 and 3.9 through
  3.15.  They need glibc 2.26 or newer (RHEL 8, Ubuntu 18.04 and
  later) and pip 20.3 or newer.  fftw3 and OpenSSL ship inside the wheel,
  and expat 2.11.0 is linked in statically, so slim container images
  work.  Other platforms, and Python 3.6 to 3.8, build from the git
  repository.

- **numpy floor.**  numpy 1.19.3 for Python 3.9 and up: the wheels are
  compiled against numpy 2 and run on 1.19.3 through 2.x.  numpy 1.16.6
  for Python 2.7.

### Fixed

- `get_source()` and `get_catalog()` returned the wrong object for a
  `Collection`, and their error paths failed.

- `CatalogError` raised with a message alone surfaced as a `TypeError`.

- Empty datasets and empty arrays no longer crash the conversion to
  NumPy.

- Datasets with many variables convert.

- `Dataset.rank` is the number of dataset indices.  It was a field that
  stayed 0.

- Iterating over a `Dataset` works on Python 2.7.

- Arithmetic on a `Quantity` works from both sides on 2.7 and 3.x
  (`1.0 / q`, `2 * q`).

- `tt2k_utc()` returned a malformed tuple.

- Auth file loading.

- Windows line endings in streams.

- Property separators are null terminated.

### Known limitations

- Ragged arrays (variable-length records) still don't convert to
  ndarrays.  das2C reads them, and das3py stops at the conversion.

- das2C doesn't yet accept an XML comment before a header's root element
  ([das2C#39](https://github.com/das-developers/das2C/issues/39)).

- Namespaced das3 streams don't read yet.

- The Python 2.7 wheel is for UCS-4 builds (`cp27mu`), as distributions
  ship.  A 2.7 compiled from source with the default `./configure` is
  UCS-2 and can't install it: rebuild with `--enable-unicode=ucs4`, or
  build das3py from git.

## [2.3.1] - 2023-11-21

Last release of the 2.x line, on PyPI as das2py 2.3.1.  It remains
installable with `pip install das2py==2.3.1`.

[3.0.6]: https://github.com/das-developers/das3py/compare/v2.3.1...v3.0.6
[2.3.1]: https://github.com/das-developers/das3py/releases/tag/v2.3.1
