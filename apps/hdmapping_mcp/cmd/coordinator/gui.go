package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/gui"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/supervisor"
)

// A reservation pins the original session across removal of its visible state.
type guiJobs struct {
	*supervisor.Supervisor
	registry *gui.Registry
	mu       sync.Mutex
	owners   map[jobs.JobID]*gui.Session
	wg       sync.WaitGroup
}
type captureCommand struct {
	Target    jobs.TargetRef         `json:"target"`
	Job       jobs.JobID             `json:"job_id"`
	Access    jobs.Access            `json:"access"`
	Phase     string                 `json:"phase"`
	Directory string                 `json:"directory,omitempty"`
	Operation *jobs.OperationRequest `json:"operation,omitempty"`
}

func capture(j jobs.JobRecord, phase, dir string) captureCommand {
	c := captureCommand{Target: j.Request.Operation.Target, Job: j.ID, Access: j.Request.Access, Phase: phase, Directory: dir}
	if phase == "reserve" {
		if j.Request.Operation.Operation == jobs.OpenSession {
			c.Phase = "reserve_external_session"
		} else {
			c.Operation = &j.Request.Operation
		}
	}
	return c
}

func (g *guiJobs) findCaptureOwner(ctx context.Context, req jobs.SubmitRequest) (*gui.Session, error) {
	id := req.Operation.Target.StateID
	if req.Source != jobs.SourceGUI {
		return g.registry.Find(id)
	}
	// The job RPC may start a coordinator before the independent desktop
	// poll has advertised its opening. Allow a few poll cycles for discovery.
	wait, cancel := context.WithTimeout(ctx, time.Second)
	defer cancel()
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		owner, err := g.registry.Find(id)
		if err == nil {
			return owner, nil
		}
		select {
		case <-wait.Done():
			return nil, err
		case <-ticker.C:
		}
	}
}

