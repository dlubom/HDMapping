package jobs

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"unicode/utf8"
)

const maxFrameSize = 1 << 20

// Admission leaves room for every subsequent lifecycle transition. Dynamic
// metadata is bounded separately, including executor-supplied error messages.
const maxRequestSize = maxFrameSize / 4
const maxPathSize = 32768
const maxProgressSize = 65536

func validSnapshot(s *SnapshotRef, target TargetRef) bool {
	return s != nil && s.ManifestPath != "" && len(s.ManifestPath) <= maxPathSize && s.Target == target
}

func validPublication(p Publication) bool {
	return p.Confirmed && p.OutputDirectory != "" && len(p.OutputDirectory) <= maxPathSize && p.ManifestPath != "" && len(p.ManifestPath) <= maxPathSize &&
		(p.Snapshot == nil || (p.Snapshot.Target.StateID != "" && filepath.IsAbs(p.Snapshot.ManifestPath) && len(p.Snapshot.ManifestPath) <= maxPathSize))
}

type journalFile interface {
	io.Reader
	io.Writer
	io.Seeker
	Truncate(int64) error
	Sync() error
	Close() error
}
type durableJournal struct{ *os.File }

func (f durableJournal) Sync() error { return syncJournalFile(f.File) }

type journalEntry struct {
	Version  int        `json:"version"`
	Sequence uint64     `json:"sequence"`
	Job      *JobRecord `json:"job,omitempty"`
	Limit    *int       `json:"limit,omitempty"`
}
type registry struct {
	jobs         map[JobID]JobRecord
	order        []JobID
	keys         map[string]JobID
	reservations map[string]JobID
	limit        int
	sequence     uint64
}

func newRegistry() *registry {
	return &registry{jobs: make(map[JobID]JobRecord), keys: make(map[string]JobID), reservations: make(map[string]JobID), limit: DefaultLimit}
}

// Store has one exclusive OS file lock, held until Close. The parent directory
// must already exist durably. A write/sync error poisons mutations until reopen;
// Get/List still expose the last acknowledged state. Never unlink a live journal.
type Store struct {
	admission sync.Mutex
	mu        sync.Mutex
	file      journalFile
	state     *registry
	closed    bool
	uncertain error
	attached  bool
}

