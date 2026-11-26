package mcp

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

const maxReportSize = 128 << 10

type artifactPageInput struct {
	ID     jobs.JobID `json:"job_id"`
	Offset int        `json:"offset,omitempty" jsonschema:"Zero-based offset from next_offset"`
	Limit  int        `json:"limit,omitempty" jsonschema:"Page size 1 through 100, default 50"`
}

type artifactItem struct {
	Index     int    `json:"index"`
	Path      string `json:"path" jsonschema:"Path relative to output_directory"`
	URI       string `json:"uri"`
	ReportURI string `json:"report_uri,omitempty"`
}

type artifactPage struct {
	ID              jobs.JobID     `json:"job_id"`
	ArtifactID      string         `json:"artifact_id"`
	OutputDirectory string         `json:"output_directory"`
	Items           []artifactItem `json:"items"`
	Total           int            `json:"total"`
	NextOffset      *int           `json:"next_offset,omitempty"`
}

type artifactReceipt struct {
	ID       jobs.JobID `json:"job_id"`
	Manifest struct {
		ID       string           `json:"artifact_id"`
		Target   jobs.TargetRef   `json:"target"`
		Snapshot jobs.SnapshotRef `json:"snapshot"`
		Files    []string         `json:"files"`
	} `json:"manifest"`
}

func pageLimit(offset, limit int) (int, error) {
	if offset < 0 || limit < 0 || limit > 100 {
		return 0, errors.New("offset must be nonnegative and limit must be between 1 and 100")
	}
	if limit == 0 {
		return 50, nil
	}
	return limit, nil
}

func jobResourceURI(id jobs.JobID, path string) string {
	return "hdmapping://jobs/" + string(id) + "/" + path
}

func addArtifactResources(server *sdk.Server, call func(context.Context, string, any, any) error) {
	sdk.AddTool(server, &sdk.Tool{Name: "list_job_artifacts",
		Description: "List a completed job's owned files and report links. Use next_offset for another page. Point clouds are local artifact files.",
		Annotations: &sdk.ToolAnnotations{ReadOnlyHint: true}},
		func(ctx context.Context, _ *sdk.CallToolRequest, input artifactPageInput) (*sdk.CallToolResult, artifactPage, error) {
			page, err := readArtifactPage(ctx, call, input)
			return nil, page, err
		})
	for _, spec := range []struct{ path, name, description string }{
		{"artifacts", "Job artifact inventory", "First page of a completed job's owned files. Use list_job_artifacts for subsequent pages."},
		{"artifact/{index}", "Job artifact", "Local artifact path and optional report link."},
		{"report/{index}", "Job report", "A bounded text or JSON report from a completed job."},
	} {
		server.AddResourceTemplate(&sdk.ResourceTemplate{
			URITemplate: "hdmapping://jobs/{job_id}/" + spec.path, Name: spec.name, Description: spec.description,
		}, func(ctx context.Context, req *sdk.ReadResourceRequest) (*sdk.ReadResourceResult, error) {
			return readJobResource(ctx, call, req.Params.URI)
		})
	}
}

func readArtifactPage(ctx context.Context, call func(context.Context, string, any, any) error, input artifactPageInput) (artifactPage, error) {
	var page artifactPage
	limit, err := pageLimit(input.Offset, input.Limit)
	if err != nil {
		return page, err
	}
	root, directory, receipt, err := openArtifactReceipt(ctx, call, input.ID)
	if err != nil {
		return page, err
	}
	defer func() { _ = root.Close() }()
	files := receipt.Manifest.Files
	if input.Offset > len(files) {
		return page, errors.New("offset exceeds artifact count")
	}
	end := input.Offset + min(limit, len(files)-input.Offset)
	page = artifactPage{ID: input.ID, ArtifactID: receipt.Manifest.ID, OutputDirectory: directory, Total: len(files),
		Items: make([]artifactItem, 0, end-input.Offset)}
	for index := input.Offset; index < end; index++ {
		item, err := describeArtifact(input.ID, directory, files[index], index)
		if err != nil {
			return artifactPage{}, err
		}
		page.Items = append(page.Items, item)
	}
	if end < len(files) {
		page.NextOffset = &end
	}
	return page, nil
}

func openArtifactReceipt(ctx context.Context, call func(context.Context, string, any, any) error, id jobs.JobID) (*os.Root, string, artifactReceipt, error) {
	var receipt artifactReceipt
	var record jobs.JobRecord
	if err := call(ctx, "jobs.get", id, &record); err != nil {
		return nil, "", receipt, err
	}
	result := record.Result
	if record.ID != id || record.Status != jobs.Completed || result == nil || !result.Confirmed || result.Snapshot == nil {
		return nil, "", receipt, errors.New("job has no confirmed publication")
	}
	dir := result.OutputDirectory
	receiptName, err := artifactName(dir, result.ManifestPath)
	if err != nil {
		return nil, "", receipt, err
	}
	root, err := os.OpenRoot(dir)
	if err != nil {
		return nil, "", receipt, fmt.Errorf("open job result directory: %w", err)
	}
	data, err := readPublishedFile(root, receiptName, protocol.MaxFrameSize)
	if err == nil {
		err = json.Unmarshal(data, &receipt)
	}
	if err != nil || receipt.ID != id || receipt.Manifest.ID == "" || receipt.Manifest.Files == nil ||
		receipt.Manifest.Target != record.Request.Operation.Target || receipt.Manifest.Snapshot.Target != result.Snapshot.Target {
		_ = root.Close()
		return nil, "", artifactReceipt{}, errors.New("invalid published artifact receipt")
	}
	return root, dir, receipt, nil
}

