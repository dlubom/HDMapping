package jobs

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func testRequest(key, state string) SubmitRequest {
	return SubmitRequest{RequestKey: key, Source: SourceMCP, Access: AccessMutate,
		Operation: OperationRequest{Operation: "test.operation", Target: TargetRef{StateID: state, Revision: 1}, Scope: []byte(`{}`), Parameters: []byte(`{"n":1}`)},
		Snapshot:  &SnapshotRef{ManifestPath: "snapshot.json", Target: TargetRef{StateID: state, Revision: 1}}}
}
func openTestStore(t *testing.T) (*Store, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "rejestr żółć.jobs")
	s, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Error(err)
		}
	})
	return s, path
}
func TestRetryAndCrashRecovery(t *testing.T) {
	ctx := context.Background()
	s, path := openTestStore(t)
	req := testRequest("one", "state")
	first, err := s.Submit(ctx, req)
	if err != nil {
		t.Fatal(err)
	}
	req.Operation.Parameters = []byte(`{ "n" : 1 }`)
	retry, err := s.Submit(ctx, req)
	if err != nil || retry.ID != first.ID {
		t.Fatalf("retry: %+v %v", retry, err)
	}
	req.Operation.Parameters = []byte(`{"n":2}`)
	if _, err = s.Submit(ctx, req); !errors.Is(err, ErrRequestConflict) {
		t.Fatalf("changed arguments: %v", err)
	}
	if err = s.Close(); err != nil {
		t.Fatal(err)
	}
	s, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer closeStore(t, s)
	got, err := s.Get(ctx, first.ID)
	if err != nil || got.Status != Queued {
		t.Fatalf("recovered: %+v %v", got, err)
	}
	if _, err = s.Submit(ctx, testRequest("two", "state")); !errors.Is(err, ErrReserved) {
		t.Fatalf("lost reservation: %v", err)
	}
	if err = s.Interrupt(ctx, first.ID); err != nil {
		t.Fatal(err)
	}
	req = testRequest("one", "state")
	retry, err = s.Submit(ctx, req)
	if err != nil || retry.ID != first.ID || retry.Status != Interrupted {
		t.Fatalf("recovered retry: %+v %v", retry, err)
	}
	if _, err = s.Submit(ctx, testRequest("new", "state")); err != nil {
		t.Fatal(err)
	}
}
func closeStore(t *testing.T, s *Store) {
	t.Helper()
	if err := s.Close(); err != nil {
		t.Error(err)
	}
}

