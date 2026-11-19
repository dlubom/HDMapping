package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"syscall"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/gui"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
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

// Job submissions may wait for a capture reservation. An independent pipe
// leaves the normal GUI relay free to poll and reply to the coordinator.
func relayJobs(ctx context.Context, input io.Reader, output io.Writer) error {
	reader := bufio.NewReader(input)
	for {
		var request protocol.Request
		if err := protocol.ReadFrame(reader, &request); err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}
		response := struct {
			Wire    int             `json:"wire"`
			Version string          `json:"version"`
			Body    json.RawMessage `json:"body,omitempty"`
			Error   string          `json:"error,omitempty"`
		}{Wire: protocol.WireVersion, Version: productVersion}
		body, err := guiJobCall(ctx, request)
		if err != nil {
			response.Error = err.Error()
		} else {
			response.Body = body
		}
		if err := protocol.WriteFrame(output, response); err != nil {
			return err
		}
	}
}

func guiJobCall(ctx context.Context, request protocol.Request) (json.RawMessage, error) {
	if request.Wire != protocol.WireVersion || request.Version != productVersion {
		return nil, errors.New("incompatible GUI bridge protocol or product version")
	}
	var input any = request.Body
	switch request.Method {
	case "jobs.submit":
		var submit jobs.SubmitRequest
		if err := json.Unmarshal(request.Body, &submit); err != nil {
			return nil, err
		}
		submit.Source = jobs.SourceGUI
		if submit.CaptureSource != "" {
			if err := waitForGUIOpening(ctx, submit.Operation.Target); err != nil {
				return nil, err
			}
		}
		input = submit
	case "jobs.get", "jobs.cancel":
	default:
		return nil, errors.New("unsupported GUI job method")
	}
	var output json.RawMessage
	err := callCoordinator(ctx, request.Method, input, &output)
	return output, err
}

// A cold start or a newly selected recording can submit before the polling
// bridge has advertised its new opening on the independent pipe.
func waitForGUIOpening(ctx context.Context, target jobs.TargetRef) error {
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		var open struct{ States []jobs.TargetRef }
		if err := callCoordinator(ctx, "states.list", nil, &open); err != nil {
			return err
		}
		for _, advertised := range open.States {
			if advertised.StateID == target.StateID {
				// The reservation still performs the exact revision check on the GUI thread.
				return nil
			}
		}
		select {
		case <-ctx.Done():
			return errors.Join(jobs.ErrSourceLost, ctx.Err())
		case <-ticker.C:
		}
	}
}
