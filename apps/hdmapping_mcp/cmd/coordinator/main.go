package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/gui"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/supervisor"
)

var productVersion string

func main() {
	if err := run(); err != nil {
		log.New(os.Stderr, "hdmapping_coordinator: ", 0).Print(err)
		os.Exit(1)
	}
}

func run() error {
	if productVersion == "" {
		return errors.New("product version missing; build through CMake")
	}
	if len(os.Args) != 3 || !filepath.IsAbs(os.Args[2]) {
		return errors.New("expected serve or supervise with an absolute directory")
	}
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	worker, err := protocol.Companion(executable, "hdmapping_coordinator", "hdmapping_worker")
	if err != nil {
		return err
	}
	switch os.Args[1] {
	case "supervise":
		return supervisor.Keep(os.Args[2], worker, productVersion)
	case "serve":
		if err := protocol.VerifyDetached(); err != nil {
			return err
		}
		dir, err := protocol.ValidateDirectory(os.Args[2])
		if err != nil {
			return err
		}
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		return serve(ctx, &supervisor.Supervisor{Directory: dir, Coordinator: executable, Worker: worker, Version: productVersion})
	default:
		return fmt.Errorf("unsupported coordinator mode %q", os.Args[1])
	}
}

func serve(ctx context.Context, processes *supervisor.Supervisor) (err error) {
	store, err := jobs.OpenStore(filepath.Join(processes.Directory, "jobs.journal"))
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, store.Close()) }()
	recovery, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	if err := processes.Recover(recovery, store); err != nil {
		return err
	}
	g := &guiJobs{Supervisor: processes, registry: gui.NewRegistry(), owners: make(map[jobs.JobID]*gui.Session)}
	life, stop := context.WithCancel(ctx)
	g.wg.Go(func() { g.registry.Expire(life) })
	defer func() { stop(); g.wg.Wait() }()
	scheduler, err := jobs.NewScheduler(store, g)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, scheduler.Close()) }()
	processes.Progress = func(id jobs.JobID, progress jobs.ProgressEvent) error {
		return scheduler.ReportProgress(context.Background(), id, progress)
	}
	return protocol.Serve(ctx, processes.Directory, processes.Version, dispatch(scheduler, g))
}

func dispatch(scheduler *jobs.Scheduler, processes *guiJobs) protocol.Handler {
	// Calls awaiting a GUI must leave transport slots for its polls and replies.
	awaiting := make(chan struct{}, 16)
	return func(ctx context.Context, method string, body json.RawMessage) (any, error) {
		if !strings.HasPrefix(method, "gui.") {
			select {
			case awaiting <- struct{}{}:
				defer func() { <-awaiting }()
			default:
				return nil, errors.New("coordinator busy: retry request")
			}
		}
		switch method {
		case "ping":
			return os.Getpid(), nil
		case "operations.describe":
			return processes.Describe(ctx)
		case "jobs.submit":
			var request jobs.SubmitRequest
			if err := json.Unmarshal(body, &request); err != nil {
				return nil, fmt.Errorf("decode submit request: %w", err)
			}
			if request.Snapshot != nil && !filepath.IsAbs(request.Snapshot.ManifestPath) {
				return nil, errors.New("submit requires an absolute captured snapshot path")
			}
			if err := supervisor.CheckArgumentDepth(request.Operation.Scope); err != nil {
				return nil, fmt.Errorf("scope: %w", err)
			}
			if err := supervisor.CheckArgumentDepth(request.Operation.Parameters); err != nil {
				return nil, fmt.Errorf("parameters: %w", err)
			}
			live := request.Snapshot == nil && request.Recording == nil &&
				(request.Operation.Operation != jobs.OpenSession || request.CaptureSource != "")
			if live {
				if request.CaptureSource != request.Operation.Target.StateID {
					return nil, errors.New("live capture requires its target state ID")
				}
			}
			if live || request.Source == jobs.SourceGUI {
				return processes.submit(ctx, scheduler, request)
			}
			return scheduler.SubmitForPreparation(ctx, request)
		case "jobs.get", "jobs.cancel":
			var id jobs.JobID
			if err := json.Unmarshal(body, &id); err != nil {
				return nil, fmt.Errorf("decode job ID: %w", err)
			}
			if method == "jobs.cancel" {
				return scheduler.Cancel(ctx, id)
			}
			return scheduler.Get(ctx, id)
		case "jobs.list":
			var filter jobs.JobFilter
			if err := json.Unmarshal(body, &filter); err != nil {
				return nil, fmt.Errorf("decode job filter: %w", err)
			}
			return scheduler.List(ctx, filter)
		default:
			return processes.handle(ctx, method, body)
		}
	}
}
