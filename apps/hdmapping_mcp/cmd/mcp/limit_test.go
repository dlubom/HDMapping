package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

func TestCoordinatorLimitConfiguration(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 60*time.Second)
	defer cancel()
	const version = "1.2.3"
	_, coordinator, _ := buildTestProduct(t, ctx, version, "HD-")
	dir, err := protocol.ValidateDirectory(filepath.Join(t.TempDir(), "stan żółć"))
	if err != nil {
		t.Fatal(err)
	}
	client, err := protocol.NewClient(dir, version, coordinator)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { stopCoordinator(t, client) })
	setLimit := func(limit string) (string, error) {
		command := exec.CommandContext(ctx, coordinator, "set-limit", limit)
		command.Env = append(os.Environ(), "HDMAPPING_MCP_STATE_DIR="+dir)
		output, err := command.CombinedOutput()
		return string(output), err
	}

	if err := client.Ensure(ctx); err != nil {
		t.Fatal(err)
	}
	var oldPID int
	call(t, ctx, client, "ping", nil, &oldPID)
	first := submit(t, ctx, client, dir, "limit-first")
	waitJob(t, ctx, client, first.ID, "ready", jobs.Running)
	second := submit(t, ctx, client, dir, "limit-second")
	waitJob(t, ctx, client, second.ID, "ready", jobs.Running)
	if output, err := setLimit("1"); err != nil {
		t.Fatalf("set-limit: %v\n%s", err, output)
	}
	if output, err := setLimit("65"); err == nil || !strings.Contains(output, "limit must be 1..64") {
		t.Fatalf("invalid limit: %v\n%s", err, output)
	}
	third := submit(t, ctx, client, dir, "limit-third")
	call(t, ctx, client, "jobs.cancel", first.ID, nil)
	waitJob(t, ctx, client, first.ID, "graceful_exit", jobs.Cancelled)
	waitJob(t, ctx, client, second.ID, "ready", jobs.Running)
	waitJob(t, ctx, client, third.ID, "", jobs.Preparing)
	call(t, ctx, client, "jobs.cancel", second.ID, nil)
	waitJob(t, ctx, client, third.ID, "ready", jobs.Running)
	call(t, ctx, client, "jobs.cancel", third.ID, nil)
	waitJob(t, ctx, client, third.ID, "graceful_exit", jobs.Cancelled)

	if err := stopTestCoordinator(oldPID); err != nil {
		t.Fatal(err)
	}
	if err := client.Ensure(ctx); err != nil {
		t.Fatal(err)
	}
	var newPID int
	call(t, ctx, client, "ping", nil, &newPID)
	if newPID == oldPID {
		t.Fatal("coordinator did not restart")
	}
	active := submit(t, ctx, client, dir, "limit-after-restart")
	waitJob(t, ctx, client, active.ID, "ready", jobs.Running)
	waiting := submit(t, ctx, client, dir, "limit-wait-after-restart")
	waitJob(t, ctx, client, waiting.ID, "", jobs.Preparing)
	call(t, ctx, client, "jobs.cancel", waiting.ID, nil)
	call(t, ctx, client, "jobs.cancel", active.ID, nil)
	waitJob(t, ctx, client, active.ID, "graceful_exit", jobs.Cancelled)
}
