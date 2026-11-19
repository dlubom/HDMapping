package main

import (
	"context"
	"encoding/json"
	"path/filepath"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestStep1RecordingJob(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 60*time.Second)
	defer cancel()
	const version = "1.2.3"
	adapter, coordinator, _ := buildTestProduct(t, ctx, version, "STEP1-")
	dir, err := protocol.ValidateDirectory(filepath.Join(t.TempDir(), "step1 stan"))
	if err != nil {
		t.Fatal(err)
	}
	local, err := protocol.NewClient(dir, version, coordinator)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { stopCoordinator(t, local) })
	client := startAdapter(t, ctx, adapter, dir)
	recording := t.TempDir()
	request := map[string]any{
		"request_key": "parking-odometry",
		"recording":   map[string]any{"directory": recording, "toml": filepath.Join(recording, "parking.toml")},
		"scope":       map[string]any{"save": false},
	}

	tool := func(name string, args any) jobs.JobRecord {
		t.Helper()
		result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: name, Arguments: args})
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if result.IsError {
			t.Fatalf("%s: %s", name, result.Content[0].(*sdk.TextContent).Text)
		}
		raw, err := json.Marshal(result.StructuredContent)
		if err != nil {
			t.Fatal(err)
		}
		var job jobs.JobRecord
		if err := json.Unmarshal(raw, &job); err != nil || job.ID == "" {
			t.Fatalf("invalid %s job %s: %v", name, raw, err)
		}
		return job
	}
	admitted := tool("step1_odometry", request)
	if duplicate := tool("step1_odometry", request); duplicate.ID != admitted.ID {
		t.Fatalf("duplicate job: %+v", duplicate)
	}
	job := waitJob(t, ctx, local, admitted.ID, "ready", jobs.Running)
	if job.Snapshot == nil || filepath.Dir(job.Snapshot.ManifestPath) != filepath.Join(dir, "inputs-"+string(job.ID)) {
		t.Fatalf("missing recorded capture: %+v", job)
	}
	args := map[string]any{"job_id": job.ID}
	if err := client.Close(); err != nil {
		t.Fatal(err)
	}
	client = startAdapter(t, ctx, adapter, dir)
	if observed := tool("get_job", args); observed.Status != jobs.Running || observed.Snapshot == nil {
		t.Fatalf("job lost after reconnect: %+v", observed)
	}
	tool("cancel_job", args)
	waitJob(t, ctx, local, job.ID, "graceful_exit", jobs.Cancelled)
	if observed := tool("get_job", args); observed.Status != jobs.Cancelled || observed.Result != nil {
		t.Fatalf("unexpected cancellation result: %+v", observed)
	}

	request["request_key"] = "parking-prepare"
	delete(request, "scope")
	preparing := tool("step1_prepare", request)
	waitJob(t, ctx, local, preparing.ID, "durable", jobs.Completed)
	result := tool("get_job", map[string]any{"job_id": preparing.ID}).Result
	if result == nil || result.Snapshot == nil || filepath.Dir(result.Snapshot.ManifestPath) != result.OutputDirectory {
		t.Fatalf("preparation returned no reusable snapshot: %+v", result)
	}
	compute := tool("step1_odometry", map[string]any{"request_key": "from-prepared", "snapshot": result.Snapshot})
	waitJob(t, ctx, local, compute.ID, "ready", jobs.Running)
	tool("cancel_job", map[string]any{"job_id": compute.ID})
	waitJob(t, ctx, local, compute.ID, "graceful_exit", jobs.Cancelled)
}
