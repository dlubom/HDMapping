package jobs

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

type execution struct {
	job     JobRecord
	ctx     context.Context
	result  chan Publication
	failure chan error
}
type controlledExecutor struct {
	started   chan execution
	preparing chan execution
}

func newExecutor() *controlledExecutor {
	return &controlledExecutor{started: make(chan execution, MaxLimit), preparing: make(chan execution, MaxLimit)}
}
func (e *controlledExecutor) Prepare(ctx context.Context, j JobRecord) (SnapshotRef, error) {
	call := execution{job: j, ctx: ctx, result: make(chan Publication, 1), failure: make(chan error, 1)}
	e.preparing <- call
	select {
	case <-call.result:
		return SnapshotRef{ManifestPath: "captured.json", Target: j.Request.Operation.Target}, nil
	case err := <-call.failure:
		return SnapshotRef{}, err
	case <-ctx.Done():
		return SnapshotRef{}, ctx.Err()
	}
}
func (e *controlledExecutor) Run(ctx context.Context, j JobRecord) (Publication, error) {
	call := execution{job: j, ctx: ctx, result: make(chan Publication, 1), failure: make(chan error, 1)}
	e.started <- call
	select {
	case p := <-call.result:
		return p, nil
	case err := <-call.failure:
		return Publication{}, err
	case <-ctx.Done():
		return Publication{}, ctx.Err()
	}
}
func receive(t *testing.T, c <-chan execution) execution {
	t.Helper()
	select {
	case v := <-c:
		return v
	case <-time.After(10 * time.Second):
		t.Fatal("executor did not start")
		return execution{}
	}
}
func schedulerFor(t *testing.T, e Executor) (*Scheduler, *Store) {
	t.Helper()
	store, _ := openTestStore(t)
	s, err := NewScheduler(store, e)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Error(err)
		}
	})
	return s, store
}
func submit(t *testing.T, s *Scheduler, r SubmitRequest) JobRecord {
	t.Helper()
	j, err := s.Submit(context.Background(), r)
	if err != nil {
		t.Fatal(err)
	}
	return j
}
func waitJob(t *testing.T, s *Scheduler, id JobID) JobRecord {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	j, err := s.Wait(ctx, id)
	if err != nil {
		t.Fatal(err)
	}
	return j
}
func published() Publication {
	return Publication{Confirmed: true, OutputDirectory: "result", ManifestPath: "result/manifest.json"}
}
func TestLimitTwoAndReservation(t *testing.T) {
	e := newExecutor()
	s, _ := schedulerFor(t, e)
	ctx := context.Background()
	a := testRequest("a", "state-a")
	a.Source = SourceGUI
	ja := submit(t, s, a)
	first := receive(t, e.started)
	jb := submit(t, s, testRequest("b", "state-b"))
	second := receive(t, e.started)
	jc := submit(t, s, testRequest("c", "state-c"))
	for _, id := range []JobID{ja.ID, jb.ID} {
		j, err := s.Get(ctx, id)
		if err != nil || j.Status != Running {
			t.Fatalf("running: %+v %v", j, err)
		}
	}
	j, err := s.Get(ctx, jc.ID)
	if err != nil || j.Status != Queued {
		t.Fatalf("queued: %+v %v", j, err)
	}
	conflict := testRequest("conflict", "state-c")
	conflict.Operation.Target.Revision = 2
	conflict.Snapshot.Target.Revision = 2
	if _, err = s.Submit(ctx, conflict); !errors.Is(err, ErrReserved) {
		t.Fatalf("queued reservation: %v", err)
	}
	read := testRequest("read", "state-a")
	read.Access = AccessRead
	readJob := submit(t, s, read)
	if _, err = s.List(ctx, JobFilter{StateID: "state-a"}); err != nil {
		t.Fatal(err)
	}
	if j, err = s.Cancel(ctx, jc.ID); err != nil || j.Status != Cancelled {
		t.Fatalf("queued cancel: %+v %v", j, err)
	}
	submit(t, s, testRequest("replacement", "state-c"))
	first.result <- published()
	if got := waitJob(t, s, ja.ID); got.Status != Completed || got.GUIApplyStatus != "pending" {
		t.Fatalf("completion: %+v", got)
	}
	next := receive(t, e.started)
	if next.job.ID != readJob.ID {
		t.Fatalf("FIFO read: %s", next.job.ID)
	}
	second.result <- published()
	next.result <- published()
}
func TestLowerLimitDoesNotStopRunning(t *testing.T) {
	e := newExecutor()
	s, store := schedulerFor(t, e)
	ctx := context.Background()
	a := submit(t, s, testRequest("a", "a"))
	first := receive(t, e.started)
	b := submit(t, s, testRequest("b", "b"))
	second := receive(t, e.started)
	c := submit(t, s, testRequest("c", "c"))
	if err := s.SetLimit(ctx, 1); err != nil {
		t.Fatal(err)
	}
	first.result <- published()
	waitJob(t, s, a.ID)
	if second.ctx.Err() != nil {
		t.Fatal("lowering limit cancelled executor")
	}
	got, err := s.Get(ctx, c.ID)
	if err != nil || got.Status != Queued {
		t.Fatalf("limit not respected: %+v %v", got, err)
	}
	second.result <- published()
	waitJob(t, s, b.ID)
	third := receive(t, e.started)
	if third.job.ID != c.ID {
		t.Fatal("wrong queued job")
	}
	third.result <- published()
	waitJob(t, s, c.ID)
	if err = s.Close(); err != nil {
		t.Fatal(err)
	}
	if store.state.limit != 1 {
		t.Fatal("limit not recorded")
	}
}
func TestPreparingAndDisconnectedRequest(t *testing.T) {
	e := newExecutor()
	s, _ := schedulerFor(t, e)
	req := testRequest("capture", "gui")
	req.Snapshot = nil
	req.CaptureSource = "gui-instance"
	req.Source = SourceGUI
	ctx, cancel := context.WithCancel(context.Background())
	j, err := s.Submit(ctx, req)
	if err != nil {
		t.Fatal(err)
	}
	cancel()
	prep := receive(t, e.preparing)
	if prep.ctx.Err() != nil {
		t.Fatal("request cancellation reached job")
	}
	if _, err = s.Submit(context.Background(), testRequest("conflict", "gui")); !errors.Is(err, ErrReserved) {
		t.Fatalf("preparing reservation: %v", err)
	}
	prep.result <- published()
	run := receive(t, e.started)
	if run.job.Snapshot == nil || run.job.Snapshot.ManifestPath != "captured.json" {
		t.Fatal("snapshot not handed off")
	}
	run.result <- published()
	if got := waitJob(t, s, j.ID); got.Status != Completed {
		t.Fatalf("%+v", got)
	}
}
func TestCancellationAndFailureRelease(t *testing.T) {
	for _, phase := range []string{"running", "preparing", "failure", "unpublished"} {
		t.Run(phase, func(t *testing.T) {
			e := newExecutor()
			s, _ := schedulerFor(t, e)
			req := testRequest("a", "state")
			if phase == "preparing" {
				req.Snapshot = nil
				req.CaptureSource = "gui"
			}
			j := submit(t, s, req)
			var call execution
			if phase == "preparing" {
				call = receive(t, e.preparing)
			} else {
				call = receive(t, e.started)
			}
			want := Cancelled
			switch phase {
			case "failure":
				call.failure <- errors.New("worker failed")
				want = Failed
			case "unpublished":
				call.result <- Publication{}
				want = Failed
			default:
				if _, err := s.Cancel(context.Background(), j.ID); err != nil {
					t.Fatal(err)
				}
			}
			if got := waitJob(t, s, j.ID); got.Status != want {
				t.Fatalf("got %s want %s", got.Status, want)
			}
			submit(t, s, testRequest("next", "state"))
		})
	}
}

