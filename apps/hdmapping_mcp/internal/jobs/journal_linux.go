package jobs

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"

	"golang.org/x/sys/unix"
)

func lockJournal(f *os.File) error {
	for {
		err := unix.Flock(int(f.Fd()), unix.LOCK_EX|unix.LOCK_NB)
		runtime.KeepAlive(f)
		if !errors.Is(err, unix.EINTR) {
			return err
		}
	}
}

func syncJournalFile(f *os.File) error {
	return f.Sync()
}

func syncJournalDirectory(f *os.File) error {
	dir, err := os.Open(filepath.Dir(f.Name()))
	if err != nil {
		return err
	}
	// fsync(file) alone does not persist a newly created directory entry.
	return errors.Join(dir.Sync(), dir.Close())
}
