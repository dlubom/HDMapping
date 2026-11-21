package jobs

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

const crashExitCode = 37

// crashJournal exits inside an append, without running Store.Close or defers.
// Syncing the partial bytes makes the torn tail deterministic across platforms.
type crashJournal struct {
	journalFile
	t *testing.T
}

func (f crashJournal) Write(data []byte) (int, error) {
	f.t.Helper()
	if _, err := f.journalFile.Write(data[:min(80, len(data))]); err != nil {
		f.t.Fatal(err)
	}
	if err := f.Sync(); err != nil {
		f.t.Fatal(err)
	}
	os.Exit(crashExitCode)
	return 0, nil
}

func TestRecoveryProcessHelper(t *testing.T) {
	path := os.Getenv("HDMAPPING_JOBS_CRASH_PATH")
	if path == "" {
		return
	}
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	job, err := store.Submit(ctx, testRequest("crashed-request", "crashed-state"))
	if err != nil {
		t.Fatal(err)
	}
	mode := os.Getenv("HDMAPPING_JOBS_CRASH_MODE")
	if mode != "queued" {
		job.Status = Running
		if err = store.update(ctx, job); err != nil {
			t.Fatal(err)
		}
	}
	if err = json.NewEncoder(os.Stdout).Encode(job); err != nil {
		t.Fatal(err)
	}
	if mode == "torn-completion" {
		store.file = crashJournal{journalFile: store.file, t: t}
		job.Status = Completed
		job.Result = &Publication{Confirmed: true, OutputDirectory: "result", ManifestPath: "result/manifest.json"}
		err = store.update(ctx, job)
		t.Fatalf("completion append did not terminate the process: %v", err)
	}
	os.Exit(crashExitCode)
}

func TestAbruptProcessRecovery(t *testing.T) {
	for _, mode := range []string{"queued", "running", "torn-completion"} {
		t.Run(mode, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "crash rejestr żółć.jobs")
			executable, err := os.Executable()
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			cmd := exec.CommandContext(ctx, executable, "-test.run=^TestRecoveryProcessHelper$")
			cmd.Env = append(os.Environ(), "HDMAPPING_JOBS_CRASH_PATH="+path, "HDMAPPING_JOBS_CRASH_MODE="+mode)
			output, err := cmd.CombinedOutput()
			if ctx.Err() != nil {
				t.Fatalf("crash helper timed out: %v\n%s", ctx.Err(), output)
			}
			var exit *exec.ExitError
			if !errors.As(err, &exit) || exit.ExitCode() != crashExitCode {
				t.Fatalf("crash helper exit: %v\n%s", err, output)
			}
			var acknowledged JobRecord
			if err = json.Unmarshal(bytes.TrimSpace(output), &acknowledged); err != nil {
				t.Fatalf("helper acknowledgement: %v\n%s", err, output)
			}
			store, err := OpenStore(path)
			if err != nil {
				t.Fatal(err)
			}
			defer closeStore(t, store)
			job, err := store.Get(ctx, acknowledged.ID)
			want := Running
			if mode == "queued" {
				want = Queued
			}
			if err != nil || job.ID == "" || job.Status != want || job.Result != nil {
				t.Fatalf("recovered unacknowledged result: %+v, %v", job, err)
			}
			retry, err := store.Submit(ctx, testRequest("crashed-request", "crashed-state"))
			if err != nil || retry.ID != acknowledged.ID || retry.Status != want {
				t.Fatalf("retry after crash: %+v, %v", retry, err)
			}
			if _, err = store.Submit(ctx, testRequest("conflict", "crashed-state")); !errors.Is(err, ErrReserved) {
				t.Fatalf("reservation lost after crash: %v", err)
			}
			scheduler, err := NewScheduler(store, newExecutor())
			if scheduler != nil {
				if closeErr := scheduler.Close(); closeErr != nil {
					t.Error(closeErr)
				}
				t.Fatal("scheduler admitted unresolved crash state")
			}
			if !errors.Is(err, ErrRecoveryRequired) {
				t.Fatalf("crash state auto-replay guard: %v", err)
			}
			read := testRequest("read", "crashed-state")
			read.Access = AccessRead
			if _, err = store.Submit(ctx, read); err != nil {
				t.Fatalf("read blocked by recovered reservation: %v", err)
			}
			if err = store.Interrupt(ctx, acknowledged.ID); err != nil {
				t.Fatal(err)
			}
			if _, err = store.Submit(ctx, testRequest("replacement", "crashed-state")); err != nil {
				t.Fatalf("explicit recovery did not release reservation: %v", err)
			}
		})
	}
}

type admissionResult struct {
	job JobRecord
	err error
}

func concurrentAdmissions(t *testing.T, store *Store, requests []SubmitRequest) []admissionResult {
	t.Helper()
	start := make(chan struct{})
	results := make(chan admissionResult, len(requests))
	var workers sync.WaitGroup
	for _, req := range requests {
		workers.Go(func() {
			<-start
			job, err := store.Submit(context.Background(), req)
			results <- admissionResult{job, err}
		})
	}
	close(start)
	done := make(chan struct{})
	go func() {
		workers.Wait()
		close(results)
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(15 * time.Second):
		t.Fatal("concurrent admission did not finish")
	}
	var collected []admissionResult
	for result := range results {
		collected = append(collected, result)
	}
	return collected
}

func TestConcurrentDuplicateAdmission(t *testing.T) {
	store, _ := openTestStore(t)
	requests := make([]SubmitRequest, 16)
	for i := range requests {
		requests[i] = testRequest("same-request", "same-state")
		if i%2 == 0 {
			requests[i].Operation.Parameters = []byte(`{ "n" : 1 }`)
		}
	}
	var id JobID
	for _, result := range concurrentAdmissions(t, store, requests) {
		if result.err != nil {
			t.Fatal(result.err)
		}
		if id == "" {
			id = result.job.ID
		}
		if id == "" || result.job.ID != id {
			t.Fatalf("concurrent retry duplicated job: %q != %q", result.job.ID, id)
		}
	}
	jobs, err := store.List(context.Background(), JobFilter{})
	if err != nil || len(jobs) != 1 {
		t.Fatalf("retry created %d records: %v", len(jobs), err)
	}
}

func TestConcurrentMixedSourceReservations(t *testing.T) {
	store, _ := openTestStore(t)
	requests := make([]SubmitRequest, 16)
	for i := range requests {
		requests[i] = testRequest(fmt.Sprintf("request-%d", i), fmt.Sprintf("state-%d", i%2))
		if i%4 < 2 {
			requests[i].Source = SourceGUI
		}
	}
	winners := make(map[string]JobID)
	rejected := 0
	for _, result := range concurrentAdmissions(t, store, requests) {
		if errors.Is(result.err, ErrReserved) {
			rejected++
			continue
		}
		if result.err != nil {
			t.Fatal(result.err)
		}
		state := result.job.Request.Operation.Target.StateID
		if winners[state] != "" {
			t.Fatalf("state %s admitted two mutations", state)
		}
		winners[state] = result.job.ID
	}
	if len(winners) != 2 || rejected != len(requests)-2 {
		t.Fatalf("independent reservations: winners=%v rejected=%d", winners, rejected)
	}
}
