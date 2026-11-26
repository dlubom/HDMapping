package mcp

import (
	"context"
	"encoding/json"
	"fmt"
	"maps"
	"slices"
	"strings"

	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

// Only names that differ from the public tool need an adapter mapping.
func operationTool(id string) string {
	switch id {
	case "step2.register_scans":
		return "register_scans"
	case "project.export":
		return "export_result"
	case "project.list_items":
		return "list_project_items"
	}
	if suffix, ok := strings.CutPrefix(id, "step1."); ok {
		return "step1_" + suffix
	}
	return id
}

func addCatalog(server *sdk.Server, call func(context.Context, string, any, any) error) {
	addOperationCatalog(server, call, "", "List all C++ processing contracts, their schemas, and the public MCP tools that use them.")
	server.AddReceivingMiddleware(func(next sdk.MethodHandler) sdk.MethodHandler {
		return func(ctx context.Context, method string, req sdk.Request) (sdk.Result, error) {
			result, err := next(ctx, method, req)
			list, ok := result.(*sdk.ListToolsResult)
			if err != nil || !ok || method != "tools/list" {
				return result, err
			}
			var descriptors []map[string]any
			if err := call(ctx, "operations.describe", nil, &descriptors); err != nil {
				return nil, err
			}
			if err := applyCatalogSchemas(list, descriptors); err != nil {
				return nil, err
			}
			return list, nil
		}
	})
}

func applyCatalogSchemas(list *sdk.ListToolsResult, descriptors []map[string]any) error {
	byTool := make(map[string][]map[string]any)
	for _, descriptor := range descriptors {
		id, _ := descriptor["id"].(string)
		if id != "" && descriptor["scope_schema"] != nil && descriptor["parameters_schema"] != nil {
			tool := operationTool(id)
			byTool[tool] = append(byTool[tool], descriptor)
		}
	}
	for i, tool := range list.Tools {
		operations := byTool[tool.Name]
		if len(operations) == 0 {
			continue
		}
		encoded, err := json.Marshal(tool.InputSchema)
		if err != nil {
			return fmt.Errorf("encode %s input schema: %w", tool.Name, err)
		}
		var schema map[string]any
		if err := json.Unmarshal(encoded, &schema); err != nil {
			return fmt.Errorf("decode %s input schema: %w", tool.Name, err)
		}
		properties, ok := schema["properties"].(map[string]any)
		if !ok {
			return fmt.Errorf("missing %s input properties", tool.Name)
		}
		if len(operations) == 1 {
			properties["scope"] = operations[0]["scope_schema"]
			if _, declared := properties["parameters"]; declared {
				properties["parameters"] = operations[0]["parameters_schema"]
			}
		} else {
			// Keep the SDK envelope, and attach paired alternatives together.
			var alternatives []map[string]any
			for _, operation := range operations {
				alternative := map[string]any{
					"properties": map[string]any{
						"scope": operation["scope_schema"], "parameters": operation["parameters_schema"],
					},
					"required": []string{"request_key", "parameters"},
				}
				if operation["id"] == "project.export" {
					alternative["if"] = map[string]any{"properties": map[string]any{"parameters": map[string]any{
						"properties": map[string]any{"format": map[string]any{"const": "project"}},
					}}}
					alternative["then"] = map[string]any{"properties": map[string]any{"scope": map[string]any{"maxProperties": 0}}}
					alternative["else"] = map[string]any{"required": []string{"scope"},
						"properties": map[string]any{"scope": map[string]any{"required": []string{"sessions"}}}}
				}
				alternatives = append(alternatives, alternative)
			}
			schema["anyOf"] = alternatives
		}
		copy := *tool
		copy.InputSchema = schema
		list.Tools[i] = &copy
	}
	return nil
}

type operationCatalog struct {
	Operations []map[string]any `json:"operations"`
}

func addOperationCatalog(server *sdk.Server, call func(context.Context, string, any, any) error, family, description string, operationIDs ...string) {
	name := "describe_operations"
	if family != "" {
		name = "describe_" + family + "_operations"
	}
	sdk.AddTool(server, &sdk.Tool{Name: name, Description: description, Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true}},
		func(ctx context.Context, _ *sdk.CallToolRequest, _ struct{}) (*sdk.CallToolResult, operationCatalog, error) {
			var all []map[string]any
			err := call(ctx, "operations.describe", nil, &all)
			result := operationCatalog{Operations: make([]map[string]any, 0)}
			for _, operation := range all {
				id, _ := operation["id"].(string)
				if family == "" || (len(operationIDs) == 0 && strings.HasPrefix(id, family+".")) || slices.Contains(operationIDs, id) {
					if family == "" {
						operation = maps.Clone(operation)
						operation["tool"] = operationTool(id)
					}
					result.Operations = append(result.Operations, operation)
				}
			}
			return nil, result, err
		})
}
