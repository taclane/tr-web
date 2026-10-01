# SQLite (vendored)

The official SQLite amalgamation, compiled into the plugin. Users need no database package.

- Version: 3.53.4
- Source: https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
- SHA3-256 of the zip: `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`
  (as published on https://sqlite.org/download.html)
- Files taken from the zip: `sqlite3.c`, `sqlite3.h` (unmodified)
- License: public domain (https://sqlite.org/copyright.html)

## Updating

Download the new amalgamation zip, check its SHA3-256 against the download page, copy
`sqlite3.c` and `sqlite3.h` over these, and update the version and hash above.
Compile options are set in tr-web's `CMakeLists.txt`.
