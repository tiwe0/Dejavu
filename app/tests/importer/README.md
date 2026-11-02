# Importer host regression

This executes the production importer, package validator, script builder and
RootShell against an in-memory Storage Access Framework provider. It does not
execute plugin entry code, connect to a device or require an Android SDK.

```sh
JAVA_HOME=/path/to/jdk \
KOTLINC=/path/to/kotlinc \
JSON_JAR=/path/to/json-20250517.jar \
bash app/tests/importer/run-host.sh
```

The launcher creates one marked temporary directory and removes it on exit.
It refuses environments where RootShell could select a real absolute su path.
A PATH-local fake su maps the fixed `/data` prefix into that temporary directory;
the real host `/data` is never written. Ownership changes are skipped, while
real chmod, source-file writes, moves, rollback and scans run as the host user.
The base64 shim matches Android's file-argument form on macOS as well as Linux.

Coverage: the self-contained example package, opaque provider document IDs, nested and
180 KiB source resources, invalid-package isolation, missing/oversized/non-UTF-8
resources, replacement guards, successful replacement, injected final-rename
failure and rollback, and stored resource/directory symlink rejection.

These stubs test the importer boundary and real shell filesystem behavior. They
do not replace full Android compilation or a real DocumentsProvider/device UI
test. No source packages are installed outside the temporary sandbox.
