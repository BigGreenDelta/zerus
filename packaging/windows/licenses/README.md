# Canonical Qt license texts for the Windows and macOS bundles

`scripts/package-windows.py` and `scripts/macos_notices.py` copy every `*.txt`
here into the bundle's Qt notice folder. Each file is named after the SPDX
license or exception identifier it holds.
Packaging fails when one of the bundled Qt SPDX documents names a standard
identifier without a file here; a `LicenseRef-` identifier must instead come
with its extracted text inside the documents.

This is conservative coverage of the shipped module metadata, including license
alternatives and optional components; it is not a compiled-component inventory.
The macOS bundle also retains its existing `GPL-3.0.txt` and `LGPL-3.0.txt`
filenames, module SPDX JSON and corresponding-source/replacement note.

Sources, unchanged:

- `GPL-3.0-only.txt` and `LGPL-3.0-only.txt`: Qt's `LICENSES` directory, as
  installed by the Arch Linux `qt6-base` 6.12.0 package.
- Every other file: the `text/` directory of the SPDX License List data,
  https://github.com/spdx/license-list-data at tag `v3.29.0`.

When a Qt upgrade names a new identifier, add its text from the same SPDX tag
(or a newer one) and list it in `QT_LICENSE_TEXTS` in
`scripts/windows_portable.py`.
