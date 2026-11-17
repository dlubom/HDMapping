package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func startAdapter(t *testing.T, ctx context.Context, executable, dir string) *sdk.ClientSession {
	t.Helper()
	command := exec.CommandContext(ctx, executable)
	command.Dir = t.TempDir()
	command.Env = append(os.Environ(), "PATH=", "GORACE=atexit_sleep_ms=0 halt_on_error=1", "HDMAPPING_MCP_STATE_DIR="+dir)
	client, err := sdk.NewClient(&sdk.Implementation{Name: "lifecycle", Version: "1"}, nil).
		Connect(ctx, &sdk.CommandTransport{Command: command}, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = client.Close() })
	return client
}

func stopCoordinator(t *testing.T, local *protocol.Client) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	var pid int
	if err := local.Call(ctx, "ping", nil, &pid); err != nil {
		return
	}
	if err := stopTestCoordinator(pid); err != nil {
		t.Errorf("stop test coordinator %d: %v", pid, err)
	}
}

func killCoordinator(pid int) error {
	process, err := os.FindProcess(pid)
	if err != nil {
		return err
	}
	defer func() { _ = process.Release() }()
	return process.Kill()
}

func TestCoordinatorProcessLifecycle(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 60*time.Second)
	defer cancel()
	const version = "1.2.3"
	adapter, coordinator, worker := buildTestProduct(t, ctx, version, "HD-")
	dir, err := protocol.ValidateDirectory(filepath.Join(t.TempDir(), "stan żółć"))
	if err != nil {
		t.Fatal(err)
	}
	client, err := protocol.NewClient(dir, version, coordinator)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { stopCoordinator(t, client) })
	a := startAdapter(t, ctx, adapter, dir)
	b := startAdapter(t, ctx, adapter, dir)
	results := make(chan error, 2)
	for _, adapter := range []*sdk.ClientSession{a, b} {
		go func() {
			result, err := adapter.CallTool(ctx, &sdk.CallToolParams{Name: "server_info"})
			if err == nil && result.IsError {
				err = errors.New(result.Content[0].(*sdk.TextContent).Text)
			}
			results <- err
		}()
	}
	for range 2 {
		if err := <-results; err != nil {
			log, _ := os.ReadFile(filepath.Join(dir, "coordinator.log"))
			t.Fatalf("adapter call: %v\n%s", err, log)
		}
	}
	var pid int
	call(t, ctx, client, "ping", nil, &pid)

	for _, invalid := range []struct {
		version, executable, errorPart string
	}{
		{"4.5.6", coordinator, "incompatible process version"},
		{version, worker, "executable identity mismatch"},
	} {
		other, err := protocol.NewClient(dir, invalid.version, invalid.executable)
		if err != nil {
			t.Fatal(err)
		}
		if err := other.Ensure(ctx); err == nil || !strings.Contains(err.Error(), invalid.errorPart) {
			t.Fatalf("expected %s, got %v", invalid.errorPart, err)
		}
	}

	grace := submit(t, ctx, client, dir, "grace")
	waitJob(t, ctx, client, grace.ID, "ready", jobs.Running)
	if err := a.Close(); err != nil {
		t.Fatal(err)
	}
	if err := b.Close(); err != nil {
		t.Fatal(err)
	}
	reconnected := startAdapter(t, ctx, adapter, dir)
	if result, err := reconnected.CallTool(ctx, &sdk.CallToolParams{Name: "server_info"}); err != nil || result.IsError {
		t.Fatalf("reconnect adapter: %v %v", result, err)
	}
	same := submit(t, ctx, client, dir, "grace")
	var stillPID int
	call(t, ctx, client, "ping", nil, &stillPID)
	if same.ID != grace.ID || same.Status != jobs.Running || stillPID != pid {
		t.Fatalf("disconnection changed coordinator or job: %d -> %d, %s/%s -> %s", pid, stillPID, grace.ID, same.ID, same.Status)
	}
	call(t, ctx, client, "jobs.cancel", grace.ID, nil)
	waitJob(t, ctx, client, grace.ID, "graceful_exit", jobs.Cancelled)

	forced := submit(t, ctx, client, dir, "force")
	waitJob(t, ctx, client, forced.ID, "ready", jobs.Running)
	call(t, ctx, client, "jobs.cancel", forced.ID, nil)
	waitJob(t, ctx, client, forced.ID, "ignoring_cancel", jobs.Cancelled)

	testCoordinatorRecovery(t, ctx, client, dir, pid)
}

func testCoordinatorRecovery(t *testing.T, ctx context.Context, client *protocol.Client, dir string, pid int) {
	t.Helper()
	durable := submit(t, ctx, client, dir, "durable")
	lost := submit(t, ctx, client, dir, "force_crash")
	waitJob(t, ctx, client, durable.ID, "durable", jobs.Running)
	waitJob(t, ctx, client, lost.ID, "ready", jobs.Running)
	if err := killCoordinator(pid); err != nil {
		t.Fatal(err)
	}
	if err := client.Ensure(ctx); err != nil {
		log, _ := os.ReadFile(filepath.Join(dir, "coordinator.log"))
		t.Fatalf("restart coordinator: %v\n%s", err, log)
	}
	var newPID int
	call(t, ctx, client, "ping", nil, &newPID)
	if newPID == pid {
		t.Fatalf("expected restarted coordinator, got %d", pid)
	}
	published := waitJob(t, ctx, client, durable.ID, "durable", jobs.Completed)
	if published.Result == nil || !published.Result.Confirmed {
		t.Fatal("restarted coordinator lost a durable result")
	}
	waitJob(t, ctx, client, lost.ID, "ready", jobs.Interrupted)
	retry := submit(t, ctx, client, dir, "force_crash")
	if retry.ID != lost.ID || retry.Status != jobs.Interrupted {
		t.Fatal("interrupted job automatically restarted")
	}
}

func call(t *testing.T, ctx context.Context, client *protocol.Client, method string, input, output any) {
	t.Helper()
	if err := client.Call(ctx, method, input, output); err != nil {
		t.Fatalf("%s: %v", method, err)
	}
}

func submit(t *testing.T, ctx context.Context, client *protocol.Client, dir, operation string) jobs.JobRecord {
	t.Helper()
	target := jobs.TargetRef{StateID: operation, Revision: 1}
	snapshot := filepath.Join(dir, operation+".json")
	if err := os.WriteFile(snapshot, []byte("{}\n"), 0600); err != nil {
		t.Fatal(err)
	}
	var result jobs.JobRecord
	call(t, ctx, client, "jobs.submit", jobs.SubmitRequest{
		RequestKey: operation, Operation: jobs.OperationRequest{Operation: operation, Target: target, Scope: []byte("{}"), Parameters: []byte("{}")},
		Source: jobs.SourceMCP, Access: jobs.AccessMutate,
		Snapshot: &jobs.SnapshotRef{ManifestPath: snapshot, Target: target},
	}, &result)
	return result
}

func waitJob(t *testing.T, ctx context.Context, client *protocol.Client, id jobs.JobID, stage string, status jobs.Status) jobs.JobRecord {
	t.Helper()
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	tick := time.NewTicker(25 * time.Millisecond)
	defer tick.Stop()
	for {
		var job jobs.JobRecord
		call(t, ctx, client, "jobs.get", id, &job)
		if job.Status == status && job.Progress.Stage == stage {
			return job
		}
		select {
		case <-ctx.Done():
			t.Fatalf("job %s: got %s/%s %v, want %s/%s", job.ID, job.Status, job.Progress.Stage, job.Error, status, stage)
		case <-tick.C:
		}
	}
}
