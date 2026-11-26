// Package mcp exposes the implemented HDMapping operations through MCP.
package mcp

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

type serverInfo struct {
	Name         string   `json:"name" jsonschema:"Product name"`
	Version      string   `json:"version" jsonschema:"HDMapping product version"`
	Capabilities []string `json:"capabilities" jsonschema:"Implemented operation names"`
}

// New creates the server with the version supplied by the product build.
func New(version string) *sdk.Server {
	return NewWithCoordinator(version, nil)
}

// NewWithCoordinator exposes live tools through the private product coordinator.
func NewWithCoordinator(version string, call func(context.Context, string, any, any) error) *sdk.Server {
	server := sdk.NewServer(&sdk.Implementation{Name: "HDMapping", Version: version},
		&sdk.ServerOptions{Capabilities: &sdk.ServerCapabilities{Tools: &sdk.ToolCapabilities{}}})
	capabilities := []string{"server_info"}
	if call != nil {
		capabilities = append(capabilities, "list_open_states", "describe_state", "set_state_parameters")
		sdk.AddTool(server, &sdk.Tool{Name: "list_open_states", Description: "List live HDMapping GUI openings and their current revisions.", Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true}},
			func(ctx context.Context, _ *sdk.CallToolRequest, _ struct{}) (*sdk.CallToolResult, openStates, error) {
				var out openStates
				err := call(ctx, "states.list", nil, &out)
				return nil, out, err
			})
		sdk.AddTool(server, &sdk.Tool{Name: "describe_state", Description: "Read parameters and metadata from an open GUI at the specified revision.", Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true}},
			func(ctx context.Context, _ *sdk.CallToolRequest, input describeState) (*sdk.CallToolResult, stateSummary, error) {
				var out stateSummary
				err := call(ctx, "states.describe", input, &out)
				return nil, out, err
			})
		sdk.AddTool(server, &sdk.Tool{Name: "set_state_parameters", Description: "Update processing parameters in an open GUI. Use the application step and the exact revision of the target opening."},
			func(ctx context.Context, _ *sdk.CallToolRequest, input setParameters) (*sdk.CallToolResult, stateSummary, error) {
				var out stateSummary
				switch input.Step {
				case "step1", "step2", "step3":
				default:
					return nil, out, errors.New("step must be step1, step2 or step3")
				}
				params, err := json.Marshal(input.Parameters)
				if err != nil {
					return nil, out, err
				}
				req := jobs.OperationRequest{Target: input.Target, Operation: input.Step + ".set_parameters", Scope: json.RawMessage("{}"), Parameters: params}
				err = call(ctx, "states.execute", req, &out)
				return nil, out, err
			})
		capabilities = append(capabilities, addStep1Tools(server, call)...)
		capabilities = append(capabilities, addStep2Tools(server, call)...)
		capabilities = append(capabilities, addProjectTools(server, call)...)
		addCatalog(server, call)
		addProjectListing(server, call)
		addArtifactResources(server, call)
		capabilities = append(capabilities, "describe_operations", "list_project_items", "list_job_artifacts")
	}
	sdk.AddTool(server, &sdk.Tool{
		Name:        "server_info",
		Description: "Return the HDMapping product version and implemented capabilities.",
		Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true},
	}, func(ctx context.Context, _ *sdk.CallToolRequest, _ struct{}) (*sdk.CallToolResult, serverInfo, error) {
		if call != nil {
			if err := call(ctx, "ping", nil, nil); err != nil {
				return nil, serverInfo{}, err
			}
		}
		return nil, serverInfo{Name: "HDMapping", Version: version, Capabilities: capabilities}, nil
	})
	return server
}

type openStates struct {
	States []jobs.TargetRef `json:"states"`
}
type describeState struct {
	Target jobs.TargetRef `json:"target"`
}
type setParameters struct {
	Target     jobs.TargetRef `json:"target"`
	Step       string         `json:"step" jsonschema:"Processing application: step1, step2, or step3."`
	Parameters map[string]any `json:"parameters" jsonschema:"Parameter values supported by this opening."`
}
type stateSummary struct {
	Target     jobs.TargetRef `json:"target"`
	Parameters map[string]any `json:"parameters"`
	Metadata   map[string]any `json:"metadata"`
}
