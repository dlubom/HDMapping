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
	// Unlike fsync, F_FULLFSYNC also requests a device-cache flush. Do not
	// silently weaken this guarantee on filesystems that return ENOTSUP.
	for {
		_, err := unix.FcntlInt(f.Fd(), unix.F_FULLFSYNC, 0)
		runtime.KeepAlive(f)
		if !errors.Is(err, unix.EINTR) {
			return err
		}
	}
}

func syncJournalDirectory(f *os.File) error {
	dir, err := os.Open(filepath.Dir(f.Name()))
	if err != nil {
		return err
	}
	for {
		err = unix.Fsync(int(dir.Fd()))
		if !errors.Is(err, unix.EINTR) {
			break
		}
	}
	if err = errors.Join(err, dir.Close()); err != nil {
		return err
	}
	// Flush the directory entry before issuing the device-cache barrier.
	return syncJournalFile(f)
}
