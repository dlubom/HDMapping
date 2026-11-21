// Package jobs owns durable job admission, reservations and bounded execution.
// Operation schemas and domain/revision validation belong to the C++ caller.
package jobs

import (
	"context"
	"encoding/json"
	"errors"
)

type JobID string
type Status string

const (
	Preparing   Status = "preparing"
	Queued      Status = "queued"
	Running     Status = "running"
	Cancelling  Status = "cancelling"
	Completed   Status = "completed"
	Failed      Status = "failed"
	Cancelled   Status = "cancelled"
	Interrupted Status = "interrupted"
)

func (s Status) terminal() bool {
	return s == Completed || s == Failed || s == Cancelled || s == Interrupted
}

type Source string

const (
	SourceGUI Source = "gui"
	SourceMCP Source = "mcp"
)

type Access string

const (
	AccessRead   Access = "read"
	AccessMutate Access = "mutate"
)

var (
	ErrSourceLost       = errors.New("GUI source disconnected")
	ErrRequestConflict  = errors.New("request_key already has different arguments")
	ErrReserved         = errors.New("state is reserved by another job")
	ErrNotFound         = errors.New("job not found")
	ErrClosed           = errors.New("jobs closed")
	ErrStoreUncertain   = errors.New("journal write failed; close and recover before mutating")
	ErrRecoveryRequired = errors.New("unfinished jobs require explicit recovery")
	ErrQueueFull        = errors.New("pending job limit reached")
)

const (
	DefaultLimit = 2
	MaxLimit     = 64
	MaxPending   = 1024
)

type TargetRef struct {
	StateID  string `json:"state_id"`
	Revision uint64 `json:"revision"`
}
type OperationRequest struct {
	Operation  string          `json:"operation"`
	Target     TargetRef       `json:"target"`
	Scope      json.RawMessage `json:"scope"`
	Parameters json.RawMessage `json:"parameters"`
}
type SnapshotRef struct {
	ManifestPath string    `json:"manifest_path"`
	Target       TargetRef `json:"target"`
}
type Step1Recording struct {
	Directory string `json:"directory" jsonschema:"Absolute path to the Mandeye recording directory"`
	TOML      string `json:"toml,omitempty" jsonschema:"Absolute path to an optional Step 1 TOML file"`
}

// DetectLoopCandidates publishes a report that can be presented in a live Step 2 editor.
const DetectLoopCandidates = "constraints.detect_candidates"

// OpenSession admits external Step 2 input before a worker publishes the session.
const OpenSession = "session.open"

// SubmitRequest describes one heavy job. Access controls exclusive reservation,
// independently of its source. Exactly one input is specified: a durable Snapshot,
// a live CaptureSource reserved until Prepare returns, a raw Step 1 Recording,
// or external files named by a session.open operation.
type SubmitRequest struct {
	RequestKey    string           `json:"request_key"`
	Operation     OperationRequest `json:"operation"`
	Source        Source           `json:"source"`
	Access        Access           `json:"access"`
	Snapshot      *SnapshotRef     `json:"snapshot,omitempty"`
	CaptureSource string           `json:"capture_source,omitempty"`
	Recording     *Step1Recording  `json:"recording,omitempty"`
}
type ProgressEvent struct {
	Stage   string  `json:"stage"`
	Current *uint64 `json:"current,omitempty"`
	Total   *uint64 `json:"total,omitempty"`
	Message string  `json:"message,omitempty"`
}
type JobError struct {
	Code          string          `json:"code"`
	Message       string          `json:"message"`
	OperationCode string          `json:"operation_code,omitempty"`
	Details       json.RawMessage `json:"details,omitempty"`
}

func (e *JobError) Error() string { return e.Code + ": " + e.Message }

// Publication is a receipt from the executor AFTER full result publication.
// Its truth must be established by the publisher, not inferred from process exit.
type Publication struct {
	Confirmed       bool         `json:"confirmed"`
	OutputDirectory string       `json:"output_directory"`
	ManifestPath    string       `json:"manifest_path"`
	Snapshot        *SnapshotRef `json:"snapshot,omitempty"`
}
type JobRecord struct {
	ID             JobID         `json:"job_id"`
	Request        SubmitRequest `json:"request"`
	Status         Status        `json:"status"`
	Snapshot       *SnapshotRef  `json:"snapshot,omitempty"`
	Progress       ProgressEvent `json:"progress"`
	Result         *Publication  `json:"result,omitempty"`
	Error          *JobError     `json:"error,omitempty"`
	GUIApplyStatus string        `json:"gui_apply_status"`
}
type JobFilter struct {
	StateID string
	Status  Status
	Limit   int
	After   JobID
}

// Executor must stop when ctx is cancelled and return only after it has stopped
// using job inputs. Scheduler.Close waits for every call. P03 owns process timeout,
// forced termination and publication reconciliation. A confirmed publication wins
// a cancellation race, even if Run also reports context cancellation.
type Executor interface {
	Prepare(ctx context.Context, job JobRecord) (SnapshotRef, error)
	Run(ctx context.Context, job JobRecord) (Publication, error)
}
