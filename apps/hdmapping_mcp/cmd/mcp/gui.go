package main

import (
	"bufio"
	"context"
	"errors"
	"io"
	"os"
	"syscall"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/gui"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

// The GUI owns the relay's stdin. EOF unregisters the process even when its
// last advertised opening is retained for an in-flight capture.
func relay(ctx context.Context, input io.Reader, output io.Writer) error {
	client, err := coordinatorClient()
	if err != nil {
		return err
	}
	var key string
	defer func() {
		if key != "" {
			_ = client.Call(ctx, "gui.disconnect", key, nil)
		}
	}()

	reader := bufio.NewReader(input)
	for {
		var p gui.Poll
		if err = protocol.ReadFrame(reader, &p); err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}
		if p.Wire != protocol.WireVersion || p.Version != productVersion {
			return errors.New("incompatible GUI bridge protocol or product version")
		}
		response := gui.Envelope{Wire: p.Wire, Version: p.Version}
		if key == "" {
			// Local preview and local parameter edits do not need a coordinator.
			// Register when an MCP client (or a heavy GUI operation) starts one.
			err = client.Call(ctx, "gui.connect", nil, &key)
			if err != nil && !errors.Is(err, os.ErrNotExist) && !errors.Is(err, syscall.ECONNREFUSED) {
				return err
			}
		}
		if key != "" {
			p.Session = key
			if err = client.Call(ctx, "gui.poll", p, &response); err != nil {
				return err
			}
		}
		if err = protocol.WriteFrame(output, response); err != nil {
			return err
		}
	}
}
