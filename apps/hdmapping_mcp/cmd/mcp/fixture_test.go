package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

func buildTestProduct(t *testing.T, ctx context.Context, version, prefix string) (adapter string, coordinator string, worker string) {
	t.Helper()
	dir := t.TempDir()
	suffix := ""
	if runtime.GOOS == "windows" {
		suffix = ".exe"
	}
	goExecutable := os.Getenv("HDMAPPING_GO_EXECUTABLE")
	if goExecutable == "" {
		goExecutable = "go"
	}
	build := func(name, pkg string) string {
		path := filepath.Join(dir, prefix+name+suffix)
		args := []string{"build", "-buildvcs=false"}
		if raceBuild {
			args = append(args, "-race")
		}
		args = append(args, "-ldflags=-X=main.productVersion="+version, "-o", path, pkg)
		cmd := exec.CommandContext(ctx, goExecutable, args...)
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("build %s: %v\n%s", name, err, output)
		}
		return path
	}
	return build("hdmapping_mcp", "."), build("hdmapping_coordinator", "../coordinator"), build("hdmapping_worker", "./testdata/worker")
}
