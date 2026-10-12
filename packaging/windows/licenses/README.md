# License texts for the Windows portable zip

`scripts/package-windows.py` copies every `*.txt` here to `licenses/qt/` in the
zip. Each file is named after the SPDX license or exception identifier it holds.
Packaging fails when one of the bundled Qt SPDX documents names a standard
identifier without a file here; a `LicenseRef-` identifier must instead come
with its extracted text inside the documents.

Sources, unchanged:

- `GPL-3.0-only.txt` and `LGPL-3.0-only.txt`: Qt's `LICENSES` directory, as
  installed by the Arch Linux `qt6-base` 6.12.0 package.
- Every other file: the `text/` directory of the SPDX License List data,
  https://github.com/spdx/license-list-data at tag `v3.29.0`.

When a Qt upgrade names a new identifier, add its text from the same SPDX tag
(or a newer one) and list it in `QT_LICENSE_TEXTS` in
`scripts/windows_portable.py`.
