package mcp

import (
	"context"
	"errors"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

type constraintInput struct {
	RequestKey string            `json:"request_key,omitempty" jsonschema:"Stable retry key. Required for snapshot operations and for ICP or optimization."`
	Target     *jobs.TargetRef   `json:"target,omitempty" jsonschema:"Live Step 2 opening and exact revision. Supply either target or snapshot."`
	Snapshot   *jobs.SnapshotRef `json:"snapshot,omitempty" jsonschema:"Durable Step 2 session snapshot and its exact target and revision."`
	Scope      map[string]any    `json:"scope,omitempty" jsonschema:"Explicit edge_index or edge_indices where required. See the C++ operation catalog."`
	Parameters map[string]any    `json:"parameters,omitempty" jsonschema:"See describe_constraints_operations or describe_step2_operations."`
}

type constraintResponse struct {
	Target      *jobs.TargetRef   `json:"target,omitempty"`
	ActiveEdges *[]map[string]any `json:"active_edges,omitempty"`
	Job         *step1Job         `json:"job,omitempty"`
}

type constraintState struct {
	Target   jobs.TargetRef `json:"target"`
	Metadata struct {
		ActiveEdges []map[string]any `json:"active_edges"`
	} `json:"metadata"`
}

func addConstraintTools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	addOperationCatalog(server, call, "constraints", "List active edge contracts, explicit scopes, units and ICP settings.")
	capabilities := []string{"describe_constraints_operations"}
	for _, op := range []struct {
		id, description string
		read, heavy     bool
	}{
		{"constraints.list", "List the active edges in the requested Step 2 revision.", true, false},
		{"constraints.add", "Activate a manual edge. Existing edges are retained.", false, false},
		{"constraints.update", "Update fields on one active edge at scope.edge_index.", false, false},
		{"constraints.remove", "Remove one active edge at scope.edge_index.", false, false},
		{"constraints.replace", "Replace exactly scope.edge_indices with parameters.edges. Requires the current revision.", false, false},
		{"constraints.run_icp", "Refine the active edges named by scope.edge_indices. Check get_job for completion.", false, true},
		{"step2.optimize_graph", "Optimize one session using its motion model and the active edges named by scope.edge_indices. Check get_job for completion.", false, true},
	} {
		capabilities = append(capabilities, op.id)
		sdk.AddTool(server, &sdk.Tool{Name: op.id, Description: op.description, Annotations: &sdk.ToolAnnotations{ReadOnlyHint: op.read}},
			func(ctx context.Context, _ *sdk.CallToolRequest, input constraintInput) (*sdk.CallToolResult, constraintResponse, error) {
				var out constraintResponse
				if (input.Target == nil) == (input.Snapshot == nil) {
					return nil, out, errors.New("supply exactly one of target or snapshot")
				}
				if input.Target != nil && op.read {
					if len(input.Scope) != 0 || len(input.Parameters) != 0 {
						return nil, out, errors.New("constraints.list takes empty scope and parameters")
					}
					var state constraintState
					if err := call(ctx, "states.describe", describeState{Target: *input.Target}, &state); err != nil {
						return nil, out, err
					}
					if state.Metadata.ActiveEdges == nil {
						return nil, out, errors.New("target is not a Step 2 session")
					}
					out.Target, out.ActiveEdges = &state.Target, &state.Metadata.ActiveEdges
					return nil, out, nil
				}
				request, err := (step1Input{Target: input.Target, Snapshot: input.Snapshot,
					RequestKey: input.RequestKey, Scope: input.Scope, Parameters: input.Parameters}).submit(op.id, jobs.AccessMutate)
				if err != nil {
					return nil, out, err
				}
				if input.Target != nil && !op.heavy {
					var state constraintState
					if err := call(ctx, "states.execute", request.Operation, &state); err != nil {
						return nil, out, err
					}
					out.Target, out.ActiveEdges = &state.Target, &state.Metadata.ActiveEdges
					return nil, out, nil
				}

				if op.read {
					request.Access = jobs.AccessRead
				}
				var record jobs.JobRecord
				err = call(ctx, "jobs.submit", request, &record)
				if err == nil {
					job := summarizeJob(record)
					out.Job = &job
				}
				return nil, out, err
			})
	}
	return capabilities
}
