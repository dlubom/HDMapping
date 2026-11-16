#!/usr/bin/env python3
"""Check MCP ON/OFF in an installed or packaged product after relocation."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command, **kwargs):
    print("+", subprocess.list2cmdline([str(arg) for arg in command]), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--mcp", choices=("ON", "OFF"), required=True)
    parser.add_argument("--package-dir", type=Path)
    parser.add_argument("--package-executable", help="MCP path relative to package directory")
    args = parser.parse_args()
    if bool(args.package_dir) != bool(args.package_executable):
        parser.error("--package-dir and --package-executable must be used together")

    build = args.build_dir.resolve()
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line.startswith(("//", "#")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    enabled = (build / "hdmapping-mcp-enabled.txt").read_text(encoding="utf-8").strip()
    if enabled != ("1" if args.mcp == "ON" else "0"):
        raise RuntimeError("BUILD_WITH_MCP does not match the requested check")
    tests = json.loads(run(
        ["ctest", "--test-dir", build, "-C", args.config, "--show-only=json-v1"],
        capture_output=True, text=True, encoding="utf-8").stdout)["tests"]
    contracts = [test for test in tests if test["name"] == "mcp-contract"]
    if len(contracts) != (1 if args.mcp == "ON" else 0):
        raise RuntimeError("Unexpected MCP test registration")
    if args.mcp == "OFF":
        go_artifacts = list((build / "_deps").glob("go-*"))
        go_artifacts += [build / name for name in ("go-cache", "go-modules", "go-tools")
                         if (build / name).exists()]
        if go_artifacts:
            raise RuntimeError(f"MCP OFF bootstrapped Go: {go_artifacts}")

    suffix = ".exe" if os.name == "nt" else ""
    with tempfile.TemporaryDirectory(prefix="hdmapping-install-") as temporary:
        staging = Path(temporary) / "original"
        if args.package_dir:
            shutil.copytree(args.package_dir, staging, symlinks=True)
            relative_executable = Path(args.package_executable)
            if relative_executable.is_absolute() or ".." in relative_executable.parts:
                raise RuntimeError("Package executable must be relative to its directory")
        else:
            run(["cmake", "--install", build, "--config", args.config,
                 "--component", "applications", "--prefix", staging])
            relative_executable = Path("bin") / (
                cache["HDMAPPING_BINARY_PREFIX"] + "hdmapping_mcp" + suffix)
        relocated = Path(temporary) / "przeniesiona instalacja żółć"
        staging.rename(relocated)
        executable = relocated / relative_executable
        if args.mcp == "ON":
            if not executable.is_file():
                raise RuntimeError(f"Installed MCP is missing: {executable}")
            test = contracts[0]
            command = test["command"][:]
            assignments = [i for i, arg in enumerate(command)
                           if arg.startswith("HDMAPPING_MCP_EXECUTABLE=")]
            if len(assignments) != 1:
                raise RuntimeError("MCP contract must explicitly select the shipped executable")
            command[assignments[0]] = f"HDMAPPING_MCP_EXECUTABLE={executable}"
            cwd = next(prop["value"] for prop in test["properties"]
                       if prop["name"] == "WORKING_DIRECTORY")
            # Reuse the SDK contract and CMake's pinned Go/environment. The
            # product subprocess uses a temporary cwd and an empty PATH.
            run(command, cwd=cwd)
        elif executable.exists() or list(relocated.rglob("*hdmapping_mcp*")):
            raise RuntimeError("MCP OFF unexpectedly installed the MCP executable")
        print(f"PASS: MCP {args.mcp}, relocated product: {relocated}", flush=True)


if __name__ == "__main__":
    main()
