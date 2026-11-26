package mcp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"slices"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func addProjectTools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	durable := []struct {
		id, description string
		access          jobs.Access
	}{
		{jobs.OpenProject, "Open an existing native multi-session project. Omit target for headless use.", jobs.AccessMutate},
		{jobs.AddProjectSession, "Add a native session or LAS/LAZ file to a project.", jobs.AccessMutate},
		{"project.save", "Publish a complete native project from the selected revision.", jobs.AccessRead},
		{"project.register_sessions", "Register explicit session pairs with numeric solver parameters.", jobs.AccessMutate},
		{"optimize_project", "Optimize the multi-session graph with explicitly selected project edges.", jobs.AccessMutate},
		{"project.constraints.run_icp", "Refine explicitly selected project edges with ICP.", jobs.AccessMutate},
		{"project.constraints.detect_candidates", "Detect nearby scans between explicitly selected sessions. Read constraints.json in the result snapshot to review candidates before activation.", jobs.AccessRead},
	}
	short := []struct{ id, description string }{
		{"project.remove_session", "Remove a session and its incident constraints."},
		{"project.set_ground_truth", "Set a ground truth session or clear it with session_index -1."},
		{"project.transform_scope", "Transform the poses in explicitly selected project sessions."},
		{"project.restore_poses", "Restore poses in explicitly selected project sessions."},
		{"project.constraints.add", "Add an active constraint with explicit session and scan endpoints."},
		{"project.constraints.update", "Update one active project constraint by index."},
		{"project.constraints.remove", "Remove one active project constraint by index."},
		{"project.constraints.replace", "Replace only the specified active project constraints."},
	}
	ids := make([]string, 0, len(durable)+len(short)+2)
	const schemaHelp = " See describe_project_operations for argument schemas."
	for _, op := range durable {
		sdk.AddTool(server, &sdk.Tool{Name: op.id, Description: op.description + schemaHelp + " Returns a job ID.",
			Annotations: &sdk.ToolAnnotations{ReadOnlyHint: op.access == jobs.AccessRead}},
			func(ctx context.Context, _ *sdk.CallToolRequest, input step2MutationInput) (*sdk.CallToolResult, step1Job, error) {
				if input.RequestKey == "" {
					return nil, step1Job{}, errors.New("request_key is required")
				}
				var request jobs.SubmitRequest
				var err error
				if op.id == jobs.OpenProject {
					request, err = externalOpenRequest(op.id, "step3", input)
				} else {
					request, err = (step1Input{RequestKey: input.RequestKey, Target: input.Target, Snapshot: input.Snapshot,
						Scope: input.Scope, Parameters: input.Parameters}).submit(op.id, op.access)
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
	for _, op := range short {
		addStep2MutationTool(server, call, op.id, op.description+schemaHelp, liveOrDurable)
		ids = append(ids, op.id)
	}
	const list = "project.constraints.list"
	addProjectConstraintList(server, call, list)
	ids = append(ids, list)
	addOperationCatalog(server, call, "project", "List the native multi-session project contracts.", slices.Concat(ids, []string{"project.export", "project.list_items"})...)
	return append(ids, "describe_project_operations")
}

func addProjectConstraintList(server *sdk.Server, call func(context.Context, string, any, any) error, id string) {
	sdk.AddTool(server, &sdk.Tool{Name: id,
		Description: "List active constraints at a live project revision or publish constraints.json for a project snapshot.",
		Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true}},
		func(ctx context.Context, _ *sdk.CallToolRequest, input step2MutationInput) (*sdk.CallToolResult, constraintResponse, error) {
			var out constraintResponse
			if (input.Target == nil) == (input.Snapshot == nil) {
				return nil, out, errors.New("supply exactly one of target or snapshot")
			}
			if len(input.Scope) != 0 || len(input.Parameters) != 0 {
				return nil, out, errors.New("project.constraints.list takes empty scope and parameters")
			}
			if input.Target != nil {
				var state constraintState
				if err := call(ctx, "states.describe", describeState{Target: *input.Target}, &state); err != nil {
					return nil, out, err
				}
				if state.Metadata.Step != 3 || state.Metadata.ActiveEdges == nil {
					return nil, out, errors.New("target is not a Step 3 project")
				}
				out.Target, out.ActiveEdges = &state.Target, &state.Metadata.ActiveEdges
				return nil, out, nil
			}
			if input.RequestKey == "" {
				return nil, out, errors.New("request_key is required")
			}
			request, err := (step1Input{RequestKey: input.RequestKey, Snapshot: input.Snapshot}).submit(id, jobs.AccessRead)
			if err != nil {
				return nil, out, err
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

type projectPageScope struct {
	Kind         string `json:"kind" jsonschema:"sessions, scans, or constraints"`
	SessionIndex *int   `json:"session_index,omitempty" jsonschema:"Required for scans"`
	Offset       int    `json:"offset,omitempty" jsonschema:"Zero-based offset from next_offset"`
	Limit        int    `json:"limit,omitempty" jsonschema:"Page size 1 through 100, default 50"`
}

type projectPageInput struct {
	Target     *jobs.TargetRef   `json:"target,omitempty"`
	Snapshot   *jobs.SnapshotRef `json:"snapshot,omitempty"`
	RequestKey string            `json:"request_key,omitempty" jsonschema:"Stable request key, required with a snapshot"`
	Scope      projectPageScope  `json:"scope"`
}

type projectPage struct {
	Target     *jobs.TargetRef   `json:"target,omitempty"`
	Items      *[]map[string]any `json:"items,omitempty"`
	Total      *int              `json:"total,omitempty"`
	NextOffset *int              `json:"next_offset,omitempty"`
	Job        *step1Job         `json:"job,omitempty"`
}

func addProjectListing(server *sdk.Server, call func(context.Context, string, any, any) error) {
	sdk.AddTool(server, &sdk.Tool{Name: "list_project_items",
		Description: "Page through sessions, scans, or active constraints at a project revision. A live target returns a page. A snapshot requires request_key and publishes the page as project-items.json.",
		Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true}},
		func(ctx context.Context, _ *sdk.CallToolRequest, input projectPageInput) (*sdk.CallToolResult, projectPage, error) {
			var out projectPage
			if (input.Target == nil) == (input.Snapshot == nil) {
				return nil, out, errors.New("supply exactly one of target or snapshot")
			}
			if input.Scope.Kind != "sessions" && input.Scope.Kind != "scans" && input.Scope.Kind != "constraints" {
				return nil, out, errors.New("kind must be sessions, scans, or constraints")
			}
			if (input.Scope.Kind == "scans") != (input.Scope.SessionIndex != nil) {
				return nil, out, errors.New("supply session_index for a scans page only")
			}
			limit, err := pageLimit(input.Scope.Offset, input.Scope.Limit)
			if err != nil {
				return nil, out, err
			}
			input.Scope.Limit = limit
			if input.Target != nil {
				err = call(ctx, "states.list_project_items", input, &out)
				return nil, out, err
			}
			if input.RequestKey == "" {
				return nil, out, errors.New("request_key is required with a snapshot")
			}
			scope, err := json.Marshal(input.Scope)
			if err != nil {
				return nil, out, fmt.Errorf("encode project page scope: %w", err)
			}
			request := jobs.SubmitRequest{RequestKey: input.RequestKey, Source: jobs.SourceMCP, Access: jobs.AccessRead,
				Snapshot: input.Snapshot, Operation: jobs.OperationRequest{Operation: "project.list_items", Target: input.Snapshot.Target,
					Scope: scope, Parameters: json.RawMessage("{}")}}
			var record jobs.JobRecord
			if err := call(ctx, "jobs.submit", request, &record); err != nil {
				return nil, out, err
			}
			job := summarizeJob(record)
			out.Job = &job
			return nil, out, nil
		})
}
