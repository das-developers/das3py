# Changelog

Notable changes to das3py (published as das2py through 3.0.4).  Newest
release first.  This is a summary, not a full history; `git log` has the
rest.

## [3.0.5] - unreleased

**The first release built on a stable das2C v3 (v3.0.5), and the first
under the name das3py.**

These notes cover everything since the last github release for das2py 
which was 2.3.1 (2023-11-21). Some conda and PyPI packages were released
from the pre-3.0 codebase, those are addressed by the transitionary 
das2py described below.

das2py was the first v3 style module, written ahead of das2C v3.0.  das2C
has since caught up, so most of the application interface from 2.3 is
unchanged.  The bigger differences are the package name, a handful of
method renames, and what you can read: das3 streams and composite values.

### Upgrading from das2py

- **The import package is `das3`** and the C extension is `_das3`.  The
  name is for the das3 stream format, not the Python version.

- **The pip and conda name is `das3py`.**  `das2py` 3.0.5 is a small
  transitional package that installs das3py and aliases the old das2
  module name for minimal code breakage.
  ```bash
  python -m pip install -U das2py   # existing installs: das3py plus the alias
  python -m pip install das3py      # new code: das3 only
  ```

- **The `das2` alias warns.**  It raises a `PendingDeprecationWarning`,
  which Python hides by default.  To find old imports, run:
  ```bash
  python -W error::PendingDeprecationWarning your_script.py
  ```

- **If you install das3py into the same virtual environment with das2py
  instead of upgrading das2py, uninstall it first.** Old versions own files
  that das3py now installs, including the `das_verify` command, and pip lets
  the second install overwrite them.  Uninstalling the old package later
  would then *delete* das3py's copies.

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
  | `Source`    | `.protoGet()`    | `.getProto()`             |
  | `Source`    | `.protoInfo()`   | `.infoProto()`            |

  The same `-W error::PendingDeprecationWarning` run that finds old
  `import das2` lines finds these too.  A `Source` subclass that overrides
  `protoGet()` or `protoInfo()` must rename its methods, since `get()`
  calls the new names.

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

- **Logins go over https only**, a das2C v3.0.5 policy.  A server asking
  for a login over plain http fails without prompting.  A redirect to
  another host, port or scheme drops the login.  Stored credentials are
  keyed by URL, scheme included, so save them under `https://` addresses.

- **The low-level read functions return `(dict, list)`.**  This changes
  `read_file`, `read_cmd` and `read_server` in the `_das3` C module, which
  only code calling that module directly sees.  The stream-level context
  dict is gone.

- **Build.**  das3py requires das2C v3.0.5 or later and no longer builds
  against das2C v2.3.  The extension links libdas3 statically and uses
  the same libcdf as das2C.

### Fixed

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

## [2.3.1] - 2023-11-21

Last release of the 2.x line, on PyPI as das2py 2.3.1.  It remains
installable with `pip install das2py==2.3.1`.

[3.0.5]: https://github.com/das-developers/das3py/compare/v2.3.1...v3.0.5
[2.3.1]: https://github.com/das-developers/das3py/releases/tag/v2.3.1
