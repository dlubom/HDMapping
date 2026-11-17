#!/usr/bin/env python3
"""Run the same pinned Go checks locally and in the product's CI."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

MODULE = Path(__file__).resolve().parents[1] / "apps" / "hdmapping_mcp"
PLATFORMS = [(os_name, arch) for os_name in ("darwin", "linux", "windows")
             for arch in ("amd64", "arm64")]


def run(command, cwd, env, expect_failure=False):
    print("+", subprocess.list2cmdline([str(arg) for arg in command]), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="", flush=True)
    if expect_failure:
        if result.returncode == 0:
            raise RuntimeError("Negative control unexpectedly passed")
    else:
        result.check_returncode()
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--go", required=True)
    parser.add_argument("--tools-dir", type=Path, required=True)
    parser.add_argument("--checks", nargs="+", required=True,
                        choices=("format", "lint", "test", "race", "fuzz", "vuln", "controls"))
    parser.add_argument("--race-controls", action="store_true")
    args = parser.parse_args()
    go = str(Path(args.go).resolve())
    tools = args.tools_dir.resolve()
    tools.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, GOTOOLCHAIN="local", GOWORK="off", GOENV="off", GOFLAGS="-mod=readonly")
    env["HDMAPPING_GO_EXECUTABLE"] = go
    env["PATH"] = str(Path(go).parent) + os.pathsep + env.get("PATH", "")
    # A native test/check must not accidentally inherit a cross-build target.
    for key in ("GOOS", "GOARCH"):
        env.pop(key, None)
    env["CGO_ENABLED"] = "0"
    suffix = ".exe" if os.name == "nt" else ""
    lint = tools / ("golangci-lint" + suffix)
    vuln = tools / ("govulncheck" + suffix)
    gofmt = Path(go).with_name("gofmt" + suffix)
    if set(args.checks) & {"lint", "controls"}:
        run([go, "build", "-o", lint, "github.com/golangci/golangci-lint/v2/cmd/golangci-lint"], MODULE / "tools", env)
        run([lint, "version"], MODULE, env)
    if "vuln" in args.checks:
        run([go, "build", "-o", vuln, "golang.org/x/vuln/cmd/govulncheck"], MODULE / "tools", env)
        run([vuln, "-version"], MODULE, env)

    def check(name, module, negative=False):
        if name == "format":
            files = sorted(str(p) for p in module.rglob("*.go"))
            if not files:
                raise RuntimeError("No Go sources discovered")
            output = run([gofmt, "-l", *files], module, env)
            if negative == (not output.strip()):
                raise RuntimeError("Unexpected formatting result")
            return output
        if name == "lint":
            return run([lint, "run", "./..."], module, env, negative)
        if name in ("test", "race"):
            test_env = dict(env, CGO_ENABLED="1" if name == "race" else "0")
            return run([go, "test", "-vet=off", "-count=1", "-timeout=90s",
                        *(["-race"] if name == "race" else []), "./..."], module, test_env, negative)
        if name == "fuzz":
            for package, target in (("./internal/jobs", "FuzzDecodeJournal"),
                                    ("./internal/protocol", "FuzzReadFrame")):
                listing = run([go, "test", "-vet=off", "-list", f"^{target}$", package],
                              module, env)
                if target not in listing.splitlines():
                    raise RuntimeError(f"Fuzz target not discovered: {target}")
                run([go, "test", "-vet=off", "-run=^$", f"-fuzz=^{target}$",
                     "-fuzztime=10s", "-parallel=2", "-timeout=90s", package], module, env)
            return

        if name == "vuln":
            for goos, goarch in PLATFORMS:
                print(f"govulncheck: {goos}/{goarch}, CGO_ENABLED=0, no build tags", flush=True)
                run([vuln, "./..."], module, dict(env, GOOS=goos, GOARCH=goarch))

    for name in args.checks:
        if name != "controls":
            check(name, MODULE)
            continue
        # Disposable copies exercise actual runner exits without dirtying product files.
        with tempfile.TemporaryDirectory(prefix="hdmapping-check-controls-") as directory:
            module = Path(directory) / "apps" / "hdmapping_mcp"
            shutil.copy2(MODULE.parents[1] / "CMakeLists.txt", Path(directory) / "CMakeLists.txt")
            shutil.copytree(MODULE, module, ignore=shutil.ignore_patterns("tools"))
            check("test", module)
            probe = module / "control_test.go"
            probes = {
                "format": ('package control\nfunc badformat( ){ }\n', "control_test.go"),
                "lint": ('package control\nimport "fmt"\nfunc bad() { fmt.Printf("%d", "wrong") }\n', "fmt.Printf"),
                "test": ('package control\nimport "testing"\nfunc TestControl(t *testing.T) { t.Fatal("deliberate test failure") }\n', "deliberate test failure"),
            }
            if args.race_controls:
                probes["race"] = ('''package control
import ("sync"; "testing")
func TestRaceControl(t *testing.T) {
 var x int
 var wg sync.WaitGroup
 start := make(chan struct{})
 for range 2 { wg.Go(func() { <-start; for range 10000 { x++ } }) }
 close(start)
 wg.Wait()
 t.Log(x)
}
''', "DATA RACE")
            for control, (source, diagnostic) in probes.items():
                probe.write_text(source, encoding="utf-8")
                if control != "format":
                    run([gofmt, "-w", probe], module, env)
                output = check(control, module, negative=True)
                if diagnostic not in output:
                    raise RuntimeError(f"Missing expected {control} diagnostic: {diagnostic}")
                probe.unlink()
                print(f"PASS negative control: {control}", flush=True)


if __name__ == "__main__":
    main()
