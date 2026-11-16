package jobs

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"unicode/utf8"
)

// Scheduler owns all executor goroutines and one Store. Only use its admission
// methods while attached. Close stops and joins executors; closing a client
// context does not stop them. Failed durable writes halt further scheduling.
type Scheduler struct {
	mu       sync.Mutex
	store    *Store
	executor Executor
	active   map[JobID]context.CancelFunc
	limit    int
	closing  bool
	done     chan struct{}
	changed  chan struct{}
	wg       sync.WaitGroup
	err      error
}

func NewScheduler(store *Store, executor Executor) (*Scheduler, error) {
	if executor == nil {
		return nil, errors.New("executor required")
	}
	store.mu.Lock()
	defer store.mu.Unlock()
	if err := store.check(context.Background(), true); err != nil {
		return nil, err
	}
	if store.attached {
		return nil, errors.New("store already has scheduler")
	}
	for _, j := range store.state.jobs {
		if !j.Status.terminal() {
			return nil, ErrRecoveryRequired
		}
	}
	store.attached = true
	return &Scheduler{store: store, executor: executor, active: make(map[JobID]context.CancelFunc), limit: store.state.limit, done: make(chan struct{}), changed: make(chan struct{})}, nil
}
func (s *Scheduler) notify() { close(s.changed); s.changed = make(chan struct{}) }
func (s *Scheduler) check() error {
	if s.closing {
		return ErrClosed
	}
	return s.err
}
func (s *Scheduler) Submit(ctx context.Context, req SubmitRequest) (JobRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(); err != nil {
		return JobRecord{}, err
	}
	j, err := s.store.Submit(ctx, req)
	if err != nil {
		return JobRecord{}, err
	}
	s.notify()
	s.pump()
	if s.err != nil {
		return j, s.err
	}
	return s.store.Get(ctx, j.ID)
}
func (s *Scheduler) Get(ctx context.Context, id JobID) (JobRecord, error) {
	return s.store.Get(ctx, id)
}
func (s *Scheduler) List(ctx context.Context, filter JobFilter) ([]JobRecord, error) {
	return s.store.List(ctx, filter)
}
func (s *Scheduler) SetLimit(ctx context.Context, limit int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(); err != nil {
		return err
	}
	if err := s.store.setLimit(ctx, limit); err != nil {
		return err
	}
	s.limit = limit
	s.notify()
	s.pump()
	return s.err
}
func (s *Scheduler) Cancel(ctx context.Context, id JobID) (JobRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(); err != nil {
		return JobRecord{}, err
	}
	j, err := s.store.Get(ctx, id)
	if err != nil {
		return JobRecord{}, err
	}
	if j.Status.terminal() {
		return j, nil
	}
	cancel, active := s.active[id]
	if active {
		j.Status = Cancelling
	} else {
		j.Status = Cancelled
	}
	if err = s.store.update(ctx, j); err != nil {
		return JobRecord{}, err
	}
	if active {
		cancel()
	}
	s.notify()
	s.pump()
	return clone(j), s.err
}

// ReportProgress records observable engine progress without inferring percentages.
func (s *Scheduler) ReportProgress(ctx context.Context, id JobID, p ProgressEvent) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(); err != nil {
		return err
	}
	if _, ok := s.active[id]; !ok {
		return errors.New("progress requires active executor")
	}
	j, err := s.store.Get(ctx, id)
	if err != nil {
		return err
	}
	j.Progress = p
	if err = s.store.update(ctx, j); err != nil {
		return err
	}
	s.notify()
	return nil
}

