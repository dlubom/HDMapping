package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
	"github.com/google/jsonschema-go/jsonschema"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

// Exercise the shipped executable, including its version injection and stdout.
func TestStdioProduct(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	cmake, err := os.ReadFile(filepath.Join("..", "..", "..", "..", "CMakeLists.txt"))
	if err != nil {
		t.Fatal(err)
	}
	match := regexp.MustCompile(`project\(hd-mapping VERSION ([0-9.]+)\)`).FindSubmatch(cmake)
	if len(match) != 2 {
		t.Fatal("product version not found in top-level CMake")
	}
	version := string(match[1])
	executable := os.Getenv("HDMAPPING_MCP_EXECUTABLE")
	shipped := executable != ""
	if !shipped {
		executable, _, _ = buildTestProduct(t, ctx, version, "")
	}
	dir, err := protocol.ValidateDirectory(filepath.Join(t.TempDir(), "stan żółć"))
	if err != nil {
		t.Fatal(err)
	}
	coordinator, err := protocol.Companion(executable, "hdmapping_mcp", "hdmapping_coordinator")
	if err != nil {
		t.Fatal(err)
	}
	local, err := protocol.NewClient(dir, version, coordinator)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { stopCoordinator(t, local) })

	cmd := exec.CommandContext(ctx, executable)
	// The installed product must run without Go, Python, Node or the source cwd.
	cmd.Dir = t.TempDir()
	cmd.Env = append(os.Environ(), "PATH=", "GORACE=atexit_sleep_ms=0 halt_on_error=1", "HDMAPPING_MCP_STATE_DIR="+dir)
	var wire, logs bytes.Buffer
	cmd.Stderr = &logs
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	stdin, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if cmd.ProcessState == nil {
			cancel()
			_ = cmd.Wait()
		}
	})
	client, err := sdk.NewClient(&sdk.Implementation{Name: "stdio-test", Version: "1"}, nil).Connect(ctx,
		&sdk.IOTransport{Reader: io.NopCloser(io.TeeReader(stdout, &wire)), Writer: stdin}, nil)
	if err != nil {
		t.Fatal(err)
	}
	result, callErr := client.CallTool(ctx, &sdk.CallToolParams{Name: "server_info", Arguments: map[string]any{}})
	catalogResult, catalogErr := client.CallTool(ctx, &sdk.CallToolParams{Name: "describe_step1_operations", Arguments: map[string]any{}})
	if catalogErr != nil || catalogResult.IsError {
		t.Fatalf("call Step 1 catalog: %v (%v)", catalogErr, catalogResult)
	}
	step2Catalog, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "describe_step2_operations", Arguments: map[string]any{}})
	if err != nil || step2Catalog.IsError {
		t.Fatalf("call Step 2 catalog: %v (%v)", err, step2Catalog)
	}
	rawCatalog, err := json.Marshal(step2Catalog.StructuredContent)
	if err != nil {
		t.Fatal(err)
	}
	var step2 struct {
		Operations []struct{ ID string }
	}
	if err := json.Unmarshal(rawCatalog, &step2); err != nil {
		t.Fatalf("unexpected Step 2 catalog: %s (%v)", rawCatalog, err)
	}
	var step2IDs []string
	for _, operation := range step2.Operations {
		step2IDs = append(step2IDs, operation.ID)
	}
	slices.Sort(step2IDs)
	if !slices.Equal(step2IDs, []string{"step2.align_reference", "step2.edit_segment", "step2.optimize_graph", "step2.register_scans", "step2.restore_poses", "step2.transform_scope"}) {
		t.Fatalf("unexpected Step 2 operations: %v", step2IDs)
	}

	constraintCatalog, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "describe_constraints_operations", Arguments: map[string]any{}})
	if err != nil || constraintCatalog.IsError {
		t.Fatalf("call constraints catalog: %v (%v)", err, constraintCatalog)
	}
	for _, name := range []string{"step2.edit_segment", "step2.restore_poses", "step2.transform_scope", "constraints.list", "constraints.add", "constraints.update", "constraints.remove", "constraints.replace", "constraints.run_icp", "step2.optimize_graph"} {
		invalid, err := client.CallTool(ctx, &sdk.CallToolParams{Name: name, Arguments: map[string]any{}})
		if err != nil || !invalid.IsError {
			t.Fatalf("%s must reject a missing target: %v (%v)", name, err, invalid)
		}
	}

	georefCatalog, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "describe_georeference_operations", Arguments: map[string]any{}})
	if err != nil || georefCatalog.IsError {
		t.Fatalf("call georeference catalog: %v (%v)", err, georefCatalog)
	}
	rawGeoref, err := json.Marshal(georefCatalog.StructuredContent)
	if err != nil {
		t.Fatal(err)
	}
	var georef struct {
		Operations []struct{ ID string }
	}
	if err := json.Unmarshal(rawGeoref, &georef); err != nil {
		t.Fatalf("decode georeference catalog: %v (%s)", err, rawGeoref)
	}
	var georefIDs []string
	for _, operation := range georef.Operations {
		georefIDs = append(georefIDs, operation.ID)
	}
	slices.Sort(georefIDs)
	wantGeoref := []string{"cp.import", "cp.remove", "cp.update", "gcp.import", "gcp.remove", "gcp.update", "gnss.import", "gnss.project", "step2.align_reference", "tum.import"}
	if !slices.Equal(georefIDs, wantGeoref) {
		t.Fatalf("unexpected georeference operations: %v", georefIDs)
	}
	for _, name := range wantGeoref {
		invalid, err := client.CallTool(ctx, &sdk.CallToolParams{Name: name, Arguments: map[string]any{}})
		if err != nil || !invalid.IsError {
			t.Fatalf("%s must reject a missing target: %v (%v)", name, err, invalid)
		}
	}

	if shipped {
		checkInstalledCatalog(t, ctx, client)
	}

	if shipped {
		input := map[string]any{"request_key": "empty-step1", "recording": map[string]string{"directory": t.TempDir()}}
		operation, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "step1_odometry", Arguments: input})
		if err != nil || operation.IsError {
			t.Fatalf("submit empty Step 1 recording: %v (%v)", err, operation)
		}
		raw, err := json.Marshal(operation.StructuredContent)
		if err != nil {
			t.Fatal(err)
		}
		var job jobs.JobRecord
		if err := json.Unmarshal(raw, &job); err != nil || job.ID == "" {
			t.Fatalf("decode Step 1 submission: %v (%s)", err, raw)
		}
		rejected := waitJob(t, ctx, local, job.ID, "", jobs.Failed)
		if rejected.Error == nil || rejected.Error.OperationCode != "invalid_input" || rejected.Result != nil {
			t.Fatalf("expected a structured Step 1 input error: %+v", rejected)
		}
	}
	info := client.InitializeResult()
	closeErr := client.Close()
	waitErr := cmd.Wait()
	if callErr != nil || closeErr != nil || waitErr != nil {
		t.Fatalf("call=%v close=%v process=%v stderr=%s", callErr, closeErr, waitErr, logs.String())
	}
	if info.ServerInfo.Version != version {
		t.Fatalf("CMake version %s, initialize reports %s", version, info.ServerInfo.Version)
	}
	data, err := json.Marshal(result.StructuredContent)
	if err != nil {
		t.Fatal(err)
	}
	var content struct {
		Version string `json:"version"`
	}
	if err := json.Unmarshal(data, &content); err != nil {
		t.Fatal(err)
	}
	if result.IsError || content.Version != version {
		t.Fatalf("wrong product result: %s; tool content %v", data, result.Content)
	}
	var catalog struct {
		Operations []struct{ ID string }
	}
	data, err = json.Marshal(catalogResult.StructuredContent)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, &catalog); err != nil {
		t.Fatal(err)
	}
	var ids []string
	for _, operation := range catalog.Operations {
		ids = append(ids, operation.ID)
	}
	sort.Strings(ids)
	if strings.Join(ids, " ") != "step1.consistency step1.export step1.load step1.odometry step1.prepare step1.reference step1.save" {
		t.Fatalf("released worker has wrong operations: %v", ids)
	}
	t.Log("adapter, coordinator and worker started from shipped layout")
	if logs.Len() == 0 {
		t.Fatal("startup diagnostic missing on stderr")
	}
	lines := strings.Split(strings.TrimSpace(wire.String()), "\n")
	if len(lines) < 2 {
		t.Fatalf("missing protocol output: %q", wire.String())
	}
	for _, line := range lines {
		var message struct {
			JSONRPC string `json:"jsonrpc"`
		}
		if err := json.Unmarshal([]byte(line), &message); err != nil || message.JSONRPC != "2.0" {
			t.Fatalf("non-protocol stdout: %q", line)
		}
	}
}

