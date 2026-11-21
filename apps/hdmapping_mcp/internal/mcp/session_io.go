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

func addSessionIOTools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	operations := []struct{ name, description string }{
		{jobs.OpenSession, "Open existing Step 2 session files. Omit target to create a headless snapshot, or supply a live target for auto-apply."},
		{"session.save", "Publish a complete native session from the selected revision."},
		{"session.subsession", "Publish a native session with the selected scans."},
		{"session.import_poses", "Import initial, current or inverse RESSO poses for selected scans."},
		{"session.import_observations", "Import native JSON observations tied to selected scan indices."},
		{"export_result", "Publish an existing Step 2 export format for an explicit scan scope."},
	}
	ids := make([]string, 0, len(operations))
	for _, op := range operations {
		sdk.AddTool(server, &sdk.Tool{Name: op.name, Description: op.description + " See describe_session_operations for the argument schemas. Returns a job ID for get_job."},
			func(ctx context.Context, _ *sdk.CallToolRequest, input step2MutationInput) (*sdk.CallToolResult, step1Job, error) {
				if input.RequestKey == "" {
					return nil, step1Job{}, errors.New("request_key is required")
				}
				request, err := sessionIORequest(op.name, input)
				if err != nil {
					return nil, step1Job{}, err
				}
				var record jobs.JobRecord
				err = call(ctx, "jobs.submit", request, &record)
				return nil, summarizeJob(record), err
			})
		ids = append(ids, op.name)
	}
	addOperationCatalog(server, call, "session", "List the closed Step 2 session import and export contracts.", ids...)
	return append(ids, "describe_session_operations")
}

func sessionIORequest(operation string, input step2MutationInput) (jobs.SubmitRequest, error) {
	if operation == jobs.OpenSession {
		if input.Snapshot != nil {
			return jobs.SubmitRequest{}, errors.New("session.open requires external files")
		}
		if input.Target != nil {
			return (step1Input{RequestKey: input.RequestKey, Target: input.Target, Scope: input.Scope, Parameters: input.Parameters}).submit(operation, jobs.AccessMutate)
		}
		parameters, err := json.Marshal(input.Parameters)
		if err != nil {
			return jobs.SubmitRequest{}, fmt.Errorf("encode open parameters: %w", err)
		}
		scope, err := json.Marshal(input.Scope)
		if err != nil {
			return jobs.SubmitRequest{}, fmt.Errorf("encode open scope: %w", err)
		}
		if input.Scope == nil {
			scope = json.RawMessage("{}")
		}
		return jobs.SubmitRequest{RequestKey: input.RequestKey, Source: jobs.SourceMCP, Access: jobs.AccessMutate,
			Operation: jobs.OperationRequest{Operation: operation, Scope: scope, Parameters: parameters,
				Target: jobs.TargetRef{StateID: fmt.Sprintf("step2:%x", sha256.Sum256([]byte(input.RequestKey)))}}}, nil
	}
	access := jobs.AccessRead
	if operation == "session.import_poses" || operation == "session.import_observations" {
		access = jobs.AccessMutate
	}
	return (step1Input{RequestKey: input.RequestKey, Target: input.Target, Snapshot: input.Snapshot,
		Scope: input.Scope, Parameters: input.Parameters}).submit(operation, access)
}
