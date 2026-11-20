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
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
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
	if err := json.Unmarshal(rawCatalog, &step2); err != nil || len(step2.Operations) != 1 || step2.Operations[0].ID != "step2.register_scans" {
		t.Fatalf("unexpected Step 2 catalog: %s (%v)", rawCatalog, err)
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
