// Package mcp exposes the implemented HDMapping operations through MCP.
package mcp

import (
	"context"

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

// NewWithCoordinator verifies the local coordinator when server_info is called.
func NewWithCoordinator(version string, ensure func(context.Context) error) *sdk.Server {
	server := sdk.NewServer(&sdk.Implementation{Name: "HDMapping", Version: version},
		&sdk.ServerOptions{Capabilities: &sdk.ServerCapabilities{Tools: &sdk.ToolCapabilities{}}})
	sdk.AddTool(server, &sdk.Tool{
		Name:        "server_info",
		Description: "Return the HDMapping product version and implemented capabilities.",
		Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true},
	}, func(ctx context.Context, _ *sdk.CallToolRequest, _ struct{}) (*sdk.CallToolResult, serverInfo, error) {
		if ensure != nil {
			if err := ensure(ctx); err != nil {
				return nil, serverInfo{}, err
			}
		}
		return nil, serverInfo{Name: "HDMapping", Version: version, Capabilities: []string{"server_info"}}, nil
	})
	return server
}
