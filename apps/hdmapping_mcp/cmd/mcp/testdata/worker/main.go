// A process-level test fixture. The product worker is a C++ executable.
package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"io"
	"os"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

var productVersion string

func write(frame any) {
	if err := protocol.WriteFrame(os.Stdout, frame); err != nil {
		os.Exit(2)
	}
}

func progress(stage string) {
	write(map[string]any{"type": "progress", "progress": jobs.ProgressEvent{Stage: stage}})
}

func main() {
	pid := os.Getpid()
	if os.Getenv("HDMAPPING_TEST_WRONG_WORKER_PID") == "1" {
		pid++
	}
	write(map[string]any{"type": "hello", "wire": protocol.WireVersion, "version": productVersion, "pid": pid})
	reader := bufio.NewReader(os.Stdin)
	var command struct {
		Type      string                `json:"type"`
		ID        jobs.JobID            `json:"job_id"`
		Operation jobs.OperationRequest `json:"operation"`
		Snapshot  jobs.SnapshotRef      `json:"snapshot"`
	}
	if err := protocol.ReadFrame(reader, &command); err != nil {
		os.Exit(3)
	}
	if command.Type == "describe" {
		write(map[string]any{"type": "operations", "operations": []any{}})
		return
	}
	if command.Operation.Operation == "durable" {
		command.Snapshot.ManifestPath = "snapshot.json"
		if err := os.WriteFile("snapshot.json", []byte("{}"), 0600); err != nil {
			os.Exit(8)
		}
		file, err := os.OpenFile("result.json", os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
		if err != nil {
			os.Exit(4)
		}
		if err := json.NewEncoder(file).Encode(map[string]any{
			"job_id": command.ID, "wire": protocol.WireVersion, "version": productVersion,
			"manifest": map[string]any{"artifact_id": "fake-result", "target": command.Operation.Target, "snapshot": command.Snapshot,
				"files": []string{"snapshot.json"}, "dependencies": []string{}},
		}); err != nil {
			os.Exit(5)
		}
		if err := errors.Join(jobs.SyncRuntimeReceipt(file), file.Close()); err != nil {
			os.Exit(6)
		}
		progress("durable")
	} else {
		progress("ready")
	}
	var cancel struct{}
	if err := protocol.ReadFrame(reader, &cancel); err != nil && !errors.Is(err, io.EOF) {
		os.Exit(7)
	}
	switch command.Operation.Operation {
	case "force", "force_crash":
		progress("ignoring_cancel")
		for {
			time.Sleep(time.Hour)
		}
	case "durable":
		write(map[string]any{"type": "completed"})
	default:
		progress("graceful_exit")
		write(map[string]any{"type": "error", "error": jobs.JobError{Code: "cancelled", Message: "test worker cancelled"}})
	}
}
