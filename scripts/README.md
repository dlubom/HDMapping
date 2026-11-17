# Repository management utilities

* `run_clang_format.py` — runs `clang-format` on all C/C++ source and header files located in:

  * `core`
  * `processing`
  * `apps`
  * `pybind`
  * `shared`
  * `raylib_widgets`

* `run_check_clang_format.py` — checks whether running `clang-format` would modify any tracked project files. The script runs `run_clang_format.py` and then checks the Git working tree for changes. If formatting would produce changes, the check fails and lists the affected files.

* `run_submodules_sync_update.py` — initializes and updates Git submodules recursively using:
  `git submodule sync` and `git submodule update --init --recursive`.

* `run_pull_submodules_changes.py` — updates submodules located under `3rdparty`. For each submodule, the script:

  * reads the branch configured in `.gitmodules`;
  * initializes the submodule if it is missing;
  * fetches from `origin`;
  * switches to the configured branch;
  * performs a fast-forward-only pull;
  * reports whether the submodule was actually updated.

  Submodules without a branch configured in `.gitmodules` are reported and skipped.

# Data utilities

* `resample_imu.py` — resamples Mandeye IMU CSV files to a lower rate (default 200 Hz) by averaging all samples in each 1/rate window, so vibration above the new Nyquist rate is attenuated instead of aliased. Handles `imu****.csv` and external-IMU subfolders such as `MURATA_IMU/`. Given a scan folder, it writes a copy (`<folder>_imu200hz` by default; other files hard-linked when possible) with the IMU resampled, leaving the input untouched:

  ```
  python3 scripts/resample_imu.py /path/to/continousScanning_0013 --rate 200
  ```

# MCP developer checks

`run_mcp_checks.py` runs fail-on-difference gofmt, the pinned golangci-lint
(govet, staticcheck, errcheck, unused), contract tests, journal parser fuzzing
and govulncheck. Ordinary tests replay the fuzz seed corpus; `mcp-check` also
requires discovery of `FuzzDecodeJournal` and runs it with `-fuzztime=10s`
and two workers on each native CI platform.
CMake supplies the pinned Go toolchain; quality tools are built from the separate
`apps/hdmapping_mcp/tools` module and are never installed with the product.

```sh
cmake -B build -DBUILD_WITH_MCP=ON -DBUILD_TESTING=ON -DHDMAPPING_MCP_QUALITY=ON
cmake --build build --target hdmapping_mcp_build mcp-check mcp-check-controls
ctest --test-dir build -L mcp --output-on-failure
```

On Linux amd64 enable `-DHDMAPPING_MCP_RACE=ON` (cgo and a C compiler required),
then run `cmake --build build --target mcp-race`; `mcp-check-controls` also checks
the race detector with a temporary reproducer. Controls copy sources into a
temporary directory, inject known format/analyzer/test failures, require the
expected diagnostics and remove the copies. Product files are not modified.

The vulnerability matrix is darwin/linux/windows × amd64/arm64, `CGO_ENABLED=0`,
no build tags. Tests run natively; analysis of another GOOS does not qualify its
runtime. CMake's `mcp-contract` test uses the built executable. To test a relocated
installation, run from `apps/hdmapping_mcp` with the pinned Go on PATH:

```sh
HDMAPPING_MCP_EXECUTABLE='/path with spaces/żółć/bin/hdmapping-hdmapping_mcp' go test -vet=off -count=1 ./...
```

The subprocess runs with an empty PATH and a temporary working directory.
Go/Python are developer dependencies only. `-vet=off` avoids duplicating the full
vet pass already owned by golangci-lint. `go test ./...` remains usable directly.

`check_mcp_install.py` installs the complete `applications` component into a
temporary prefix, moves it to a path with spaces and Polish characters, then
reuses CTest's SDK contract against that executable. It takes `--build-dir`,
`--config` (default `Release`) and `--mcp ON|OFF`. OFF checks the absence of MCP
tests, the installed executable and Go bootstrap artifacts; use a fresh build
directory and configure with `-DHDMAPPING_GO_EXECUTABLE=must-not-run-go` to prove
OFF does not consult Go. To check an existing package instead of CMake install,
also pass `--package-dir` and its relative `--package-executable`; the entire
package is copied and relocated before testing. Temporary copies are removed.

The existing platform workflows run ON/OFF on MCP branches and keep OFF for
other builds. ON runs Go quality gates including the process tests and fuzzing
of both the journal and IPC frame parsers. Both variants build the full product,
run CTest and check installation/relocation. The checker starts the adapter,
coordinator and worker from the moved Windows deploy directory, extracted DEB
or mounted DMG with an empty product PATH. OFF checks for all three binaries.

P03 C++ checks run in the bundled Ubuntu workflow. Build with
`BUILD_WITH_MCP=ON`, `BUILD_TESTING=ON`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`
and `HDMAPPING_PROCESSING_TIDY=ON` using Ninja or Makefiles. Run
`cmake --build build --target processing-tidy processing-check-controls`
and `ctest --test-dir build -L processing-runtime --output-on-failure`.
Use clang-tidy 18.1.3 on Ubuntu or 22.1.8 on macOS and clang-format 21.
For sanitizers, configure separate Clang builds with
`HDMAPPING_PROCESSING_SANITIZER=address-undefined` and `thread` respectively
and run the instrumented `processing_runtime_test` and negative controls.
