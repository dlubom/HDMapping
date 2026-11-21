package supervisor

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
)

func cameraPaths(operation jobs.OperationRequest) (map[string]json.RawMessage, []string, error) {
	var parameters map[string]json.RawMessage
	var paths []string
	if json.Unmarshal(operation.Parameters, &parameters) != nil ||
		json.Unmarshal(parameters["camera_dirs"], &paths) != nil || len(paths) == 0 {
		return nil, nil, &jobs.JobError{Code: "invalid_parameters", Message: "Loop detection requires a nonempty camera_dirs array"}
	}
	for _, path := range paths {
		if !filepath.IsAbs(path) || strings.ContainsRune(path, 0) {
			return nil, nil, &jobs.JobError{Code: "invalid_parameters", Message: "Camera folders must be absolute local paths"}
		}
	}
	return parameters, paths, nil
}

func (s *Supervisor) cameraDirectory(id jobs.JobID) (string, error) {
	jobDir, err := s.jobDirectory(id)
	return filepath.Join(s.Directory, "cameras-"+filepath.Base(jobDir)), err
}

func captureCamera(ctx context.Context, source, destination string) (map[string]referenceFile, error) {
	entries, err := os.ReadDir(source)
	if err != nil {
		return nil, err
	}
	if err := os.Mkdir(destination, 0700); err != nil {
		return nil, err
	}
	files := make(map[string]referenceFile)
	var last string
	for _, entry := range entries {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		name := entry.Name()
		if entry.IsDir() || filepath.Ext(name) != ".jpg" {
			continue
		}
		out, err := os.OpenFile(filepath.Join(destination, name), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return nil, err
		}
		record, err := streamReference(ctx, filepath.Join(source, name), out)
		if err != nil {
			return nil, errors.Join(err, out.Close())
		}
		if err := errors.Join(out.Sync(), out.Close()); err != nil {
			return nil, err
		}
		files[name] = record
		last = name
	}
	if last != "" {
		file, err := os.OpenFile(filepath.Join(destination, last), os.O_RDWR, 0)
		if err != nil {
			return nil, err
		}
		if err := errors.Join(jobs.SyncRuntimeReceipt(file), file.Close()); err != nil {
			return nil, err
		}
	}
	return files, nil
}

func (s *Supervisor) stageCameras(ctx context.Context, job jobs.JobRecord) (err error) {
	_, sources, err := cameraPaths(job.Request.Operation)
	if err != nil {
		return err
	}
	destination, err := s.cameraDirectory(job.ID)
	if err != nil {
		return err
	}
	if _, err := os.Lstat(destination); err == nil || !errors.Is(err, os.ErrNotExist) {
		if err == nil {
			return fmt.Errorf("camera capture directory already exists: %q", destination)
		}
		return fmt.Errorf("inspect camera capture directory: %w", err)
	}
	stage, err := os.MkdirTemp(s.Directory, "cameras-stage-")
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, os.RemoveAll(stage)) }()
	inventory := make([]map[string]referenceFile, len(sources))
	s.reportProgress(job.ID)(event{Progress: &jobs.ProgressEvent{Stage: "stage_camera_input", Message: "Capture camera images"}})
	for i, source := range sources {
		inventory[i], err = captureCamera(ctx, source, filepath.Join(stage, strconv.Itoa(i)))
		if err != nil {
			return fmt.Errorf("capture camera folder %q: %w", source, err)
		}
	}
	path := filepath.Join(stage, referenceInventory)
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	if err := errors.Join(json.NewEncoder(file).Encode(inventory), file.Close()); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := os.Rename(stage, destination); err != nil {
		return err
	}
	file, err = os.OpenFile(filepath.Join(destination, referenceInventory), os.O_RDWR, 0)
	if err == nil {
		err = errors.Join(jobs.SyncRuntimeReceipt(file), file.Close())
	}
	if err != nil {
		return errors.Join(err, os.RemoveAll(destination))
	}
	return nil
}

func verifyCamera(ctx context.Context, folder string, expected map[string]referenceFile) error {
	entries, err := os.ReadDir(folder)
	if err != nil {
		return err
	}
	if len(entries) != len(expected) {
		return errors.New("staged camera frame inventory changed")
	}
	for _, entry := range entries {
		if !entry.Type().IsRegular() {
			return fmt.Errorf("staged camera frame %q is not a regular file", entry.Name())
		}
		actual, err := streamReference(ctx, filepath.Join(folder, entry.Name()), io.Discard)
		if err != nil {
			return err
		}
		if actual != expected[entry.Name()] {
			return fmt.Errorf("staged camera frame %q changed", entry.Name())
		}
	}
	return nil
}

func (s *Supervisor) useStagedCameras(ctx context.Context, job jobs.JobRecord) (jobs.OperationRequest, error) {
	operation := job.Request.Operation
	parameters, paths, err := cameraPaths(operation)
	if err != nil {
		return operation, err
	}
	dir, err := s.cameraDirectory(job.ID)
	if err != nil {
		return operation, err
	}
	raw, err := os.ReadFile(filepath.Join(dir, referenceInventory))
	if err != nil {
		return operation, err
	}
	var inventory []map[string]referenceFile
	if json.Unmarshal(raw, &inventory) != nil || len(inventory) != len(paths) {
		return operation, errors.New("invalid staged camera inventory")
	}
	for i, entries := range inventory {
		paths[i] = filepath.Join(dir, strconv.Itoa(i))
		if err := verifyCamera(ctx, paths[i], entries); err != nil {
			return operation, fmt.Errorf("verify staged camera %d: %w", i, err)
		}
	}
	parameters["camera_dirs"], err = json.Marshal(paths)
	if err != nil {
		return operation, err
	}
	operation.Parameters, err = json.Marshal(parameters)
	return operation, err
}

// A completed report owns its review frames. Discard only the coordinator's temporary copies.
func (s *Supervisor) discardCameraCapture(id jobs.JobID) {
	dir, err := s.cameraDirectory(id)
	if err == nil {
		err = os.RemoveAll(dir)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "discard camera capture for %s: %v\n", id, err)
	}
}
