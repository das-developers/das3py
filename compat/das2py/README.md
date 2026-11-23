# das2py is now das3py

As of version 3.0.6 this library is published as
[**das3py**](https://pypi.org/project/das3py/).  It imports as `das3`,
named for the das3 stream format it reads, and runs on Python 2.7 and 3.x
alike.

New projects should install das3py directly:

```bash
python -m pip install das3py
```

This package exists so that old installs keep working.  Upgrading it
installs das3py and adds a small `das2` alias, so `import das2` still works
and returns the very same modules as `import das3`:

```bash
python -m pip install -U das2py
```

This package stays on the das3py 3.0 series, where the method names
from das2py 2.3 still work.  das3py 3.1 removes them, so code that
has moved to `import das3` and the current names should uninstall das2py
to get newer releases.

The alias issues a `PendingDeprecationWarning`, which Python hides by
default.  To find code that still uses the old name, run it with:

```bash
python -W error::PendingDeprecationWarning your_script.py
```

The pre-rename releases (2.3.x) remain on PyPI.  To stay on them, pin
`das2py<3`.

* Source and issues: <https://github.com/das-developers/das3py>
* das3py on PyPI: <https://pypi.org/project/das3py/>
