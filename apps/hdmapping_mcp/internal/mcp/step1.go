package mcp

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

type step1Input struct {
	RequestKey string               `json:"request_key" jsonschema:"Your stable key for retrying this exact submission"`
	Target     *jobs.TargetRef      `json:"target,omitempty" jsonschema:"Live GUI opening and revision. Supply exactly one of target, snapshot, or recording"`
	Snapshot   *jobs.SnapshotRef    `json:"snapshot,omitempty" jsonschema:"A durable snapshot and its target. Use result.snapshot from a previous job when continuing a workflow"`
	Recording  *jobs.Step1Recording `json:"recording,omitempty" jsonschema:"Raw Mandeye recording for headless processing"`
	Scope      map[string]any       `json:"scope,omitempty" jsonschema:"Processing range and operation-specific options"`
	Parameters map[string]any       `json:"parameters,omitempty" jsonschema:"Optional processing parameter overrides using Step 1 TOML names"`
}

type jobInput struct {
	ID jobs.JobID `json:"job_id" jsonschema:"Job ID returned by the submission tool"`
}

type step1Job struct {
	ID             jobs.JobID         `json:"job_id"`
	Operation      string             `json:"operation"`
	Target         jobs.TargetRef     `json:"target"`
	Status         jobs.Status        `json:"status"`
	Progress       jobs.ProgressEvent `json:"progress"`
	Snapshot       *jobs.SnapshotRef  `json:"snapshot,omitempty"`
	Result         *jobs.Publication  `json:"result,omitempty"`
	Error          *toolJobError      `json:"error,omitempty"`
	GUIApplyStatus string             `json:"gui_apply_status"`
	ArtifactsURI   string             `json:"artifacts_uri,omitempty"`
}

type toolJobError struct {
	Code          string `json:"code"`
	Message       string `json:"message"`
	OperationCode string `json:"operation_code,omitempty"`
	// The SDK infers a byte-array schema from json.RawMessage. The worker
	// supplies a JSON value whose encoding must survive the tool boundary.
	Details any `json:"details,omitempty"`
}

func addStep1Tools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	addOperationCatalog(server, call, "step1", "List Step 1 scope and parameter schemas, including the native defaults.")
	operations := []struct {
		name, description string
		access            jobs.Access
	}{
		{"load", "Load a raw Mandeye recording and publish its prepared Step 1 state. With a live target use an absolute scope.recording_directory.", jobs.AccessMutate},
		{"prepare", "Prepare IMU and LiDAR data from a recording for odometry. With a live target use an absolute scope.recording_directory.", jobs.AccessMutate},
		{"odometry", "Compute LiDAR odometry. Set scope.save to false to retain the prepared state. An optional absolute scope.destination writes the session there.", jobs.AccessMutate},
		{"consistency", "Run the consistency optimizer on a state with a computed trajectory.", jobs.AccessMutate},
		{"reference", "Process Step 1 reference clouds. Use scope.mode load, filter, or align. For load, supply LAS or LAZ paths in scope.files.", jobs.AccessMutate},
		{"save", "Save the computed trajectory and point clouds as a native Step 1 session. Optional scope.destination is an absolute directory.", jobs.AccessRead},
		{"export", "Export Step 1 output. Use scope.mode complete, las, laz, trajectory, or poses. Optional scope.begin and scope.end select a half-open scan range, or scope.selected selects the current scan selection for LAS, LAZ, or poses. Optional scope.destination is absolute.", jobs.AccessRead},
	}
	var capabilities []string
	for _, op := range operations {
		name := "step1_" + op.name
		capabilities = append(capabilities, name)
		sdk.AddTool(server, &sdk.Tool{Name: name, Description: op.description + " Returns a job ID for get_job."},
			func(ctx context.Context, _ *sdk.CallToolRequest, input step1Input) (*sdk.CallToolResult, step1Job, error) {
				request, err := input.submit("step1."+op.name, op.access)
				if err != nil {
					return nil, step1Job{}, err
				}
				var record jobs.JobRecord
				err = call(ctx, "jobs.submit", request, &record)
				return nil, summarizeJob(record), err
			})
	}
	sdk.AddTool(server, &sdk.Tool{Name: "get_job", Description: "Read a job's progress, result, error, and GUI apply outcome.", Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true}},
		func(ctx context.Context, _ *sdk.CallToolRequest, input jobInput) (*sdk.CallToolResult, step1Job, error) {
			var record jobs.JobRecord
			if err := call(ctx, "jobs.get", input.ID, &record); err != nil {
				return nil, step1Job{}, err
			}
			job := summarizeJob(record)
			if job.ArtifactsURI == "" {
				return nil, job, nil
			}
			data, err := json.Marshal(job)
			if err != nil {
				return nil, step1Job{}, err
			}
			return &sdk.CallToolResult{Content: []sdk.Content{
				&sdk.TextContent{Text: string(data)},
				&sdk.ResourceLink{URI: job.ArtifactsURI, Name: "Job artifacts", MIMEType: "application/json"},
			}}, job, nil
		})
	sdk.AddTool(server, &sdk.Tool{Name: "cancel_job", Description: "Request cancellation of a job. Use get_job to observe the final outcome."},
		func(ctx context.Context, _ *sdk.CallToolRequest, input jobInput) (*sdk.CallToolResult, step1Job, error) {
			var record jobs.JobRecord
			err := call(ctx, "jobs.cancel", input.ID, &record)
			return nil, summarizeJob(record), err
		})
	return append(capabilities, "get_job", "cancel_job", "describe_step1_operations")
}