// Wait observes terminal state without polling. Its context only cancels waiting.
func (s *Scheduler) Wait(ctx context.Context, id JobID) (JobRecord, error) {
	for {
		s.mu.Lock()
		j, err := s.store.Get(ctx, id)
		if err == nil && !j.Status.terminal() {
			err = s.err
		}
		if err != nil || j.Status.terminal() {
			s.mu.Unlock()
			return j, err
		}
		changed := s.changed
		s.mu.Unlock()
		select {
		case <-ctx.Done():
			return JobRecord{}, ctx.Err()
		case <-changed:
		}
	}
}
func (s *Scheduler) save(j JobRecord) bool {
	if err := s.store.update(context.Background(), j); err != nil {
		s.err = errors.Join(s.err, err)
		s.notify()
		return false
	}
	s.notify()
	return true
}
func (s *Scheduler) pump() {
	if s.closing || s.err != nil {
		return
	}
	// Copy IDs under the store lock; no caller-owned mutable records escape.
	s.store.mu.Lock()
	ids := append([]JobID(nil), s.store.state.order...)
	s.store.mu.Unlock()
	for _, id := range ids {
		if len(s.active) >= s.limit {
			return
		}
		if _, ok := s.active[id]; ok {
			continue
		}
		j, err := s.store.Get(context.Background(), id)
		if err != nil {
			s.err = err
			s.notify()
			return
		}
		if j.Status != Preparing && j.Status != Queued {
			continue
		}
		if j.Status == Queued {
			j.Status = Running
			if !s.save(j) {
				return
			}
		}
		ctx, cancel := context.WithCancel(context.Background())
		s.active[id] = cancel
		s.wg.Add(1)
		go s.execute(ctx, clone(j))
	}
}
func (s *Scheduler) execute(ctx context.Context, j JobRecord) {
	defer s.wg.Done()
	if j.Status == Preparing {
		snapshot, err := s.executor.Prepare(ctx, clone(j))
		s.mu.Lock()
		current, getErr := s.store.Get(context.Background(), j.ID)
		if getErr != nil {
			s.err = errors.Join(s.err, getErr)
			s.release(j.ID)
			s.mu.Unlock()
			return
		}
		if err != nil || current.Status == Cancelling || s.closing {
			s.finish(current, Publication{}, err)
			s.mu.Unlock()
			return
		}
		if !validSnapshot(&snapshot, j.Request.Operation.Target) {
			s.finish(current, Publication{}, errors.New("executor returned invalid snapshot"))
			s.mu.Unlock()
			return
		}
		current.Snapshot = &snapshot
		current.Status = Queued
		if !s.save(current) {
			s.release(j.ID)
			s.mu.Unlock()
			return
		}
		current.Status = Running
		if !s.save(current) {
			s.release(j.ID)
			s.mu.Unlock()
			return
		}
		j = current
		s.mu.Unlock()
	}
	result, err := s.executor.Run(ctx, clone(j))
	s.mu.Lock()
	defer s.mu.Unlock()
	current, getErr := s.store.Get(context.Background(), j.ID)
	if getErr != nil {
		s.err = errors.Join(s.err, getErr)
		s.release(j.ID)
		return
	}
	s.finish(current, result, err)
}
func (s *Scheduler) finish(j JobRecord, result Publication, runErr error) {
	switch {
	case validPublication(result):
		j.Status = Completed
		j.Result = &result
		j.Error = nil
	case s.closing:
		j.Status = Interrupted
		j.Error = &JobError{Code: "interrupted", Message: "scheduler closed before publication"}
	case j.Status == Cancelling:
		j.Status = Cancelled
	default:
		j.Status = Failed
		if runErr == nil {
			runErr = errors.New("executor returned without confirmed publication")
		}
		message := strings.ToValidUTF8(runErr.Error(), "�")
		if len(message) > 4096 {
			message = message[:4096]
			for !utf8.ValidString(message) {
				message = message[:len(message)-1]
			}
		}
		j.Error = &JobError{Code: "execution_failed", Message: message}
	}
	s.save(j)
	s.release(j.ID)
	s.pump()
}
func (s *Scheduler) release(id JobID) {
	if cancel, ok := s.active[id]; ok {
		cancel()
		delete(s.active, id)
	}
	s.notify()
}

// Close waits for executors to return. An executor that ignores cancellation is a
// contract violation; process enforcement belongs to the P03 supervisor.
func (s *Scheduler) Close() error {
	s.mu.Lock()
	if s.closing {
		done := s.done
		s.mu.Unlock()
		<-done
		s.mu.Lock()
		defer s.mu.Unlock()
		return s.err
	}
	s.closing = true
	for _, cancel := range s.active {
		cancel()
	}
	s.notify()
	s.mu.Unlock()
	s.wg.Wait()
	s.mu.Lock()
	defer s.mu.Unlock()
	s.store.mu.Lock()
	if s.store.uncertain == nil {
		for _, id := range s.store.state.order {
			j := s.store.state.jobs[id]
			if !j.Status.terminal() {
				j.Status = Interrupted
				j.Error = &JobError{Code: "interrupted", Message: "scheduler closed before execution"}
				if err := s.store.commit(journalEntry{Job: &j}); err != nil {
					s.err = errors.Join(s.err, fmt.Errorf("close: %w", err))
					break
				}
			}
		}
	}
	s.store.attached = false
	s.store.mu.Unlock()
	s.notify()
	close(s.done)
	return s.err
}