func checkInstalledCatalog(t *testing.T, ctx context.Context, client *sdk.ClientSession) {
	t.Helper()
	result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "describe_operations"})
	if err != nil || result.IsError {
		t.Fatalf("read installed C++ registry: %v (%v)", err, result)
	}
	data, err := json.Marshal(result.StructuredContent)
	if err != nil {
		t.Fatal(err)
	}
	var catalog struct {
		Operations []struct {
			ID         string          `json:"id"`
			Tool       string          `json:"tool"`
			Class      string          `json:"class"`
			Scope      json.RawMessage `json:"scope_schema"`
			Parameters json.RawMessage `json:"parameters_schema"`
			Result     map[string]any  `json:"result_schema"`
		}
	}
	if err := json.Unmarshal(data, &catalog); err != nil || len(catalog.Operations) < 40 {
		t.Fatalf("decode installed operations: %d (%v)", len(catalog.Operations), err)
	}
	tools := make(map[string]*sdk.Tool)
	for tool, err := range client.Tools(ctx, nil) {
		if err != nil {
			t.Fatal(err)
		}
		tools[tool.Name] = tool
	}
	seen := make(map[string]bool)
	for _, operation := range catalog.Operations {
		tool := tools[operation.Tool]
		if operation.ID == "" || seen[operation.ID] || tool == nil || operation.Class == "" ||
			len(operation.Scope) == 0 || len(operation.Parameters) == 0 || operation.Result["properties"] == nil {
			t.Fatalf("incomplete C++ operation mapping: %+v", operation)
		}
		seen[operation.ID] = true
		input, err := json.Marshal(tool.InputSchema)
		if err != nil || !bytes.Contains(input, operation.Scope) || (operation.ID != "project.list_items" && !bytes.Contains(input, operation.Parameters)) {
			t.Fatalf("%s does not advertise the native input schema for %s: %v", operation.Tool, operation.ID, err)
		}
	}
	if !seen["project.export"] || !seen["project.list_items"] {
		t.Fatal("project export is absent from the installed registry")
	}
	checkExportSchema(t, tools["export_result"])
	t.Logf("%d C++ operations with MCP tools and matching argument schemas", len(seen))
}

