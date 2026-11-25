package mcp

import (
	"context"
	"errors"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func addProjectTools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	durable := []struct{ id, description string }{
		{jobs.OpenProject, "Open an existing native multi-session project. Omit target for headless use."},
		{jobs.AddProjectSession, "Add a native session or LAS/LAZ file to a project."},
		{"project.save", "Publish a complete native project from the selected revision."},
	}
	ids := make([]string, 0, len(durable)+3)
	for _, op := range durable {
		sdk.AddTool(server, &sdk.Tool{Name: op.id, Description: op.description + " See describe_project_operations for argument schemas. Returns a job ID."},
			func(ctx context.Context, _ *sdk.CallToolRequest, input step2MutationInput) (*sdk.CallToolResult, step1Job, error) {
				if input.RequestKey == "" {
					return nil, step1Job{}, errors.New("request_key is required")
				}
				var request jobs.SubmitRequest
				var err error
				if op.id == jobs.OpenProject {
					request, err = externalOpenRequest(op.id, "step3", input)
				} else {
					access := jobs.AccessRead
					if op.id == jobs.AddProjectSession {
						access = jobs.AccessMutate
					}
					request, err = (step1Input{RequestKey: input.RequestKey, Target: input.Target, Snapshot: input.Snapshot,
						Scope: input.Scope, Parameters: input.Parameters}).submit(op.id, access)
				}
				if err != nil {
					return nil, step1Job{}, err
				}
				var record jobs.JobRecord
				err = call(ctx, "jobs.submit", request, &record)
				return nil, summarizeJob(record), err
			})
		ids = append(ids, op.id)
	}
	addStep2MutationTool(server, call, "project.remove_session", "Remove a session and its incident constraints.", liveOrDurable)
	addStep2MutationTool(server, call, "project.set_ground_truth", "Set a ground truth session or clear it with session_index -1.", liveOrDurable)
	ids = append(ids, "project.remove_session", "project.set_ground_truth")
	addOperationCatalog(server, call, "project", "List the native multi-session project contracts.", ids...)
	return append(ids, "describe_project_operations")
}
