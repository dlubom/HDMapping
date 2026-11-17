//go:build darwin || linux

package jobs

import (
	"errors"
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

// LockRuntimeLease uses an open-file-description lock. The keeper passes the
// file as worker stderr so the child retains the lock if the keeper dies.
func LockRuntimeLease(path string) (*os.File, error) {
	file, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_APPEND, 0600)
	if err != nil {
		return nil, err
	}
	if err := lockJournal(file); err != nil {
		if errors.Is(err, unix.EWOULDBLOCK) {
			err = errors.Join(ErrRuntimeLeaseBusy, err)
		}
		return nil, errors.Join(fmt.Errorf("lock worker lease: %w", err), file.Close())
	}
	return file, nil
}