func TestJournalLockAndMissingDirectory(t *testing.T) {
	s, path := openTestStore(t)
	if other, err := OpenStore(path); err == nil {
		closeStore(t, other)
		t.Fatal("second writer admitted")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	reopened, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	closeStore(t, reopened)
	if _, err := OpenStore(filepath.Join(t.TempDir(), "missing", "jobs")); err == nil {
		t.Fatal("missing parent accepted")
	}
}

type failingJournal struct {
	journalFile
	remaining int
	syncError bool
}

func (f *failingJournal) Write(p []byte) (int, error) {
	if f.remaining < 0 {
		return f.journalFile.Write(p)
	}
	if len(p) > f.remaining {
		p = p[:f.remaining]
	}
	n, err := f.journalFile.Write(p)
	f.remaining -= n
	if err != nil {
		return n, err
	}
	return n, io.ErrShortWrite
}
func (f *failingJournal) Sync() error {
	if f.syncError {
		return errors.New("injected sync failure")
	}
	return f.journalFile.Sync()
}

func TestInterruptedCommitNeverCompletes(t *testing.T) {
	ctx := context.Background()
	for _, cut := range []int{0, 1, 4, 32, 80} {
		t.Run(string(rune('a'+cut)), func(t *testing.T) {
			s, path := openTestStore(t)
			j, err := s.Submit(ctx, testRequest("one", "state"))
			if err != nil {
				t.Fatal(err)
			}
			j.Status = Running
			if err = s.update(ctx, j); err != nil {
				t.Fatal(err)
			}
			s.file = &failingJournal{journalFile: s.file, remaining: cut}
			j.Status = Completed
			j.Result = &Publication{Confirmed: true, OutputDirectory: "result", ManifestPath: "result/manifest.json"}
			if err = s.update(ctx, j); err == nil {
				t.Fatal("short write accepted")
			}
			got, err := s.Get(ctx, j.ID)
			if err != nil || got.Status != Running {
				t.Fatalf("failed commit visible: %+v %v", got, err)
			}
			if _, err = s.Submit(ctx, testRequest("another", "other")); !errors.Is(err, ErrStoreUncertain) {
				t.Fatalf("poisoned writer continued: %v", err)
			}
			if err = s.Close(); err != nil {
				t.Fatal(err)
			}
			s, err = OpenStore(path)
			if err != nil {
				t.Fatal(err)
			}
			defer closeStore(t, s)
			got, err = s.Get(ctx, j.ID)
			if err != nil || got.Status != Running {
				t.Fatalf("partial commit recovered as success: %+v %v", got, err)
			}
			if _, err = s.Submit(ctx, testRequest("conflict", "state")); !errors.Is(err, ErrReserved) {
				t.Fatalf("reservation after torn write: %v", err)
			}
		})
	}
}
func TestSyncFailureAndPublicationGuard(t *testing.T) {
	ctx := context.Background()
	s, _ := openTestStore(t)
	j, err := s.Submit(ctx, testRequest("one", "state"))
	if err != nil {
		t.Fatal(err)
	}
	j.Status = Running
	if err = s.update(ctx, j); err != nil {
		t.Fatal(err)
	}
	j.Status = Completed
	if err = s.update(ctx, j); err == nil {
		t.Fatal("completed without publication")
	}
	j.Result = &Publication{Confirmed: true, OutputDirectory: "result", ManifestPath: "manifest"}
	s.file = &failingJournal{journalFile: s.file, remaining: -1, syncError: true}
	if err = s.update(ctx, j); err == nil {
		t.Fatal("sync error hidden")
	}
	got, err := s.Get(ctx, j.ID)
	if err != nil || got.Status != Running {
		t.Fatalf("unacknowledged completion: %+v %v", got, err)
	}
}
func TestJournalCorruptionAndTruncation(t *testing.T) {
	s, path := openTestStore(t)
	if _, err := s.Submit(context.Background(), testRequest("one", "state")); err != nil {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for n := 0; n < len(data); n++ {
		if _, _, err := decodeJournal(bytes.NewReader(data[:n])); err != nil {
			t.Fatalf("torn prefix %d: %v", n, err)
		}
	}
	data[len(data)-1] ^= 1
	if err = os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err = OpenStore(path); err == nil {
		t.Fatal("checksum corruption accepted")
	}
}
func TestListCopiesAndPagination(t *testing.T) {
	ctx := context.Background()
	s, _ := openTestStore(t)
	a, err := s.Submit(ctx, testRequest("a", "a"))
	if err != nil {
		t.Fatal(err)
	}
	b, err := s.Submit(ctx, testRequest("b", "b"))
	if err != nil {
		t.Fatal(err)
	}
	a.Request.Operation.Parameters[0] = 'x'
	a.Request.Snapshot.ManifestPath = "changed"
	got, err := s.Get(ctx, a.ID)
	if err != nil || !bytes.Equal(got.Request.Operation.Parameters, []byte(`{"n":1}`)) || got.Snapshot.ManifestPath != "snapshot.json" {
		t.Fatalf("aliased record: %+v %v", got, err)
	}
	page, err := s.List(ctx, JobFilter{Limit: 1})
	if err != nil || len(page) != 1 || page[0].ID != a.ID {
		t.Fatalf("page: %+v %v", page, err)
	}
	page, err = s.List(ctx, JobFilter{After: a.ID, Limit: 1})
	if err != nil || len(page) != 1 || page[0].ID != b.ID {
		t.Fatalf("page 2: %+v %v", page, err)
	}
	page, err = s.List(ctx, JobFilter{StateID: "a", Status: Queued})
	if err != nil || len(page) != 1 {
		t.Fatalf("filter: %+v %v", page, err)
	}
}
func FuzzDecodeJournal(f *testing.F) {
	f.Add([]byte{})
	f.Add([]byte{0, 0, 0, 1})
	f.Add([]byte("broken journal"))
	f.Add([]byte{255, 255, 255, 255})
	// These payloads run as ordinary regression seeds too. Recompute framing in
	// the harness so mutations can exercise JSON and transition validation rather
	// than being stopped exclusively at the checksum boundary.
	for _, payload := range []string{
		`{"version":1,"sequence":1,"limit":2}`,
		`{"version":2,"sequence":1,"limit":2}`,
		`{"version":1,"sequence":1,"limit":0}`,
		`{"version":1,"sequence":1,"limit":2,"unknown":true}`,
		`{"version":1,"sequence":1,"job":null}`,
	} {
		f.Add([]byte(payload))
	}
	j := JobRecord{ID: "seed", Request: testRequest("seed", "state"), Status: Queued, GUIApplyStatus: "not_requested"}
	j.Snapshot = j.Request.Snapshot
	payload, err := json.Marshal(journalEntry{Version: 1, Sequence: 1, Job: &j})
	if err != nil {
		f.Fatal(err)
	}
	f.Add(payload)
	f.Fuzz(func(t *testing.T, data []byte) {
		if len(data) > 4*maxFrameSize {
			t.Skip()
		}
		_, _, _ = decodeJournal(bytes.NewReader(data))
		if len(data) <= maxFrameSize {
			_, _, _ = decodeJournal(bytes.NewReader(testFrame(data)))
		}
	})
}

func testFrame(payload []byte) []byte {
	frame := make([]byte, 40+len(payload))
	n := uint32(len(payload))
	binary.BigEndian.PutUint32(frame, n)
	binary.BigEndian.PutUint32(frame[4:], ^n)
	sum := sha256.Sum256(payload)
	copy(frame[8:], sum[:])
	copy(frame[40:], payload)
	return frame
}