func (input step1Input) submit(operation string, access jobs.Access) (jobs.SubmitRequest, error) {
	request := jobs.SubmitRequest{RequestKey: input.RequestKey, Source: jobs.SourceMCP, Access: access,
		Operation: jobs.OperationRequest{Operation: operation, Scope: json.RawMessage("{}"), Parameters: json.RawMessage("{}")}}
	switch {
	case input.Target != nil && input.Snapshot == nil && input.Recording == nil:
		request.Operation.Target = *input.Target
		request.CaptureSource = input.Target.StateID
	case input.Target == nil && input.Snapshot != nil && input.Recording == nil:
		request.Operation.Target = input.Snapshot.Target
		request.Snapshot = input.Snapshot
	case input.Target == nil && input.Snapshot == nil && input.Recording != nil:
		request.Operation.Target.StateID = fmt.Sprintf("step1:%x", sha256.Sum256([]byte(input.RequestKey)))
		request.Recording = input.Recording
	default:
		return request, errors.New("supply exactly one of target, snapshot, or recording")
	}
	var err error
	if input.Scope != nil {
		request.Operation.Scope, err = json.Marshal(input.Scope)
		if err != nil {
			return request, fmt.Errorf("encode scope: %w", err)
		}
	}
	if input.Parameters != nil {
		request.Operation.Parameters, err = json.Marshal(input.Parameters)
		if err != nil {
			return request, fmt.Errorf("encode parameters: %w", err)
		}
	}
	return request, nil
}

func summarizeJob(record jobs.JobRecord) step1Job {
	job := step1Job{
		ID: record.ID, Operation: record.Request.Operation.Operation, Target: record.Request.Operation.Target,
		Status: record.Status, Progress: record.Progress, Snapshot: record.Snapshot, Result: record.Result,
		GUIApplyStatus: record.GUIApplyStatus,
	}
	if record.Status == jobs.Completed && record.Result != nil && record.Result.Confirmed && record.Result.Snapshot != nil {
		job.ArtifactsURI = jobResourceURI(job.ID, "artifacts")
	}
	if failure := record.Error; failure != nil {
		job.Error = &toolJobError{Code: failure.Code, Message: failure.Message, OperationCode: failure.OperationCode}
		if len(failure.Details) != 0 {
			job.Error.Details = failure.Details
		}
	}
	return job
}
