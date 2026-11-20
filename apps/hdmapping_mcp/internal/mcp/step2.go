package mcp

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

type registrationInput struct {
	RequestKey string            `json:"request_key" jsonschema:"Stable key for retrying this exact registration"`
	Target     *jobs.TargetRef   `json:"target,omitempty" jsonschema:"Live Step 2 opening and current revision. Supply either target or snapshot"`
	Snapshot   *jobs.SnapshotRef `json:"snapshot,omitempty" jsonschema:"Captured session or published native session including its target"`
	Scope      map[string]any    `json:"scope" jsonschema:"Ordered zero-based scan_indices from the target. See describe_step2_operations"`
	Parameters map[string]any    `json:"parameters" jsonschema:"Registration algorithm and optional settings. See describe_step2_operations"`
}

func addStep2Tools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	sdk.AddTool(server, &sdk.Tool{Name: "register_scans", Description: "Run Step 2 scan registration over explicit scan indices. Returns a job ID for get_job."},
		func(ctx context.Context, _ *sdk.CallToolRequest, input registrationInput) (*sdk.CallToolResult, step1Job, error) {
			if (input.Target == nil) == (input.Snapshot == nil) {
				return nil, step1Job{}, errors.New("supply exactly one of target or snapshot")
			}
			scope, err := json.Marshal(input.Scope)
			if err != nil {
				return nil, step1Job{}, err
			}
			parameters, err := json.Marshal(input.Parameters)
			if err != nil {
				return nil, step1Job{}, err
			}
			request := jobs.SubmitRequest{RequestKey: input.RequestKey, Source: jobs.SourceMCP, Access: jobs.AccessMutate,
				Snapshot: input.Snapshot, Operation: jobs.OperationRequest{Operation: "step2.register_scans", Scope: scope, Parameters: parameters}}
			if input.Target != nil {
				request.Operation.Target = *input.Target
				request.CaptureSource = input.Target.StateID
			} else {
				request.Operation.Target = input.Snapshot.Target
			}
			algorithm, _ := input.Parameters["algorithm"].(string)
			switch algorithm {
			case "ndt_mahalanobis", "icp_rms":
				request.Access = jobs.AccessRead
			}
			var record jobs.JobRecord
			err = call(ctx, "jobs.submit", request, &record)
			return nil, summarizeJob(record), err
		})
	addOperationCatalog(server, call, "step2", "List Step 2 registration and graph optimization scope, settings, defaults and units.")
	return append([]string{"register_scans", "describe_step2_operations"}, addConstraintTools(server, call)...)
}
