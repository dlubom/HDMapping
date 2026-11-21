package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/supervisor"
)

func TestReferenceImportUsesPreparedBytes(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 60*time.Second)
	defer cancel()
	const version = "1.2.3"
	_, coordinator, worker := buildTestProduct(t, ctx, version, "P10-")
	for _, operation := range []string{"gnss.import", "tum.import"} {
		t.Run(operation, func(t *testing.T) {
			dir := t.TempDir()
			supervisor := supervisor.Supervisor{Directory: dir, Coordinator: coordinator, Worker: worker, Version: version}
			target := jobs.TargetRef{StateID: "georeference-fixture", Revision: 7}
			snapshot := jobs.SnapshotRef{Target: target, ManifestPath: filepath.Join(dir, "snapshot.json")}
			if err := os.WriteFile(snapshot.ManifestPath, []byte(`{"fixture_complete":true}`), 0600); err != nil {
				t.Fatal(err)
			}
			source := filepath.Join(dir, "reference źródłowa.txt")
			if err := os.WriteFile(source, []byte("recorded reference\n"), 0600); err != nil {
				t.Fatal(err)
			}
			params, err := json.Marshal(map[string]any{"paths": []string{source}})
			if err != nil {
				t.Fatal(err)
			}
			job := jobs.JobRecord{
				ID: "0123456789abcdef0123456789abcdef",
				Request: jobs.SubmitRequest{
					Snapshot: &snapshot,
					Operation: jobs.OperationRequest{
						Operation: operation, Target: target, Parameters: bytes.Clone(params),
					},
				},
			}
			prepared, err := supervisor.Prepare(ctx, job)
			if err != nil {
				t.Fatalf("prepare import: %v", err)
			}
			job.Snapshot = &prepared
			if err := os.Remove(source); err != nil {
				t.Fatal(err)
			}
			result, err := supervisor.Run(ctx, job)
			if err != nil || !result.Confirmed {
				t.Fatalf("run after removing the source: %v, %v", result, err)
			}
			if !bytes.Equal(params, job.Request.Operation.Parameters) {
				t.Fatalf("journal operation parameters changed: %s", job.Request.Operation.Parameters)
			}
		})
	}
}
