package jobs

import (
	"os"
	"runtime"

	"golang.org/x/sys/windows"
)

func lockJournal(f *os.File) error {
	err := windows.LockFileEx(windows.Handle(f.Fd()),
		windows.LOCKFILE_EXCLUSIVE_LOCK|windows.LOCKFILE_FAIL_IMMEDIATELY,
		0, ^uint32(0), ^uint32(0), &windows.Overlapped{})
	runtime.KeepAlive(f)
	return err
}

func syncJournalFile(f *os.File) error {
	// File.Sync calls FlushFileBuffers, which flushes data and metadata.
	return f.Sync()
}

func syncJournalDirectory(f *os.File) error {
	// The journal has a stable name in an existing directory. Windows flushes
	// its creation metadata through the file handle; no rename is involved.
	return syncJournalFile(f)
}
