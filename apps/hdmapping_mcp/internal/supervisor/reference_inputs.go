package supervisor

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
)

type referenceFile struct {
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

const referenceInventory = "inventory.json"

func hasReferenceFiles(operation jobs.OperationRequest) bool {
	switch operation.Operation {
	case "gnss.import", "tum.import", "session.import_poses", "session.import_observations":
		return true
	case "export_result":
		var options struct {
			Format string `json:"format"`
		}
		return json.Unmarshal(operation.Parameters, &options) == nil && options.Format == "e57_poses"
	default:
		return false
	}
}

func referencePaths(operation jobs.OperationRequest) (map[string]json.RawMessage, []string, error) {
	var parameters map[string]json.RawMessage
	var paths []string
	if err := json.Unmarshal(operation.Parameters, &parameters); err != nil ||
		json.Unmarshal(parameters["paths"], &paths) != nil || len(paths) == 0 {
		return nil, nil, &jobs.JobError{Code: "invalid_parameters", Message: "File input requires a nonempty paths array"}
	}
	for _, path := range paths {
		if !filepath.IsAbs(path) {
			return nil, nil, &jobs.JobError{Code: "invalid_parameters", Message: "File input requires absolute local file paths"}
		}
	}
	return parameters, paths, nil
}

type referenceReader struct {
	ctx context.Context
	io.Reader
}

func (r referenceReader) Read(p []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	return r.Reader.Read(p)
}

// streamReference checks that the source is regular and observes changes during
// a copy. The captured bytes and their digest are produced in the same read.
func streamReference(ctx context.Context, path string, sink io.Writer) (result referenceFile, err error) {
	if err := ctx.Err(); err != nil {
		return result, err
	}
	original, err := os.Stat(path)
	if err != nil {
		return result, err
	}
	if !original.Mode().IsRegular() {
		return result, errors.New("reference input is not a regular file")
	}
	file, err := os.Open(path)
	if err != nil {
		return result, err
	}
	defer func() { err = errors.Join(err, file.Close()) }()
	opened, err := file.Stat()
	if err != nil {
		return result, err
	}
	if !opened.Mode().IsRegular() || !os.SameFile(original, opened) {
		return result, errors.New("reference input changed while being opened")
	}
	sum := sha256.New()
	n, err := io.Copy(io.MultiWriter(sink, sum), referenceReader{ctx, file})
	if err != nil {
		return result, err
	}
	current, err := file.Stat()
	if err != nil {
		return result, err
	}
	if original.Size() != n || current.Size() != n || !current.ModTime().Equal(original.ModTime()) {
		return result, errors.New("reference input changed while being read")
	}
	return referenceFile{Size: n, SHA256: hex.EncodeToString(sum.Sum(nil))}, nil
}

func (s *Supervisor) referenceDirectory(id jobs.JobID) (string, error) {
	jobDir, err := s.jobDirectory(id)
	if err != nil {
		return "", err
	}
	return filepath.Join(s.Directory, "references-"+filepath.Base(jobDir)), nil
}

func (s *Supervisor) stageReferences(ctx context.Context, job jobs.JobRecord) (err error) {
	_, paths, err := referencePaths(job.Request.Operation)
	if err != nil {
		return err
	}
	destination, err := s.referenceDirectory(job.ID)
	if err != nil {
		return err
	}
	if _, err := os.Lstat(destination); err == nil {
		return fmt.Errorf("reference input destination already exists: %s", destination)
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("inspect reference input destination: %w", err)
	}
	stage, err := os.MkdirTemp(s.Directory, "references-stage-")
	if err != nil {
		return fmt.Errorf("create reference input staging directory: %w", err)
	}
	defer func() { err = errors.Join(err, os.RemoveAll(stage)) }()
	s.reportProgress(job.ID)(event{Progress: &jobs.ProgressEvent{Stage: "stage_reference_input", Message: "Capture reference files"}})
	inventory := make([]referenceFile, 0, len(paths))
	for i, source := range paths {
		out, err := os.OpenFile(filepath.Join(stage, strconv.Itoa(i)), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return fmt.Errorf("create staged reference: %w", err)
		}
		received, copyErr := streamReference(ctx, source, out)
		if copyErr != nil {
			return fmt.Errorf("capture reference %q: %w", source, errors.Join(copyErr, out.Close()))
		}
		if err := errors.Join(out.Sync(), out.Close()); err != nil {
			return fmt.Errorf("persist staged reference: %w", err)
		}
		inventory = append(inventory, received)
	}
	file, err := os.OpenFile(filepath.Join(stage, referenceInventory), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return fmt.Errorf("create staged reference inventory: %w", err)
	}
	if err := errors.Join(json.NewEncoder(file).Encode(inventory), file.Close()); err != nil {
		return fmt.Errorf("write staged reference inventory: %w", err)
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := os.Rename(stage, destination); err != nil {
		return fmt.Errorf("publish staged references: %w", err)
	}
	// Confirm the inventory and the final directory entry before returning a
	// successful preparation to the scheduler.
	file, err = os.OpenFile(filepath.Join(destination, referenceInventory), os.O_RDWR, 0)
	if err == nil {
		err = errors.Join(jobs.SyncRuntimeReceipt(file), file.Close())
	}
	if err != nil {
		return errors.Join(fmt.Errorf("persist staged reference inventory: %w", err), os.RemoveAll(destination))
	}
	return nil
}

func (s *Supervisor) useStagedReferences(ctx context.Context, job jobs.JobRecord) (jobs.OperationRequest, error) {
	operation := job.Request.Operation
	parameters, paths, err := referencePaths(operation)
	if err != nil {
		return operation, err
	}
	dir, err := s.referenceDirectory(job.ID)
	if err != nil {
		return operation, err
	}
	data, err := os.ReadFile(filepath.Join(dir, referenceInventory))
	if err != nil {
		return operation, fmt.Errorf("read staged reference inventory: %w", err)
	}
	var inventory []referenceFile
	if err := json.Unmarshal(data, &inventory); err != nil || len(inventory) != len(paths) {
		return operation, errors.New("invalid staged reference inventory")
	}
	for i, entry := range inventory {
		paths[i] = filepath.Join(dir, strconv.Itoa(i))
		info, err := os.Lstat(paths[i])
		if err != nil {
			return operation, fmt.Errorf("inspect staged reference %d: %w", i, err)
		}
		if !info.Mode().IsRegular() || info.Size() != entry.Size {
			return operation, fmt.Errorf("staged reference %d has changed", i)
		}
		actual, err := streamReference(ctx, paths[i], io.Discard)
		if err != nil {
			return operation, fmt.Errorf("verify staged reference %d: %w", i, err)
		}
		if actual != entry {
			return operation, fmt.Errorf("staged reference %d has changed", i)
		}
	}
	parameters["paths"], err = json.Marshal(paths)
	if err != nil {
		return operation, fmt.Errorf("encode staged reference paths: %w", err)
	}
	operation.Parameters, err = json.Marshal(parameters)
	return operation, err
}
