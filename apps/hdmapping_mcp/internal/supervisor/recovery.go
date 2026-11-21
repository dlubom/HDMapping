package supervisor

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
	"unicode/utf8"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

func readReceipt(dir, version string, job jobs.JobRecord) (result jobs.Publication, err error) {
	path := filepath.Join(dir, "result.json")
	// Windows FlushFileBuffers needs a writable handle.
	file, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return result, fmt.Errorf("open worker receipt: %w", err)
	}
	defer func() { err = errors.Join(err, file.Close()) }()
	data, err := io.ReadAll(io.LimitReader(file, protocol.MaxFrameSize+1))
	if err != nil {
		return result, fmt.Errorf("read worker receipt: %w", err)
	}
	var received receipt
	if len(data) > protocol.MaxFrameSize || !utf8.Valid(data) || json.Unmarshal(data, &received) != nil {
		return result, errors.New("invalid worker receipt")
	}
	if received.ID != job.ID || received.Version != version || received.Wire != protocol.WireVersion ||
		received.Manifest.ID == "" || received.Manifest.Target != job.Request.Operation.Target ||
		received.Manifest.Snapshot.Target.StateID == "" || received.Manifest.Files == nil ||
		received.Manifest.Dependencies == nil {
		return result, errors.New("worker receipt identity or manifest mismatch")
	}
	if err := validateOutputFiles(dir, received.Manifest.Snapshot.ManifestPath, received.Manifest.Files); err != nil {
		return result, err
	}
	if err := jobs.SyncRuntimeReceipt(file); err != nil {
		return result, fmt.Errorf("sync worker receipt: %w", err)
	}
	snapshot := received.Manifest.Snapshot
	if !filepath.IsAbs(snapshot.ManifestPath) {
		snapshot.ManifestPath = filepath.Join(dir, snapshot.ManifestPath)
	}
	return jobs.Publication{Confirmed: true, OutputDirectory: dir, ManifestPath: path, Snapshot: &snapshot}, nil
}

// Both the manifest and listed owned files must resolve within this job's
// output directory. External dependencies remain separate in the receipt.
func validateOutputFiles(dir, manifest string, files []string) error {
	root, err := os.OpenRoot(dir)
	if err != nil {
		return err
	}
	defer func() { _ = root.Close() }()
	for _, name := range append([]string{manifest}, files...) {
		if filepath.IsAbs(name) {
			name, err = filepath.Rel(dir, name)
			if err != nil {
				return fmt.Errorf("resolve result artifact: %w", err)
			}
		}
		if !filepath.IsLocal(name) {
			return fmt.Errorf("result artifact is outside the job directory: %q", name)
		}
		info, err := root.Stat(name)
		if err != nil {
			return fmt.Errorf("inspect result artifact %q: %w", name, err)
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("result artifact is not a regular file: %q", name)
		}
	}
	return nil
}

// Recover reconciles jobs while the store is exclusively held and before
// attaching the scheduler. It never restarts work or signals a saved PID.
func (s *Supervisor) Recover(ctx context.Context, store *jobs.Store) error {
	var after jobs.JobID
	for {
		page, err := store.List(ctx, jobs.JobFilter{After: after, Limit: 100})
		if err != nil || len(page) == 0 {
			return err
		}
		for _, job := range page {
			switch job.Status {
			case jobs.Preparing, jobs.Queued:
				err = store.Interrupt(ctx, job.ID)
			case jobs.Running, jobs.Cancelling:
				err = s.recoverRunning(ctx, store, job)
			}
			if err == nil && job.GUIApplyStatus == "pending" {
				// A crash may occur after GUI mutation but before its acknowledgement.
				// Preserve the artifact without replaying the mutation.
				var recovered jobs.JobRecord
				recovered, err = store.Get(ctx, job.ID)
				if err == nil && recovered.Status == jobs.Completed {
					status := "unconfirmed"
					if job.Request.Access == jobs.AccessRead && job.Request.Operation.Operation != jobs.DetectLoopCandidates {
						status = "not_requested"
					}
					err = store.RecordGUIApply(ctx, job.ID, status)
				}
			}
			if err != nil {
				return fmt.Errorf("recover job %s: %w", job.ID, err)
			}
			if job.Request.Operation.Operation == jobs.DetectLoopCandidates {
				s.discardCameraCapture(job.ID)
			}
			after = job.ID
		}
	}
}

func (s *Supervisor) recoverRunning(ctx context.Context, store *jobs.Store, job jobs.JobRecord) (err error) {
	dir, err := s.jobDirectory(job.ID)
	if err != nil {
		return err
	}
	lease, err := awaitLease(ctx, dir)
	if errors.Is(err, os.ErrNotExist) {
		return store.Interrupt(ctx, job.ID)
	}
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, lease.Close()) }()
	if published, err := readReceipt(dir, s.Version, job); err == nil {
		return store.RecoverPublication(ctx, job.ID, published)
	}
	return store.Interrupt(ctx, job.ID)
}

func awaitLease(ctx context.Context, dir string) (*os.File, error) {
	ticker := time.NewTicker(30 * time.Millisecond)
	defer ticker.Stop()
	for {
		lease, err := jobs.LockRuntimeLease(filepath.Join(dir, "worker.log"))
		if !errors.Is(err, jobs.ErrRuntimeLeaseBusy) {
			return lease, err
		}
		select {
		case <-ctx.Done():
			return nil, errors.Join(ctx.Err(), err)
		case <-ticker.C:
		}
	}
}
