// A process-level test fixture. The product worker is a C++ executable.
package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
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
		Directory string                `json:"directory"`
		Recording struct {
			Directory string `json:"directory"`
			TOML      string `json:"toml"`
		} `json:"recording"`
	}
	if err := protocol.ReadFrame(reader, &command); err != nil {
		os.Exit(3)
	}
	if command.Type == "describe" {
		var operations []map[string]string
		for _, id := range []string{
			"constraints.add", "constraints.list", "constraints.remove", "constraints.replace", "constraints.run_icp", "constraints.update",
			"cp.import", "cp.remove", "cp.update", "gcp.import", "gcp.remove", "gcp.update", "gnss.import", "gnss.project", "tum.import",
			"step1.consistency", "step1.export", "step1.load", "step1.odometry", "step1.prepare", "step1.reference", "step1.save",
			"step2.register_scans", "step2.optimize_graph", "step2.align_reference",
			"step2.edit_segment", "step2.restore_poses", "step2.transform_scope",
		} {
			operations = append(operations, map[string]string{"id": id})
		}
		write(map[string]any{"type": "operations", "operations": operations})
		return
	}
	if command.Type == "validate_snapshot" {
		var input map[string]bool
		data, err := os.ReadFile(command.Snapshot.ManifestPath)
		if err != nil || json.Unmarshal(data, &input) != nil || !input["fixture_complete"] {
			write(map[string]any{"type": "error", "error": map[string]string{
				"code": "invalid_snapshot", "message": "test fixture is incomplete",
			}})
			return
		}
		write(map[string]any{"type": "snapshot_validated"})
		return
	}
	if command.Type == "capture_step1_recording" {
		if !filepath.IsAbs(command.Recording.Directory) || command.Recording.TOML != filepath.Join(command.Recording.Directory, "parking.toml") {
			os.Exit(9)
		}
		if err := os.Mkdir(command.Directory, 0700); err != nil {
			os.Exit(10)
		}
		path := filepath.Join(command.Directory, "snapshot.json")
		if err := os.WriteFile(path, []byte(`{"fixture_complete":true}`), 0600); err != nil {
			os.Exit(11)
		}
		write(map[string]any{"type": "snapshot_captured", "snapshot": jobs.SnapshotRef{ManifestPath: path, Target: command.Operation.Target}})
		return
	}
	referenceImport := command.Operation.Operation == "gnss.import" || command.Operation.Operation == "tum.import"
	if referenceImport {
		var parameters struct{ Paths []string }
		if err := json.Unmarshal(command.Operation.Parameters, &parameters); err != nil || len(parameters.Paths) != 1 {
			os.Exit(12)
		}
		data, err := os.ReadFile(parameters.Paths[0])
		if err != nil || string(data) != "recorded reference\n" {
			os.Exit(13)
		}
	}
	if command.Operation.Operation == "durable" || command.Operation.Operation == "step1.prepare" || referenceImport {
		command.Snapshot.ManifestPath = "snapshot.json"
		if err := os.WriteFile("snapshot.json", []byte(`{"fixture_complete":true}`), 0600); err != nil {
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
		if command.Operation.Operation == "step1.prepare" || referenceImport {
			write(map[string]any{"type": "completed"})
			return
		}
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
