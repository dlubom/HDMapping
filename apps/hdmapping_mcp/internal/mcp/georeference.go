package mcp

import (
	"context"

	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

func addGeoreferenceTools(server *sdk.Server, call func(context.Context, string, any, any) error) []string {
	operations := []struct {
		id, description string
		mode            step2MutationMode
	}{
		{"cp.import", "Import surveyed control points tied to local scan coordinates.", liveOrSnapshot},
		{"cp.update", "Edit a control point from the specified session revision.", liveOrSnapshot},
		{"cp.remove", "Remove a control point from the specified session revision.", liveOrSnapshot},
		{"gcp.import", "Import surveyed ground control points tied to LiDAR trajectory nodes.", liveOrSnapshot},
		{"gcp.update", "Edit a ground control point from the specified session revision.", liveOrSnapshot},
		{"gcp.remove", "Remove a ground control point from the specified session revision.", liveOrSnapshot},
		{"gnss.import", "Import a GNSS reference track using a supported input format and projection.", durableJob},
		{"gnss.project", "Project the GNSS reference track in a supported coordinate system and vertical datum.", durableJob},
		{"tum.import", "Import a TUM reference trajectory, optionally relative to its first pose.", durableJob},
		{"step2.align_reference", "Fit a rigid transform to the session poses from CP, GCP, GNSS or TUM observations.", durableJob},
	}
	ids := make([]string, 0, len(operations))
	for _, op := range operations {
		ids = append(ids, op.id)
		addStep2MutationTool(server, call, op.id, op.description, op.mode)
	}
	addOperationCatalog(server, call, "georeference", "List the separate CP and GCP schemas, reference tracks, alignment options and units.", ids...)
	return append(ids, "describe_georeference_operations")
}