func OpenStore(path string) (*Store, error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, fmt.Errorf("open job journal: %w", err)
	}
	fail := func(cause error) (*Store, error) { return nil, errors.Join(cause, f.Close()) }
	if err = lockJournal(f); err != nil {
		return fail(fmt.Errorf("lock job journal: %w", err))
	}
	if err = syncJournalFile(f); err != nil {
		return fail(fmt.Errorf("sync job journal: %w", err))
	}
	if err = syncJournalDirectory(f); err != nil {
		return fail(fmt.Errorf("sync journal directory: %w", err))
	}
	state, valid, err := decodeJournal(f)
	if err != nil {
		return fail(fmt.Errorf("recover job journal: %w", err))
	}
	info, err := f.Stat()
	if err != nil {
		return fail(err)
	}
	if valid != info.Size() {
		if err = f.Truncate(valid); err != nil {
			return fail(fmt.Errorf("truncate incomplete journal tail: %w", err))
		}
		if err = syncJournalFile(f); err != nil {
			return fail(fmt.Errorf("sync repaired journal: %w", err))
		}
	}
	if _, err = f.Seek(0, io.SeekEnd); err != nil {
		return fail(err)
	}
	return &Store{file: durableJournal{f}, state: state}, nil
}
func (s *Store) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil
	}
	if s.attached {
		return errors.New("close scheduler before store")
	}
	s.closed = true
	return s.file.Close()
}
func (s *Store) check(ctx context.Context, write bool) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if s.closed {
		return ErrClosed
	}
	if write && s.uncertain != nil {
		return errors.Join(ErrStoreUncertain, s.uncertain)
	}
	return nil
}
func clone(j JobRecord) JobRecord {
	j.Request.Operation.Scope = bytes.Clone(j.Request.Operation.Scope)
	j.Request.Operation.Parameters = bytes.Clone(j.Request.Operation.Parameters)
	if j.Request.Snapshot != nil {
		v := *j.Request.Snapshot
		j.Request.Snapshot = &v
	}
	if j.Request.Recording != nil {
		v := *j.Request.Recording
		j.Request.Recording = &v
	}
	if j.Snapshot != nil {
		v := *j.Snapshot
		j.Snapshot = &v
	}
	if j.Result != nil {
		v := *j.Result
		if v.Snapshot != nil {
			snapshot := *v.Snapshot
			v.Snapshot = &snapshot
		}
		j.Result = &v
	}
	if j.Error != nil {
		v := *j.Error
		v.Details = bytes.Clone(v.Details)
		j.Error = &v
	}
	if j.Progress.Current != nil {
		v := *j.Progress.Current
		j.Progress.Current = &v
	}
	if j.Progress.Total != nil {
		v := *j.Progress.Total
		j.Progress.Total = &v
	}
	return j
}
func canonicalObject(raw json.RawMessage) (json.RawMessage, error) {
	if len(raw) == 0 {
		raw = []byte(`{}`)
	}
	if len(raw) > maxFrameSize {
		return nil, errors.New("arguments exceed journal limit")
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	d.UseNumber()
	var v map[string]any
	if err := d.Decode(&v); err != nil {
		return nil, err
	}
	if v == nil {
		return nil, errors.New("expected JSON object")
	}
	if err := d.Decode(new(any)); err != io.EOF {
		return nil, errors.New("trailing JSON input")
	}
	return json.Marshal(v)
}
func normalize(req SubmitRequest) (SubmitRequest, error) {
	if req.RequestKey == "" || len(req.RequestKey) > 256 || req.Operation.Operation == "" || req.Operation.Target.StateID == "" || len(req.Operation.Target.StateID) > 256 {
		return req, errors.New("missing or oversized request identity")
	}
	// JSON replaces invalid UTF-8, which would change these map keys on replay.
	if !utf8.ValidString(req.RequestKey) || !utf8.ValidString(req.Operation.Target.StateID) {
		return req, errors.New("request_key and state_id must be valid UTF-8")
	}
	if req.Source != SourceGUI && req.Source != SourceMCP {
		return req, errors.New("invalid job source")
	}
	if req.Access != AccessRead && req.Access != AccessMutate {
		return req, errors.New("invalid job access")
	}
	switch {
	case req.Recording != nil:
		if req.Snapshot != nil || req.CaptureSource != "" {
			return req, errors.New("recording requires an exclusive input source")
		}
		v := *req.Recording
		if !strings.HasPrefix(req.Operation.Operation, "step1.") || !filepath.IsAbs(v.Directory) || (v.TOML != "" && !filepath.IsAbs(v.TOML)) {
			return req, errors.New("raw recording requires a Step 1 operation and absolute input paths")
		}
		req.Recording = &v
	case req.Snapshot != nil:
		if req.CaptureSource != "" || !validSnapshot(req.Snapshot, req.Operation.Target) {
			return req, errors.New("invalid snapshot reference")
		}
		v := *req.Snapshot
		req.Snapshot = &v
	case req.CaptureSource == "":
		return req, errors.New("snapshot, capture source or recording required")
	}
	var err error
	req.Operation.Scope, err = canonicalObject(req.Operation.Scope)
	if err != nil {
		return req, fmt.Errorf("scope: %w", err)
	}
	req.Operation.Parameters, err = canonicalObject(req.Operation.Parameters)
	if err != nil {
		return req, fmt.Errorf("parameters: %w", err)
	}
	encoded, err := json.Marshal(req)
	if err != nil {
		return req, fmt.Errorf("encode request: %w", err)
	}
	if len(encoded) > maxRequestSize {
		return req, errors.New("request exceeds admission limit")
	}
	return req, nil
}
func sameRequest(a, b SubmitRequest) bool {
	x, _ := json.Marshal(a)
	y, _ := json.Marshal(b)
	return bytes.Equal(x, y)
}
func (s *Store) Submit(ctx context.Context, req SubmitRequest) (JobRecord, error) {
	return s.submit(ctx, req, false, nil)
}

func (s *Store) submit(ctx context.Context, req SubmitRequest, prepare bool, reserve func(JobRecord) error) (JobRecord, error) {
	// Serialize admission by request key across the GUI pin handshake. Other
	// store operations (including progress, cancellation and lookup) stay live.
	s.admission.Lock()
	defer s.admission.Unlock()
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(ctx, true); err != nil {
		return JobRecord{}, err
	}
	req, err := normalize(req)
	if err != nil {
		return JobRecord{}, fmt.Errorf("submit: %w", err)
	}
	if id, ok := s.state.keys[req.RequestKey]; ok {
		j := s.state.jobs[id]
		if !sameRequest(req, j.Request) {
			return JobRecord{}, ErrRequestConflict
		}
		return clone(j), nil
	}
	if req.Access == AccessMutate && s.state.reservations[req.Operation.Target.StateID] != "" {
		return JobRecord{}, ErrReserved
	}
	pending := 0
	for _, j := range s.state.jobs {
		if !j.Status.terminal() {
			pending++
		}
	}
	if pending >= MaxPending {
		return JobRecord{}, ErrQueueFull
	}
	var id [16]byte
	if _, err = rand.Read(id[:]); err != nil {
		return JobRecord{}, fmt.Errorf("job ID: %w", err)
	}
	j := JobRecord{ID: JobID(hex.EncodeToString(id[:])), Request: req, Status: Preparing, GUIApplyStatus: "not_requested"}
	if req.Access == AccessMutate && (req.Source == SourceGUI || req.CaptureSource != "") {
		j.GUIApplyStatus = "pending"
	}
	if req.Snapshot != nil && !prepare {
		j.Snapshot = req.Snapshot
		j.Status = Queued
	}
	if reserve != nil {
		s.mu.Unlock()
		err = reserve(j)
		s.mu.Lock()
		if err != nil {
			return JobRecord{}, err
		}
	}
	if err = s.check(ctx, true); err != nil {
		return JobRecord{}, err
	}
	if err = s.commit(journalEntry{Job: &j}); err != nil {
		return JobRecord{}, err
	}
	return clone(j), nil
}
func (s *Store) Get(ctx context.Context, id JobID) (JobRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(ctx, false); err != nil {
		return JobRecord{}, err
	}
	j, ok := s.state.jobs[id]
	if !ok {
		return JobRecord{}, ErrNotFound
	}
	return clone(j), nil
}
func (s *Store) List(ctx context.Context, filter JobFilter) ([]JobRecord, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(ctx, false); err != nil {
		return nil, err
	}
	if filter.Limit < 0 || filter.Limit > 1000 {
		return nil, errors.New("list limit must be 0..1000")
	}
	if filter.Limit == 0 {
		filter.Limit = 100
	}
	if filter.After != "" {
		if _, ok := s.state.jobs[filter.After]; !ok {
			return nil, ErrNotFound
		}
	}
	out := []JobRecord{}
	past := filter.After == ""
	for _, id := range s.state.order {
		if !past {
			past = id == filter.After
			continue
		}
		j := s.state.jobs[id]
		if (filter.StateID == "" || filter.StateID == j.Request.Operation.Target.StateID) && (filter.Status == "" || filter.Status == j.Status) {
			out = append(out, clone(j))
			if len(out) == filter.Limit {
				break
			}
		}
	}
	return out, nil
}
func (s *Store) update(ctx context.Context, j JobRecord) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(ctx, true); err != nil {
		return err
	}
	return s.commit(journalEntry{Job: &j})
}

