package main

import (
	"context"
	"fmt"
	"log"
	"os"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/mcp"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
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
	if err := mcp.NewWithCoordinator(productVersion, ensureCoordinator).Run(context.Background(), &sdk.StdioTransport{}); err != nil {
		logger.Printf("serve stdio: %v", err)
		os.Exit(1)
	}
}

func ensureCoordinator(ctx context.Context) error {
	executable, err := os.Executable()
	if err != nil {
		return fmt.Errorf("resolve adapter: %w", err)
	}
	coordinator, err := protocol.Companion(executable, "hdmapping_mcp", "hdmapping_coordinator")
	if err != nil {
		return err
	}
	dir, err := protocol.RuntimeDirectory()
	if err != nil {
		return err
	}
	client, err := protocol.NewClient(dir, productVersion, coordinator)
	if err != nil {
		return err
	}
	return client.Ensure(ctx)
}
