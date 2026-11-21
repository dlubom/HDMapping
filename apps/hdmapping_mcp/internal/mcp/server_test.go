package mcp

import (
	"context"
	"encoding/json"
	"fmt"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/google/jsonschema-go/jsonschema"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestServerContract(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	serverTransport, clientTransport := sdk.NewInMemoryTransports()
	serverSession, err := New("9.8.7").Connect(ctx, serverTransport, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := serverSession.Close(); err != nil {
			t.Error(err)
		}
	}()
	client, err := sdk.NewClient(&sdk.Implementation{Name: "contract-test", Version: "1"}, nil).Connect(ctx, clientTransport, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := client.Close(); err != nil {
			t.Error(err)
		}
	}()
	info := client.InitializeResult()
	if info.ServerInfo.Name != "HDMapping" || info.ServerInfo.Version != "9.8.7" {
		t.Fatalf("wrong identity: %+v", info.ServerInfo)
	}
	caps, err := json.Marshal(info.Capabilities)
	if err != nil {
		t.Fatal(err)
	}
	if string(caps) != `{"tools":{}}` {
		t.Fatalf("unimplemented capabilities: %s", caps)
	}
	list, err := client.ListTools(ctx, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(list.Tools) != 1 || list.Tools[0].Name != "server_info" {
		t.Fatalf("wrong tools: %+v", list.Tools)
	}
	tool := list.Tools[0]
	for name, schema := range map[string]any{"input": tool.InputSchema, "output": tool.OutputSchema} {
		encoded, err := json.Marshal(schema)
		if err != nil {
			t.Fatal(err)
		}
		var s jsonschema.Schema
		if err := json.Unmarshal(encoded, &s); err != nil {
			t.Fatal(err)
		}
		if s.Type != "object" {
			t.Fatalf("%s schema is not an object", name)
		}
		if _, err := s.Resolve(nil); err != nil {
			t.Fatalf("%s schema: %v", name, err)
		}
	}
	result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "server_info", Arguments: map[string]any{}})
	if err != nil {
		t.Fatal(err)
	}
	if result.IsError {
		t.Fatalf("server_info failed: %+v", result)
	}
	data, err := json.Marshal(result.StructuredContent)
	if err != nil {
		t.Fatal(err)
	}
	var got struct {
		Name         string   `json:"name"`
		Version      string   `json:"version"`
		Capabilities []string `json:"capabilities"`
	}
	if err := json.Unmarshal(data, &got); err != nil {
		t.Fatal(err)
	}
	if got.Name != "HDMapping" || got.Version != "9.8.7" || len(got.Capabilities) != 1 || got.Capabilities[0] != "server_info" {
		t.Fatalf("wrong result: %s", data)
	}
	for _, args := range []any{map[string]any{"unexpected": true}, []any{1}, "bad"} {
		result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "server_info", Arguments: args})
		if err == nil && !result.IsError {
			t.Fatalf("invalid arguments accepted: %#v", args)
		}
	}
	if _, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "run_odometry", Arguments: map[string]any{}}); err == nil {
		t.Fatal("unimplemented tool accepted")
	}
}

func TestGetJobPreservesWorkerErrorDetails(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	defer cancel()
	srv := NewWithCoordinator("1.2.3", func(_ context.Context, method string, in, out any) error {
		if method != "jobs.get" || in != jobs.JobID("projection-job") {
			return fmt.Errorf("unexpected request %s, %v", method, in)
		}
		*out.(*jobs.JobRecord) = jobs.JobRecord{
			ID: "projection-job", Status: jobs.Failed, Error: &jobs.JobError{
				Code: "execution_failed", OperationCode: "missing_geoid", Message: "grid file unavailable",
				Details: json.RawMessage(`{"geoid":"missing-grid.gtx"}`),
			},
		}
		return nil
	})
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
	client, err := sdk.NewClient(&sdk.Implementation{Name: "contract-test", Version: "1"}, nil).Connect(ctx, clientTransport, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := client.Close(); err != nil {
			t.Error(err)
		}
	})
	result, err := client.CallTool(ctx, &sdk.CallToolParams{Name: "get_job", Arguments: map[string]any{"job_id": "projection-job"}})
	if err != nil || result.IsError {
		t.Fatalf("get_job rejected worker error details: %v (%v)", err, result)
	}
	data, err := json.Marshal(result.StructuredContent)
	if err != nil {
		t.Fatal(err)
	}
	var response jobs.JobRecord
	if err := json.Unmarshal(data, &response); err != nil || response.Status != jobs.Failed || response.Error == nil ||
		response.Error.OperationCode != "missing_geoid" || string(response.Error.Details) != `{"geoid":"missing-grid.gtx"}` {
		t.Fatalf("get_job lost the structured worker error: %s (%v)", data, err)
	}
}