// Interrupt is a recovery decision, permitted only without an attached scheduler.
// The caller MUST first establish that the former executor cannot still publish or
// mutate this target. P03 supplies process identity and result reconciliation.
func (s *Store) Interrupt(ctx context.Context, id JobID) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(ctx, true); err != nil {
		return err
	}
	if s.attached {
		return errors.New("cannot recover attached scheduler")
	}
	j, ok := s.state.jobs[id]
	if !ok {
		return ErrNotFound
	}
	if j.Status.terminal() {
		return nil
	}
	j.Status = Interrupted
	j.Error = &JobError{Code: "interrupted", Message: "former executor confirmed stopped"}
	return s.commit(journalEntry{Job: &j})
}
func (s *Store) setLimit(ctx context.Context, limit int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(ctx, true); err != nil {
		return err
	}
	return s.commit(journalEntry{Limit: &limit})
}
func (s *Store) commit(e journalEntry) error {
	if j := e.Job; j != nil && j.Status.terminal() && j.Status != Completed && j.GUIApplyStatus == "pending" {
		j.GUIApplyStatus = "not_requested"
	}
	e.Version = 1
	e.Sequence = s.state.sequence + 1
	if err := s.state.validate(e); err != nil {
		return err
	}
	payload, err := json.Marshal(e)
	if err != nil {
		return err
	}
	if len(payload) > maxFrameSize {
		return errors.New("job record exceeds journal limit")
	}
	frame := make([]byte, 40+len(payload))
	n := uint32(len(payload))
	binary.BigEndian.PutUint32(frame, n)
	binary.BigEndian.PutUint32(frame[4:], ^n)
	sum := sha256.Sum256(payload)
	copy(frame[8:40], sum[:])
	copy(frame[40:], payload)
	written, err := s.file.Write(frame)
	if err == nil && written != len(frame) {
		err = io.ErrShortWrite
	}
	if err == nil {
		err = s.file.Sync()
	}
	if err != nil {
		s.uncertain = fmt.Errorf("commit journal sequence %d: %w", e.Sequence, err)
		return errors.Join(ErrStoreUncertain, s.uncertain)
	}
	s.state.apply(e)
	return nil
}
func applyOutcome(status string) bool {
	switch status {
	case "applied", "closed", "stale_revision", "not_applied", "unconfirmed":
		return true
	}
	return false
}
func (r *registry) validate(e journalEntry) error {
	if e.Version != 1 || e.Sequence != r.sequence+1 {
		return errors.New("invalid journal version or sequence")
	}
	if (e.Job == nil) == (e.Limit == nil) {
		return errors.New("journal entry must contain job or limit")
	}
	if e.Limit != nil {
		if *e.Limit < 1 || *e.Limit > MaxLimit {
			return fmt.Errorf("limit must be 1..%d", MaxLimit)
		}
		return nil
	}
	j := e.Job
	if j.ID == "" {
		return errors.New("empty job ID")
	}
	normalized, err := normalize(j.Request)
	if err != nil || !sameRequest(normalized, j.Request) {
		return errors.New("invalid journal request")
	}
	if j.Status != Preparing && j.Status != Queued && j.Status != Running && j.Status != Cancelling && !j.Status.terminal() {
		return errors.New("invalid job status")
	}
	if j.GUIApplyStatus != "pending" && j.GUIApplyStatus != "not_requested" && (j.Status != Completed || !applyOutcome(j.GUIApplyStatus)) {
		return errors.New("invalid GUI apply status")
	}
	if j.Snapshot != nil && !validSnapshot(j.Snapshot, j.Request.Operation.Target) {
		return errors.New("invalid captured snapshot")
	}
	if (j.Status == Queued || j.Status == Running || j.Status == Completed) && j.Snapshot == nil {
		return errors.New("ready job has no snapshot")
	}
	if j.Status == Completed {
		if j.Result == nil || !validPublication(*j.Result) {
			return errors.New("completion requires confirmed publication")
		}
	} else if j.Result != nil {
		return errors.New("publication requires completed status")
	}
	progress, err := json.Marshal(j.Progress)
	if err != nil || len(progress) > maxProgressSize {
		return errors.New("progress exceeds metadata limit")
	}
	if j.Error != nil && (len(j.Error.Code) > 256 || len(j.Error.OperationCode) > 256 ||
		len(j.Error.Message) > 4096 || len(j.Error.Details) > 4096) {
		return errors.New("job error exceeds metadata limit")
	}
	old, exists := r.jobs[j.ID]
	if !exists {
		if j.Status != Preparing && j.Status != Queued {
			return errors.New("invalid initial job status")
		}
		if r.keys[j.Request.RequestKey] != "" {
			return ErrRequestConflict
		}
	} else {
		if !sameRequest(old.Request, j.Request) {
			return ErrRequestConflict
		}
		if old.Status.terminal() {
			// Older journals marked read-only results as pending.
			readOnly := j.Request.Access == AccessRead && j.GUIApplyStatus == "not_requested"
			if old.Status == Completed && old.GUIApplyStatus == "pending" && (readOnly || applyOutcome(j.GUIApplyStatus)) {
				old.GUIApplyStatus = j.GUIApplyStatus
				if reflect.DeepEqual(old, *j) {
					return nil
				}
			}
			return errors.New("terminal job is immutable")
		}
		allowed := j.Status == old.Status || j.Status == Interrupted ||
			old.Status == Preparing && (j.Status == Queued || j.Status == Failed || j.Status == Cancelling || j.Status == Cancelled) ||
			old.Status == Queued && (j.Status == Running || j.Status == Cancelled) ||
			old.Status == Running && (j.Status == Completed || j.Status == Failed || j.Status == Cancelling) ||
			old.Status == Cancelling && (j.Status == Cancelled || j.Status == Completed || j.Status == Failed)
		if !allowed {
			return fmt.Errorf("invalid transition %s -> %s", old.Status, j.Status)
		}
		if old.Snapshot != nil && (j.Snapshot == nil || *old.Snapshot != *j.Snapshot) {
			return errors.New("captured snapshot is immutable")
		}
	}
	if !j.Status.terminal() && j.Request.Access == AccessMutate {
		owner := r.reservations[j.Request.Operation.Target.StateID]
		if owner != "" && owner != j.ID {
			return ErrReserved
		}
	}
	return nil
}
func (r *registry) apply(e journalEntry) {
	r.sequence = e.Sequence
	if e.Limit != nil {
		r.limit = *e.Limit
		return
	}
	j := clone(*e.Job)
	if _, exists := r.jobs[j.ID]; !exists {
		r.order = append(r.order, j.ID)
		r.keys[j.Request.RequestKey] = j.ID
	}
	r.jobs[j.ID] = j
	if j.Request.Access == AccessMutate {
		state := j.Request.Operation.Target.StateID
		if j.Status.terminal() {
			if r.reservations[state] == j.ID {
				delete(r.reservations, state)
			}
		} else {
			r.reservations[state] = j.ID
		}
	}
}

