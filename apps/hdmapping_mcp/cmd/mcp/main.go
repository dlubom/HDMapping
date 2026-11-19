package main

import (
	"context"
	"fmt"
	"io"
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
	var serveGUI func(context.Context, io.Reader, io.Writer) error
	if len(os.Args) == 2 {
		switch os.Args[1] {
		case "--gui-bridge":
			serveGUI = relay
		case "--gui-rpc":
			serveGUI = relayJobs
		}
	}
	if serveGUI != nil {
		if err := serveGUI(context.Background(), os.Stdin, os.Stdout); err != nil {
			logger.Print(err)
			os.Exit(1)
		}
		return
	}
	logger.Printf("HDMapping %s MCP stdio", productVersion)
	if err := mcp.NewWithCoordinator(productVersion, callCoordinator).Run(context.Background(), &sdk.StdioTransport{}); err != nil {
		logger.Printf("serve stdio: %v", err)
		os.Exit(1)
	}
}

func callCoordinator(ctx context.Context, method string, input, output any) error {
	client, err := coordinatorClient()
	if err != nil {
		return err
	}
	if err = client.Ensure(ctx); err != nil {
		return err
	}
	return client.Call(ctx, method, input, output)
}
func coordinatorClient() (*protocol.Client, error) {
	executable, err := os.Executable()
	if err != nil {
		return nil, fmt.Errorf("resolve adapter: %w", err)
	}
	coordinator, err := protocol.Companion(executable, "hdmapping_mcp", "hdmapping_coordinator")
	if err != nil {
		return nil, err
	}
	dir, err := protocol.RuntimeDirectory()
	if err != nil {
		return nil, err
	}
	return protocol.NewClient(dir, productVersion, coordinator)
}
