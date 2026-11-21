package mcp

import (
	"context"
	"errors"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

type georeferenceInput struct {
	RequestKey string            `json:"request_key,omitempty" jsonschema:"Stable retry key. Required for durable jobs."`
	Target     *jobs.TargetRef   `json:"target,omitempty" jsonschema:"Live Step 2 opening and exact revision. Supply either target or snapshot."`
	Snapshot   *jobs.SnapshotRef `json:"snapshot,omitempty" jsonschema:"Durable Step 2 session snapshot and its exact target and revision."`
	Scope      map[string]any    `json:"scope,omitempty" jsonschema:"Operation-specific scope from describe_georeference_operations."`
	Parameters map[string]any    `json:"parameters,omitempty" jsonschema:"Operation-specific parameters from describe_georeference_operations."`
}

type georeferenceResponse struct {
	State *stateSummary `json:"state,omitempty"`
	Job   *step1Job     `json:"job,omitempty"`
}

func addGeoreferenceTools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	operations := []struct {
		id, description string
		heavy           bool
	}{
		{"cp.import", "Import surveyed control points tied to local scan coordinates.", false},
		{"cp.update", "Edit a control point from the specified session revision.", false},
		{"cp.remove", "Remove a control point from the specified session revision.", false},
		{"gcp.import", "Import surveyed ground control points tied to LiDAR trajectory nodes.", false},
		{"gcp.update", "Edit a ground control point from the specified session revision.", false},
		{"gcp.remove", "Remove a ground control point from the specified session revision.", false},
		{"gnss.import", "Import a GNSS reference track using a supported input format and projection.", true},
		{"gnss.project", "Project the GNSS reference track in a supported coordinate system and vertical datum.", true},
		{"tum.import", "Import a TUM reference trajectory, optionally relative to its first pose.", true},
		{"step2.align_reference", "Fit a rigid transform to the session poses from CP, GCP, GNSS or TUM observations.", true},
	}
	ids := make([]string, 0, len(operations))
	for _, op := range operations {
		ids = append(ids, op.id)
		description := op.description
		if op.heavy {
			description += " Returns a job ID for get_job."
		} else {
			description += " Returns the updated live state or a job ID for a snapshot."
		}
		sdk.AddTool(server, &sdk.Tool{Name: op.id, Description: description},
			func(ctx context.Context, _ *sdk.CallToolRequest, input georeferenceInput) (*sdk.CallToolResult, georeferenceResponse, error) {
				var out georeferenceResponse
				if (input.Target == nil) == (input.Snapshot == nil) {
					return nil, out, errors.New("supply exactly one of target or snapshot")
				}
				request, err := (step1Input{RequestKey: input.RequestKey, Target: input.Target, Snapshot: input.Snapshot,
					Scope: input.Scope, Parameters: input.Parameters}).submit(op.id, jobs.AccessMutate)
				if err != nil {
					return nil, out, err
				}
				if input.Target != nil && !op.heavy {
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
	addOperationCatalog(server, call, "georeference", "List the separate CP and GCP schemas, reference tracks, alignment options and units.", ids...)
	return append(ids, "describe_georeference_operations")
}
