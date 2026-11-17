package supervisor

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

func TestReceiptRequiresMaterializedManifest(t *testing.T) {
	dir := t.TempDir()
	target := jobs.TargetRef{StateID: "input", Revision: 1}
	job := jobs.JobRecord{ID: "receipt-job", Request: jobs.SubmitRequest{
		Operation: jobs.OperationRequest{Target: target},
	}}
	result := receipt{ID: job.ID, Version: "1.2.3", Wire: protocol.WireVersion}
	result.Manifest.ID = "output"
	result.Manifest.Target = target
	result.Manifest.Snapshot = jobs.SnapshotRef{
		ManifestPath: "missing.json", Target: jobs.TargetRef{StateID: "output"},
	}
	result.Manifest.Files = []string{"missing.json"}
	result.Manifest.Dependencies = []string{}
	data, err := json.Marshal(result)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "result.json"), data, 0600); err != nil {
		t.Fatal(err)
	}
	if got, err := readReceipt(dir, "1.2.3", job); err == nil || got.Confirmed {
		t.Fatalf("missing snapshot was treated as published: %v %v", got, err)
	}
	if err := os.WriteFile(filepath.Join(dir, "missing.json"), []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	if got, err := readReceipt(dir, "1.2.3", job); err != nil || !got.Confirmed {
		t.Fatalf("materialized result was rejected: %v %v", got, err)
	}
}
