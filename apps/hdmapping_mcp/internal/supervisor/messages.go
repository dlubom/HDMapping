// Package supervisor owns worker processes and reconciles their receipts.
package supervisor

import (
	"encoding/json"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

type command struct {
	Type      string                 `json:"type"`
	ID        jobs.JobID             `json:"job_id,omitempty"`
	Operation *jobs.OperationRequest `json:"operation,omitempty"`
	Snapshot  *jobs.SnapshotRef      `json:"snapshot,omitempty"`
}

type event struct {
	protocol.Hello
	Type       string              `json:"type"`
	Progress   *jobs.ProgressEvent `json:"progress,omitempty"`
	Operations json.RawMessage     `json:"operations,omitempty"`
	Error      *jobs.JobError      `json:"error,omitempty"`
}

type receipt struct {
	ID       jobs.JobID `json:"job_id"`
	Wire     int        `json:"wire"`
	Version  string     `json:"version"`
	Manifest struct {
		ID           string           `json:"artifact_id"`
		Target       jobs.TargetRef   `json:"target"`
		Snapshot     jobs.SnapshotRef `json:"snapshot"`
		Files        []string         `json:"files"`
		Dependencies []string         `json:"dependencies"`
	} `json:"manifest"`
}
