package main

import (
	"context"
	"log"
	"os"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/mcp"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

// Set only by the common CMake product build; no independent MCP version.
var productVersion string

func main() {
	logger := log.New(os.Stderr, "hdmapping_mcp: ", 0)
	if productVersion == "" {
		logger.Print("product version missing; build through CMake")
		os.Exit(1)
	}
	logger.Printf("HDMapping %s MCP stdio", productVersion)
	if err := mcp.New(productVersion).Run(context.Background(), &sdk.StdioTransport{}); err != nil {
		logger.Printf("serve stdio: %v", err)
		os.Exit(1)
	}
}