// decodeJournal accepts only a fully verified prefix. A short final frame is a
// torn append, while corruption of any complete frame fails closed. Allocation
// is bounded per frame before reading JSON; replay validates transitions too.
func decodeJournal(reader io.Reader) (*registry, int64, error) {
	r := newRegistry()
	var valid int64
	for {
		var header [40]byte
		_, err := io.ReadFull(reader, header[:])
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			return r, valid, nil
		}
		if err != nil {
			return nil, valid, err
		}
		n := binary.BigEndian.Uint32(header[:4])
		if n == 0 || n > maxFrameSize || binary.BigEndian.Uint32(header[4:8]) != ^n {
			return nil, valid, errors.New("invalid journal frame length")
		}
		payload := make([]byte, int(n))
		if _, err = io.ReadFull(reader, payload); err == io.EOF || err == io.ErrUnexpectedEOF {
			return r, valid, nil
		}
		if err != nil {
			return nil, valid, err
		}
		sum := sha256.Sum256(payload)
		if !bytes.Equal(header[8:], sum[:]) {
			return nil, valid, errors.New("journal checksum mismatch")
		}
		var e journalEntry
		d := json.NewDecoder(bytes.NewReader(payload))
		d.DisallowUnknownFields()
		if err = d.Decode(&e); err != nil {
			return nil, valid, fmt.Errorf("decode journal entry: %w", err)
		}
		if err = d.Decode(new(any)); err != io.EOF {
			return nil, valid, errors.New("trailing journal JSON")
		}
		if err = r.validate(e); err != nil {
			return nil, valid, err
		}
		r.apply(e)
		valid += int64(len(header)) + int64(n)
	}
}

// RecordGUIApply updates only the separate application outcome after publication.
func (s *Store) RecordGUIApply(ctx context.Context, id JobID, status string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.check(ctx, true); err != nil {
		return err
	}
	j, ok := s.state.jobs[id]
	if !ok {
		return ErrNotFound
	}
	j.GUIApplyStatus = status
	return s.commit(journalEntry{Job: &j})
}
