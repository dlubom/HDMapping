package supervisor

import (
	"bufio"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

type Supervisor struct {
	Directory   string
	Coordinator string
	Worker      string
	Version     string
	Progress    func(jobs.JobID, jobs.ProgressEvent) error
}

func (s *Supervisor) Describe(ctx context.Context) (json.RawMessage, error) {
	cmd := exec.Command(s.Worker)
	cmd.Dir = filepath.Dir(s.Worker)
	cmd.Stderr = os.Stderr
	reply, err := exchange(ctx, cmd, s.Version, command{Type: "describe"}, nil, false)
	if err != nil {
		return nil, err
	}
	if reply.Type != "operations" || len(reply.Operations) == 0 || reply.Operations[0] != '[' {
		return nil, fmt.Errorf("worker returned invalid operation catalog: %s", reply.Type)
	}
	return reply.Operations, nil
}

func (s *Supervisor) Prepare(ctx context.Context, job jobs.JobRecord) (jobs.SnapshotRef, error) {
	snapshot := job.Request.Snapshot
	if snapshot == nil || snapshot.Target != job.Request.Operation.Target {
		return jobs.SnapshotRef{}, errors.New("worker requires a captured snapshot for this revision")
	}
	cmd := exec.Command(s.Worker)
	cmd.Dir, cmd.Stderr = filepath.Dir(s.Worker), os.Stderr
	reply, err := exchange(ctx, cmd, s.Version, command{Type: "validate_snapshot", Snapshot: snapshot}, nil, false)
	if err != nil {
		return jobs.SnapshotRef{}, err
	}
	if reply.Error != nil {
		return jobs.SnapshotRef{}, fmt.Errorf("%s: %s", reply.Error.Code, reply.Error.Message)
	}
	if reply.Type != "snapshot_validated" {
		return jobs.SnapshotRef{}, fmt.Errorf("unexpected snapshot validation response: %s", reply.Type)
	}
	return *snapshot, nil
}

func (s *Supervisor) Run(ctx context.Context, job jobs.JobRecord) (jobs.Publication, error) {
	dir, err := s.jobDirectory(job.ID)
	if err != nil {
		return jobs.Publication{}, err
	}
	if err := os.Mkdir(dir, 0700); err != nil {
		return jobs.Publication{}, fmt.Errorf("create job directory: %w", err)
	}
	cmd := exec.Command(s.Coordinator, "supervise", dir)
	cmd.Dir = dir
	cmd.Stderr = os.Stderr
	reply, runErr := exchange(ctx, cmd, s.Version, command{
		Type: "run", ID: job.ID, Operation: &job.Request.Operation, Snapshot: job.Snapshot,
	}, func(reply event) {
		if s.Progress == nil {
			return
		}
		if err := s.Progress(job.ID, *reply.Progress); err != nil {
			fmt.Fprintf(os.Stderr, "record job progress: %v\n", err)
		}
	}, true)
	if reply.Error != nil {
		runErr = errors.Join(runErr, fmt.Errorf("%s: %s", reply.Error.Code, reply.Error.Message))
	}
	// If the keeper itself was lost, the worker still owns its inherited
	// lease. Keep the reservation until it has stopped using its inputs.
	lease, err := awaitLease(context.Background(), dir)
	if err != nil {
		return jobs.Publication{}, errors.Join(runErr, err)
	}
	defer func() { _ = lease.Close() }()
	published, err := readReceipt(dir, s.Version, job)
	if err == nil {
		// Match P02: durable publication wins a concurrent cancellation.
		return published, nil
	}
	return jobs.Publication{}, errors.Join(runErr, err)
}

// Keep owns the worker independently of coordinator lifetime. The keeper
// acquires the lease before authorizing work, and holds it until after Wait.
func Keep(dir, worker, version string) (err error) {
	ignoreBrokenOutput()
	lease, err := jobs.LockRuntimeLease(filepath.Join(dir, "worker.log"))
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, lease.Close()) }()

	hello := event{Type: "hello", Hello: protocol.Hello{Wire: protocol.WireVersion, Version: version, PID: os.Getpid()}}
	if err := protocol.WriteFrame(os.Stdout, hello); err != nil {
		return err
	}
	reader := bufio.NewReader(os.Stdin)
	var req command
	if err := protocol.ReadFrame(reader, &req); err != nil {
		if errors.Is(err, io.EOF) {
			return nil
		}
		return err
	}
	if req.Type != "run" || string(req.ID) != filepath.Base(dir) || req.Operation == nil || req.Snapshot == nil {
		return errors.New("invalid keeper run command")
	}

	cmd := exec.Command(worker)
	cmd.Dir, cmd.Stderr = dir, lease
	ctx, cancel := context.WithCancel(context.Background())
	inputDone := make(chan struct{})
	go func() {
		defer close(inputDone)
		defer cancel()
		// The sole remaining command or EOF requests cancellation.
		_ = protocol.ReadFrame(reader, &command{})
	}()
	reply, runErr := exchange(ctx, cmd, version, req, func(reply event) {
		// Parent disconnection is handled by the stdin monitor.
		_ = protocol.WriteFrame(os.Stdout, reply)
	}, false)
	if runErr != nil {
		reply = event{Type: "error", Error: &jobs.JobError{Code: "worker_process", Message: runErr.Error()}}
	}
	writeErr := protocol.WriteFrame(os.Stdout, reply)
	// The coordinator closes its end after a terminal event, including when
	// startup or the worker failed. Join the cancellation reader before exit.
	<-inputDone
	return writeErr
}

func (s *Supervisor) jobDirectory(id jobs.JobID) (string, error) {
	decoded, err := hex.DecodeString(string(id))
	if err != nil || len(decoded) != 16 {
		return "", errors.New("invalid worker job ID")
	}
	return filepath.Join(s.Directory, string(id)), nil
}
