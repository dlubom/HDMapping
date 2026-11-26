package mcp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestProjectExportCatalogAndRouting(t *testing.T) {
	ctx := t.Context()
	catalog := []map[string]any{
		{"id": "project.export", "scope_schema": map[string]any{"type": "object"}, "parameters_schema": map[string]any{"type": "object"}, "class": "heavy"},
		{"id": "export_result", "scope_schema": map[string]any{"type": "object"}, "parameters_schema": map[string]any{"type": "object"}, "class": "heavy"},
	}
	server := NewWithCoordinator("test", func(_ context.Context, method string, in, out any) error {
		switch method {
		case "operations.describe":
			return decodeGraphTestOutput(out, catalog)
		case "jobs.submit":
			request := in.(jobs.SubmitRequest)
			if request.Operation.Operation != "project.export" || request.Access != jobs.AccessRead {
				return fmt.Errorf("unexpected export route: %+v", request)
			}
			*out.(*jobs.JobRecord) = jobs.JobRecord{ID: "project-export-job", Request: request, Status: jobs.Preparing}
			return nil
		default:
			return fmt.Errorf("unexpected method %s", method)
		}
	})
	client := projectGraphTestClient(t, ctx, server)
	result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "export_result", Arguments: map[string]any{
		"request_key": "project-export", "target": jobs.TargetRef{StateID: "project", Revision: 7},
		"parameters": map[string]any{"format": "project"},
	}})
	if err != nil || result.IsError {
		t.Fatalf("project export: %v (%+v)", err, result)
	}
	var output step1Job
	if err := decodeGraphTestOutput(&output, result.StructuredContent); err != nil || output.ID != "project-export-job" {
		t.Fatalf("project export result: %+v (%v)", output, err)
	}
	description, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "describe_project_operations"})
	if err != nil || description.IsError {
		t.Fatalf("project catalog: %v (%+v)", err, description)
	}
	var got operationCatalog
	if err := decodeGraphTestOutput(&got, description.StructuredContent); err != nil || len(got.Operations) != 1 || got.Operations[0]["id"] != "project.export" {
		t.Fatalf("project catalog: %+v (%v)", got, err)
	}
}

func TestProjectListingAndPublishedResources(t *testing.T) {
	const id jobs.JobID = "000000000000000000000000000000ab"
	ctx := t.Context()
	target := jobs.TargetRef{StateID: "large-project", Revision: 12}
	dir := t.TempDir()
	snapshot := jobs.SnapshotRef{Target: target, ManifestPath: filepath.Join(dir, "snapshot.json")}
	paths := []string{"constraints.json", "geometry.laz"}
	if err := os.WriteFile(filepath.Join(dir, paths[0]), []byte(`{"candidates":[]}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, paths[1]), []byte("POINTS"), 0600); err != nil {
		t.Fatal(err)
	}
	receiptPath := filepath.Join(dir, "result.json")
	data, err := json.Marshal(map[string]any{"job_id": id, "manifest": map[string]any{
		"artifact_id": "result", "target": target, "snapshot": snapshot, "files": paths,
	}})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(receiptPath, data, 0600); err != nil {
		t.Fatal(err)
	}
	server := NewWithCoordinator("test", func(_ context.Context, method string, in, out any) error {
		switch method {
		case "states.list_project_items":
			input := in.(projectPageInput)
			if input.Target == nil || *input.Target != target || input.Scope.Kind != "sessions" || input.Scope.Offset != 900 || input.Scope.Limit != 50 {
				return fmt.Errorf("invalid project page request: %+v", input)
			}
			next, total := 950, 2000
			items := []map[string]any{{"index": 900, "scans": 95}}
			*out.(*projectPage) = projectPage{Target: &target, Total: &total, NextOffset: &next, Items: &items}
			return nil
		case "jobs.get":
			if in != id {
				return errors.New("unexpected job id")
			}
			*out.(*jobs.JobRecord) = jobs.JobRecord{ID: id, Status: jobs.Completed,
				Request: jobs.SubmitRequest{Operation: jobs.OperationRequest{Target: target}},
				Result:  &jobs.Publication{Confirmed: true, OutputDirectory: dir, ManifestPath: receiptPath, Snapshot: &snapshot}}
			return nil
		default:
			return fmt.Errorf("unexpected method %s", method)
		}
	})
	client := projectGraphTestClient(t, ctx, server)
	result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "list_project_items", Arguments: map[string]any{
		"target": target, "scope": map[string]any{"kind": "sessions", "offset": 900},
	}})
	if err != nil || result.IsError {
		t.Fatalf("project page: %v (%+v)", err, result)
	}
	var page projectPage
	if err := decodeGraphTestOutput(&page, result.StructuredContent); err != nil || page.Total == nil || *page.Total != 2000 || page.NextOffset == nil || *page.NextOffset != 950 {
		t.Fatalf("project page: %+v (%v)", page, err)
	}
	result, err = client.CallTool(ctx, &sdk.CallToolParams{Name: "list_job_artifacts", Arguments: map[string]any{"job_id": id, "limit": 1}})
	if err != nil || result.IsError {
		t.Fatalf("artifact page: %v (%+v)", err, result)
	}
	var artifacts artifactPage
	if err := decodeGraphTestOutput(&artifacts, result.StructuredContent); err != nil || len(artifacts.Items) != 1 || artifacts.NextOffset == nil || *artifacts.NextOffset != 1 {
		t.Fatalf("artifact page: %+v (%v)", artifacts, err)
	}
	report, err := client.ReadResource(ctx, &sdk.ReadResourceParams{URI: artifacts.Items[0].ReportURI})
	if err != nil || len(report.Contents) != 1 || report.Contents[0].Text != `{"candidates":[]}` {
		t.Fatalf("report: %+v (%v)", report, err)
	}
	cloud, err := client.ReadResource(ctx, &sdk.ReadResourceParams{URI: jobResourceURI(id, "artifact/1")})
	if err != nil || len(cloud.Contents) != 1 || !strings.Contains(cloud.Contents[0].Text, "geometry.laz") || strings.Contains(cloud.Contents[0].Text, "POINTS") {
		t.Fatalf("cloud metadata: %+v (%v)", cloud, err)
	}
	if _, err := client.ReadResource(ctx, &sdk.ReadResourceParams{URI: jobResourceURI(id, "report/1")}); err == nil {
		t.Fatal("cloud bytes were accessible as a report")
	}
	if _, err := client.ReadResource(ctx, &sdk.ReadResourceParams{URI: jobResourceURI(id, "artifact/2")}); err == nil {
		t.Fatal("invalid artifact index was accepted")
	}
}
