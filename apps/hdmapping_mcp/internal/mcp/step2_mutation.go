package mcp

import (
	"context"
	"errors"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

type step2MutationInput struct {
	RequestKey string            `json:"request_key,omitempty" jsonschema:"Stable retry key. Required for durable jobs."`
	Target     *jobs.TargetRef   `json:"target,omitempty" jsonschema:"Live Step 2 opening and exact revision. Supply either target or snapshot."`
	Snapshot   *jobs.SnapshotRef `json:"snapshot,omitempty" jsonschema:"Durable Step 2 session snapshot and its exact target and revision."`
	Scope      map[string]any    `json:"scope,omitempty" jsonschema:"Operation-specific scope from the Step 2 operation catalog."`
	Parameters map[string]any    `json:"parameters,omitempty" jsonschema:"Operation-specific parameters from the Step 2 operation catalog."`
}

type step2MutationResponse struct {
	State *stateSummary `json:"state,omitempty"`
	Job   *step1Job     `json:"job,omitempty"`
}

type step2MutationMode int

const (
	liveOrSnapshot step2MutationMode = iota
	durableJob
	liveOrDurable
)

func addStep2MutationTool(server *sdk.Server, call func(context.Context, string, any, any) error, id, description string, mode step2MutationMode) {
	switch mode {
	case durableJob:
		description += " Returns a job ID for get_job."
	case liveOrDurable:
		description += " With a live target, returns its updated state. Supply request_key to queue a durable job with auto-apply. A snapshot always requires request_key and returns a job ID."
	case liveOrSnapshot:
		description += " Returns the updated live state or a job ID for a snapshot."
	}
	sdk.AddTool(server, &sdk.Tool{Name: id, Description: description},
		func(ctx context.Context, _ *sdk.CallToolRequest, input step2MutationInput) (*sdk.CallToolResult, step2MutationResponse, error) {
			var out step2MutationResponse
			if (input.Target == nil) == (input.Snapshot == nil) {
				return nil, out, errors.New("supply exactly one of target or snapshot")
			}
			request, err := (step1Input{RequestKey: input.RequestKey, Target: input.Target, Snapshot: input.Snapshot,
				Scope: input.Scope, Parameters: input.Parameters}).submit(id, jobs.AccessMutate)
			if err != nil {
				return nil, out, err
			}
			if input.Target != nil && (mode == liveOrSnapshot || (mode == liveOrDurable && input.RequestKey == "")) {
				var state stateSummary
				if err := call(ctx, "states.execute", request.Operation, &state); err != nil {
					return nil, out, err
				}
				out.State = &state
				return nil, out, nil
			}
			var record jobs.JobRecord
			if err := call(ctx, "jobs.submit", request, &record); err != nil {
				return nil, out, err
			}
			job := summarizeJob(record)
			out.Job = &job
			return nil, out, nil
		})
}