type publishingExecutor struct {
	started chan struct{}
	release chan struct{}
}

func (e *publishingExecutor) Prepare(context.Context, JobRecord) (SnapshotRef, error) {
	return SnapshotRef{}, errors.New("unexpected preparation")
}
func (e *publishingExecutor) Run(ctx context.Context, _ JobRecord) (Publication, error) {
	close(e.started)
	<-e.release
	return published(), ctx.Err()
}
func TestPublicationWinsCancellation(t *testing.T) {
	e := &publishingExecutor{started: make(chan struct{}), release: make(chan struct{})}
	s, _ := schedulerFor(t, e)
	j := submit(t, s, testRequest("a", "a"))
	select {
	case <-e.started:
	case <-time.After(10 * time.Second):
		t.Fatal("no start")
	}
	if _, err := s.Cancel(context.Background(), j.ID); err != nil {
		t.Fatal(err)
	}
	close(e.release)
	if got := waitJob(t, s, j.ID); got.Status != Completed {
		t.Fatalf("publication lost: %+v", got)
	}
	got, err := s.Cancel(context.Background(), j.ID)
	if err != nil || got.Status != Completed {
		t.Fatalf("completed cancellation: %+v %v", got, err)
	}
}
func TestRestartRequiresExplicitRecovery(t *testing.T) {
	store, path := openTestStore(t)
	j, err := store.Submit(context.Background(), testRequest("a", "a"))
	if err != nil {
		t.Fatal(err)
	}
	if err = store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer closeStore(t, store)
	if _, err = NewScheduler(store, newExecutor()); !errors.Is(err, ErrRecoveryRequired) {
		t.Fatalf("auto replay: %v", err)
	}
	if err = store.Interrupt(context.Background(), j.ID); err != nil {
		t.Fatal(err)
	}
	s, err := NewScheduler(store, newExecutor())
	if err != nil {
		t.Fatal(err)
	}
	if err = s.Close(); err != nil {
		t.Fatal(err)
	}
}
func TestCloseJoinsAndInterrupts(t *testing.T) {
	e := newExecutor()
	s, store := schedulerFor(t, e)
	j := submit(t, s, testRequest("a", "a"))
	receive(t, e.started)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	got, err := store.Get(context.Background(), j.ID)
	if err != nil || got.Status != Interrupted {
		t.Fatalf("closed scheduler: %+v %v", got, err)
	}
}

func TestOversizedAdmissionAndExecutorMetadata(t *testing.T) {
	e := newExecutor()
	s, _ := schedulerFor(t, e)
	req := testRequest("oversized", "state")
	req.Operation.Parameters = []byte(`{"padding":"` + strings.Repeat("x", maxFrameSize-1000) + `"}`)
	if _, err := s.Submit(context.Background(), req); err == nil {
		t.Fatal("request without lifecycle headroom accepted")
	}
	j := submit(t, s, testRequest("normal", "state"))
	call := receive(t, e.started)
	if err := s.ReportProgress(context.Background(), j.ID, ProgressEvent{Message: strings.Repeat("x", maxFrameSize)}); err == nil {
		t.Fatal("oversized progress accepted")
	}
	call.failure <- errors.New(strings.Repeat("x", maxFrameSize))
	got := waitJob(t, s, j.ID)
	if got.Status != Failed || got.Error == nil || len(got.Error.Message) > 4096 {
		t.Fatalf("failed to persist bounded terminal error: %s", got.Status)
	}
	submit(t, s, testRequest("next", "state"))
}
