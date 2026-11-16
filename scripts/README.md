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
(govet, staticcheck, errcheck, unused), contract tests and govulncheck.
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

C++ P03 preparation: both existing formatting scripts include `processing/`,
and the common build already exports `compile_commands.json`. P03 adds
per-target warning/clang-tidy and separate ASan+UBSan/TSan CMake configurations
when the first processing targets exist at that stage.