func artifactName(directory, name string) (string, error) {
	if filepath.IsAbs(name) {
		var err error
		name, err = filepath.Rel(directory, name)
		if err != nil {
			return "", fmt.Errorf("resolve artifact path: %w", err)
		}
	}
	if !filepath.IsLocal(name) || filepath.Clean(name) == "." {
		return "", errors.New("artifact path is outside its job directory")
	}
	return filepath.Clean(name), nil
}

func describeArtifact(id jobs.JobID, directory, name string, index int) (artifactItem, error) {
	name, err := artifactName(directory, name)
	if err != nil {
		return artifactItem{}, err
	}
	suffix := strconv.Itoa(index)
	item := artifactItem{Index: index, Path: name, URI: jobResourceURI(id, "artifact/"+suffix)}
	if reportMIME(name) != "" {
		item.ReportURI = jobResourceURI(id, "report/"+suffix)
	}
	return item, nil
}

func reportMIME(name string) string {
	base := filepath.Base(name)
	switch base {
	case "candidates.json", "constraints.json", "project-items.json", "snapshot.json":
		return "application/json"
	case "rms.csv":
		return "text/csv"
	}
	if strings.HasPrefix(base, "HDMapping_results_") && filepath.Ext(base) == ".json" {
		return "application/json"
	}
	return ""
}

func readPublishedFile(root *os.Root, name string, limit int64) ([]byte, error) {
	info, err := root.Stat(name)
	if err != nil {
		return nil, fmt.Errorf("inspect published file: %w", err)
	}
	if !info.Mode().IsRegular() || info.Size() > limit {
		return nil, errors.New("published file is not a regular file within the resource size limit")
	}
	file, err := root.Open(name)
	if err != nil {
		return nil, fmt.Errorf("open published file: %w", err)
	}
	defer func() { _ = file.Close() }()
	data, err := io.ReadAll(io.LimitReader(file, limit+1))
	if err != nil {
		return nil, fmt.Errorf("read published file: %w", err)
	}
	if int64(len(data)) > limit || !utf8.Valid(data) {
		return nil, errors.New("published file exceeds the resource size limit or is not UTF-8")
	}
	return data, nil
}

func readJobResource(ctx context.Context, call func(context.Context, string, any, any) error, uri string) (*sdk.ReadResourceResult, error) {
	u, err := url.Parse(uri)
	if err != nil || u.Scheme != "hdmapping" || u.Host != "jobs" || u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return nil, sdk.ResourceNotFoundError(uri)
	}
	parts := strings.Split(strings.TrimPrefix(u.Path, "/"), "/")
	if len(parts) < 2 {
		return nil, sdk.ResourceNotFoundError(uri)
	}
	rawID, err := hex.DecodeString(parts[0])
	if err != nil || len(rawID) != 16 {
		return nil, sdk.ResourceNotFoundError(uri)
	}
	id := jobs.JobID(parts[0])
	mime := "application/json"
	var data []byte
	switch {
	case len(parts) == 2 && parts[1] == "artifacts":
		page, err := readArtifactPage(ctx, call, artifactPageInput{ID: id})
		if err != nil {
			return nil, err
		}
		data, err = json.Marshal(page)
		if err != nil {
			return nil, fmt.Errorf("encode artifact inventory: %w", err)
		}
	case len(parts) == 3 && (parts[1] == "artifact" || parts[1] == "report"):
		index, err := strconv.Atoi(parts[2])
		if err != nil || index < 0 {
			return nil, sdk.ResourceNotFoundError(uri)
		}
		root, dir, receipt, err := openArtifactReceipt(ctx, call, id)
		if err != nil {
			return nil, err
		}
		defer func() { _ = root.Close() }()
		if index >= len(receipt.Manifest.Files) {
			return nil, sdk.ResourceNotFoundError(uri)
		}
		item, err := describeArtifact(id, dir, receipt.Manifest.Files[index], index)
		if err != nil {
			return nil, err
		}
		if parts[1] == "report" {
			mime = reportMIME(item.Path)
			if mime == "" {
				return nil, sdk.ResourceNotFoundError(uri)
			}
			data, err = readPublishedFile(root, item.Path, maxReportSize)
		} else {
			data, err = json.Marshal(struct {
				artifactItem
				OutputDirectory string `json:"output_directory"`
			}{item, dir})
		}
		if err != nil {
			return nil, err
		}
	default:
		return nil, sdk.ResourceNotFoundError(uri)
	}
	return &sdk.ReadResourceResult{Contents: []*sdk.ResourceContents{{URI: uri, MIMEType: mime, Text: string(data)}}}, nil
}
