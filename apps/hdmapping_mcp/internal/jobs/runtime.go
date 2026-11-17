package jobs

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"runtime"
)

var ErrRuntimeLeaseBusy = errors.New("worker still holds its runtime lease")

// SyncRuntimeReceipt makes the receipt and its directory entry durable with
// the same platform barriers as the journal.
func SyncRuntimeReceipt(file *os.File) error {
	if err := syncJournalFile(file); err != nil {
		return err
	}
	if runtime.GOOS != "windows" {
		// The job directory is new. Persist its entry before confirming
		// the receipt inside it.
		parent, err := os.Open(filepath.Dir(filepath.Dir(file.Name())))
		if err != nil {
			return err
		}
		if err := errors.Join(parent.Sync(), parent.Close()); err != nil {
			return err
		}
	}
	return syncJournalDirectory(file)
}

// RecoverPublication records a verified receipt after the former worker has
// stopped. The caller must hold its runtime lease during reconciliation.
func (s *Store) RecoverPublication(ctx context.Context, id JobID, result Publication) error {
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
	if (j.Status != Running && j.Status != Cancelling) || !validPublication(result) {
		return errors.New("recovery requires a running job and verified publication")
	}
	j.Status = Completed
	j.Result = &result
	j.Error = nil
	return s.commit(journalEntry{Job: &j})
}