func checkExportSchema(t *testing.T, tool *sdk.Tool) {
	t.Helper()
	data, err := json.Marshal(tool.InputSchema)
	if err != nil {
		t.Fatal(err)
	}
	var schema jsonschema.Schema
	if err := json.Unmarshal(data, &schema); err != nil {
		t.Fatal(err)
	}
	resolved, err := schema.Resolve(nil)
	if err != nil {
		t.Fatal(err)
	}
	projectScope := map[string]any{"sessions": []any{map[string]any{"session_index": 0}}}
	for _, test := range []struct {
		name   string
		scope  map[string]any
		params map[string]any
		valid  bool
	}{
		{"complete", nil, map[string]any{"format": "project"}, true},
		{"extra-selection", projectScope, map[string]any{"format": "project"}, false},
		{"missing-selection", nil, map[string]any{"format": "trajectory_tum"}, false},
		{"partial-section", projectScope, map[string]any{"format": "section_laz"}, false},
		{"section", projectScope, map[string]any{"format": "section_laz", "plane": "xy", "width_m": 1.0}, true},
		{"existing-session", map[string]any{"scan_indices": []any{0}}, map[string]any{"format": "laz"}, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			args := map[string]any{"request_key": "schema-check", "target": map[string]any{"state_id": "test", "revision": 1}, "parameters": test.params}
			if test.scope != nil {
				args["scope"] = test.scope
			}
			if err := resolved.Validate(args); (err == nil) != test.valid {
				t.Fatalf("advertised export contract validity = %v, want %v (%v)", err == nil, test.valid, err)
			}
		})
	}
}