func (g *guiJobs) submit(ctx context.Context, scheduler *jobs.Scheduler, req jobs.SubmitRequest) (jobs.JobRecord, error) {
	var owner *gui.Session
	var reserved jobs.JobRecord
	fresh := false
	job, err := scheduler.SubmitWithReservation(ctx, req, func(j jobs.JobRecord) error {
		fresh = true
		if req.Snapshot != nil || req.Recording != nil {
			return nil
		}
		var err error
		owner, err = g.findCaptureOwner(ctx, req)
		if err != nil {
			return err
		}
		if _, err = owner.Call(ctx, "states.capture", capture(j, "reserve", "")); err != nil {
			if errors.Is(err, context.DeadlineExceeded) || errors.Is(err, context.Canceled) {
				// The deadline may have raced with dispatch. Undo a possible pin.
				g.registry.Release(owner, capture(j, "release", ""))
			}
			return err
		}
		reserved = j
		g.mu.Lock()
		g.owners[j.ID] = owner
		g.mu.Unlock()
		return nil
	})
	if !fresh || (job.ID == "" && reserved.ID == "") {
		return job, err
	}
	g.wg.Go(func() {
		if job.ID != "" {
			var sourceDone <-chan struct{}
			if reserved.ID != "" && req.Operation.Operation != jobs.OpenSession {
				sourceDone = owner.Done()
			}
			terminal, waitErr := scheduler.WaitForSource(context.Background(), job.ID, sourceDone)
			if waitErr == nil && terminal.Status == jobs.Completed && terminal.GUIApplyStatus == "pending" {
				status := g.apply(terminal)
				if setErr := scheduler.RecordGUIApply(context.Background(), job.ID, status); setErr != nil {
					fmt.Fprintln(os.Stderr, setErr)
				}
			}
		}
		if reserved.ID == "" {
			return
		}
		g.mu.Lock()
		delete(g.owners, reserved.ID)
		g.mu.Unlock()
		g.registry.Release(owner, capture(reserved, "release", ""))
	})
	return job, err
}
func (g *guiJobs) apply(j jobs.JobRecord) string {
	ctx := context.Background() // A native reload may take longer than a client RPC.
	owner, err := g.registry.Find(j.Request.Operation.Target.StateID)
	if err != nil {
		return "closed"
	}
	_, err = owner.Call(ctx, "states.apply_result", struct {
		Target       jobs.TargetRef `json:"target"`
		Job          jobs.JobID     `json:"job_id"`
		ManifestPath string         `json:"manifest_path"`
	}{j.Request.Operation.Target, j.ID, j.Result.ManifestPath})
	if errors.Is(err, jobs.ErrSourceLost) {
		return "unconfirmed"
	}
	var rejected *gui.CommandError
	if errors.As(err, &rejected) {
		switch rejected.Code() {
		case "stale_revision":
			return "stale_revision"
		case "state_closed":
			return "closed"
		}
	}
	if err != nil {
		return "not_applied"
	}
	return "applied"
}
func (g *guiJobs) Prepare(ctx context.Context, j jobs.JobRecord) (jobs.SnapshotRef, error) {
	operation := j.Request.Operation.Operation
	if j.Request.Snapshot != nil || j.Request.Recording != nil || operation == jobs.OpenSession ||
		(operation == jobs.OpenProject && j.Request.CaptureSource == "") {
		return g.Supervisor.Prepare(ctx, j)
	}
	g.mu.Lock()
	owner := g.owners[j.ID]
	g.mu.Unlock()
	if owner == nil {
		return jobs.SnapshotRef{}, jobs.ErrSourceLost
	}
	// Use a separate input directory. The worker publishes into a fresh job
	// directory and its recovery protocol owns that directory exclusively.
	prefix := "inputs-"
	if operation == jobs.OpenProject || operation == jobs.AddProjectSession {
		prefix = "opening-"
	}
	dir := filepath.Join(g.Directory, prefix+string(j.ID))
	// CaptureSnapshot stages beside this unused destination and publishes it atomically.
	raw, err := owner.Call(ctx, "states.capture", capture(j, "write", dir))
	if err != nil {
		return jobs.SnapshotRef{}, err
	}
	var snapshot jobs.SnapshotRef
	if err = json.Unmarshal(raw, &snapshot); err != nil {
		return snapshot, fmt.Errorf("decode captured snapshot: %w", err)
	}
	if snapshot.Target != j.Request.Operation.Target || !filepath.IsAbs(snapshot.ManifestPath) {
		return snapshot, errors.New("GUI returned an invalid capture for this target")
	}
	resolved, err := filepath.EvalSymlinks(snapshot.ManifestPath)
	if err != nil {
		return snapshot, err
	}
	rel, err := filepath.Rel(dir, resolved)
	if err != nil || !filepath.IsLocal(rel) {
		return snapshot, errors.New("GUI capture manifest outside reserved directory")
	}
	if operation == jobs.OpenProject || operation == jobs.AddProjectSession {
		defer func() {
			if err := os.RemoveAll(dir); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		}()
	}
	j.Request.Snapshot = &snapshot
	return g.Supervisor.Prepare(ctx, j)
}
func (g *guiJobs) handle(ctx context.Context, method string, body json.RawMessage) (any, error) {
	switch method {
	case "gui.connect":
		return g.registry.Connect()
	case "gui.poll":
		var p gui.Poll
		if err := json.Unmarshal(body, &p); err != nil {
			return nil, err
		}
		if p.Wire != protocol.WireVersion || p.Version != g.Version {
			return nil, errors.New("incompatible GUI bridge version")
		}
		command, err := g.registry.Poll(p)
		return gui.Envelope{Wire: p.Wire, Version: g.Version, Command: command}, err
	case "gui.disconnect":
		var key string
		if err := json.Unmarshal(body, &key); err != nil {
			return nil, err
		}
		g.registry.Remove(key)
		return nil, nil
	case "states.list":
		targets, err := g.registry.List(ctx)
		return map[string]any{"states": targets}, err
	case "states.describe", "states.execute", "states.list_project_items":
		var req struct {
			Target jobs.TargetRef `json:"target"`
		}
		if err := json.Unmarshal(body, &req); err != nil {
			return nil, err
		}
		s, err := g.registry.Find(req.Target.StateID)
		if err != nil {
			return nil, err
		}
		return s.Call(ctx, method, body)
	default:
		return nil, fmt.Errorf("unsupported coordinator method %q", method)
	}
}
