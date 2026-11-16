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
	server := sdk.NewServer(&sdk.Implementation{Name: "HDMapping", Version: version},
		&sdk.ServerOptions{Capabilities: &sdk.ServerCapabilities{Tools: &sdk.ToolCapabilities{}}})
	sdk.AddTool(server, &sdk.Tool{
		Name:        "server_info",
		Description: "Return the HDMapping product version and implemented capabilities.",
		Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true},
	}, func(context.Context, *sdk.CallToolRequest, struct{}) (*sdk.CallToolResult, serverInfo, error) {
		return nil, serverInfo{Name: "HDMapping", Version: version, Capabilities: []string{"server_info"}}, nil
	})
	return server
}
