package mcp

import (
	"context"
	"encoding/json"
	"fmt"
	"slices"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestProjectGraphTools(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	defer cancel()
	target := jobs.TargetRef{StateID: "project", Revision: 7}
	snapshot := jobs.SnapshotRef{Target: target, ManifestPath: "/project/snapshot.json"}
	const pairs = `{"session_pairs":[{"from":0,"to":2}]}`
	const indices = `{"edge_indices":[1]}`
	const edge = `{"edge":{"from":{"scan_index":2,"session_index":0},"relative_pose":[0,0,0,0,0,0],"to":{"scan_index":1,"session_index":2},"weights":[1,1,1,1,1,1]}}`
	type graphCall struct {
		id, route, source, scope, parameters string
		access                               jobs.Access
	}
	tests := []graphCall{
		{"project.transform_scope", "states.execute", "live", `{"session_indices":[2]}`, `{"transform":[1,2,3,0,0,0]}`, ""},
		{"project.restore_poses", "jobs.submit", "snapshot", `{"session_indices":[2]}`, `{"source":"initial"}`, jobs.AccessMutate},
		{"project.register_sessions", "jobs.submit", "live", pairs, `{"iterations":3,"method":"ndt"}`, jobs.AccessMutate},
		{"optimize_project", "jobs.submit", "live", indices, `{}`, jobs.AccessMutate},
		{"project.constraints.list", "states.describe", "live", `{}`, `{}`, ""},
		{"project.constraints.list", "jobs.submit", "snapshot", `{}`, `{}`, jobs.AccessRead},
		{"project.constraints.add", "states.execute", "live", `{}`, edge, ""},
		{"project.constraints.update", "states.execute", "live", `{"edge_index":1}`, edge, ""},
		{"project.constraints.remove", "jobs.submit", "snapshot", `{"edge_index":1}`, `{}`, jobs.AccessMutate},
		{"project.constraints.replace", "jobs.submit", "live", indices, `{"edges":[]}`, jobs.AccessMutate},
		{"project.constraints.run_icp", "jobs.submit", "live", indices, `{"search_radius_m":0.25}`, jobs.AccessMutate},
		{"project.constraints.detect_candidates", "jobs.submit", "live", pairs, `{"max_distance_m":8}`, jobs.AccessRead},
	}
	var catalog []map[string]any
	for _, tt := range tests {
		catalog = append(catalog, map[string]any{"id": tt.id})
	}
	catalog = append(catalog, map[string]any{"id": "constraints.list"})
	srv := NewWithCoordinator("1.2.3", func(_ context.Context, method string, in, out any) error {
		switch method {
		case "operations.describe":
			*out.(*[]map[string]any) = catalog
			return nil
		case "states.describe":
			request := in.(describeState)
			step := 3
			if request.Target.StateID == "session" {
				step = 2
			}
			return decodeGraphTestOutput(out, map[string]any{
				"target": request.Target, "metadata": map[string]any{"step": step, "active_edges": []any{}},
			})
		case "states.execute":
			request := in.(jobs.OperationRequest)
			for _, tt := range tests {
				if tt.route == method && tt.id == request.Operation && request.Target == target &&
					string(request.Scope) == tt.scope && string(request.Parameters) == tt.parameters {
					*out.(*stateSummary) = stateSummary{
						Target:     jobs.TargetRef{StateID: target.StateID, Revision: target.Revision + 1},
						Parameters: map[string]any{}, Metadata: map[string]any{},
					}
					return nil
				}
			}
			return fmt.Errorf("unexpected live edit %+v", request)
		case "jobs.submit":
			request := in.(jobs.SubmitRequest)
			for _, tt := range tests {
				if tt.route != method || tt.id != request.Operation.Operation {
					continue
				}
				validSource := request.CaptureSource == target.StateID && request.Snapshot == nil
				if tt.source == "snapshot" {
					validSource = request.CaptureSource == "" && request.Snapshot != nil && *request.Snapshot == snapshot
				}
				if !validSource || request.Access != tt.access || request.Source != jobs.SourceMCP ||
					request.RequestKey != tt.id || request.Operation.Target != target ||
					string(request.Operation.Scope) != tt.scope || string(request.Operation.Parameters) != tt.parameters {
					return fmt.Errorf("unexpected route, source, access or arguments: %+v", request)
				}
				*out.(*jobs.JobRecord) = jobs.JobRecord{ID: "project-job", Request: request, Status: jobs.Preparing}
				return nil
			}
			return fmt.Errorf("unexpected job %+v", request)
		default:
			return fmt.Errorf("unexpected RPC %s", method)
		}
	})
	client := projectGraphTestClient(t, ctx, srv)
	for _, tt := range tests {
		t.Run(tt.id+"/"+tt.source, func(t *testing.T) {
			args := map[string]any{"scope": json.RawMessage(tt.scope), "parameters": json.RawMessage(tt.parameters)}
			if tt.source == "live" {
				args["target"] = target
			} else {
				args["snapshot"] = snapshot
			}
			if tt.route == "jobs.submit" {
				args["request_key"] = tt.id
			}
			result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: tt.id, Arguments: args})
			if err != nil || result.IsError {
				t.Fatalf("%s failed: %v (%+v)", tt.id, err, result)
			}
			var output struct {
				JobID       string          `json:"job_id"`
				Job         *step1Job       `json:"job"`
				State       *stateSummary   `json:"state"`
				Target      *jobs.TargetRef `json:"target"`
				ActiveEdges []any           `json:"active_edges"`
			}
			if err := decodeGraphTestOutput(&output, result.StructuredContent); err != nil {
				t.Fatal(err)
			}
			switch tt.route {
			case "states.execute":
				if output.State == nil || output.State.Target.Revision != target.Revision+1 {
					t.Fatalf("missing updated live state: %+v", output)
				}
			case "states.describe":
				if output.Target == nil || *output.Target != target || output.ActiveEdges == nil {
					t.Fatalf("missing project edges: %+v", output)
				}
			case "jobs.submit":
				if output.JobID != "project-job" && (output.Job == nil || output.Job.ID != "project-job") {
					t.Fatalf("missing durable job: %+v", output)
				}
			}
		})
	}

	result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "describe_project_operations", Arguments: map[string]any{}})
	if err != nil || result.IsError {
		t.Fatalf("project catalog failed: %v (%+v)", err, result)
	}
	var described operationCatalog
	if err := decodeGraphTestOutput(&described, result.StructuredContent); err != nil {
		t.Fatal(err)
	}
	for _, tt := range tests {
		if !slices.ContainsFunc(described.Operations, func(entry map[string]any) bool { return entry["id"] == tt.id }) {
			t.Errorf("project catalog omitted %s", tt.id)
		}
	}
	if slices.ContainsFunc(described.Operations, func(entry map[string]any) bool { return entry["id"] == "constraints.list" }) {
		t.Error("project catalog contains a Step 2 operation")
	}

	session := jobs.TargetRef{StateID: "session", Revision: 7}
	for _, tt := range []struct {
		id        string
		arguments map[string]any
	}{
		{"project.constraints.list", map[string]any{"target": session}},
		{"constraints.list", map[string]any{"target": target}},
		{"project.constraints.list", map[string]any{"snapshot": snapshot}},
		{"project.constraints.list", map[string]any{"target": target, "scope": map[string]any{"edge_index": 1}}},
		{"optimize_project", map[string]any{"target": target}},
	} {
		result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: tt.id, Arguments: tt.arguments})
		if err == nil && !result.IsError {
			t.Errorf("%s accepted invalid arguments %v", tt.id, tt.arguments)
		}
	}
	result, err = client.CallTool(ctx, &sdk.CallToolParams{Name: "constraints.list", Arguments: map[string]any{"target": session}})
	if err != nil || result.IsError {
		t.Fatalf("Step 2 list failed: %v (%+v)", err, result)
	}
	tools, err := client.ListTools(ctx, nil)
	if err != nil {
		t.Fatal(err)
	}
	for _, tool := range tools.Tools {
		switch tool.Name {
		case "project.constraints.list", "project.constraints.detect_candidates":
			if tool.Annotations == nil || !tool.Annotations.ReadOnlyHint {
				t.Errorf("%s lacks its read-only annotation", tool.Name)
			}
		case "project.constraints.run_icp", "optimize_project", "project.register_sessions":
			if tool.Annotations != nil && tool.Annotations.ReadOnlyHint {
				t.Errorf("%s has a read-only annotation", tool.Name)
			}
		}
	}
}

func projectGraphTestClient(t *testing.T, ctx context.Context, srv *sdk.Server) *sdk.ClientSession {
	t.Helper()
	serverTransport, clientTransport := sdk.NewInMemoryTransports()
	session, err := srv.Connect(ctx, serverTransport, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := session.Close(); err != nil {
			t.Error(err)
		}
	})
	client, err := sdk.NewClient(&sdk.Implementation{Name: "project-graph-test", Version: "1"}, nil).Connect(ctx, clientTransport, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := client.Close(); err != nil {
			t.Error(err)
		}
	})
	return client
}

func decodeGraphTestOutput(destination, value any) error {
	raw, err := json.Marshal(value)
	if err != nil {
		return err
	}
	return json.Unmarshal(raw, destination)
}
