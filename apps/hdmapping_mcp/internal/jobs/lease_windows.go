package jobs

import (
	"errors"
	"os"

	"golang.org/x/sys/windows"
)

// Hold the lease with a share restriction that survives inheritance, so the
// lock remains held by worker stderr even on abrupt keeper termination.
func LockRuntimeLease(path string) (*os.File, error) {
	name, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	handle, err := windows.CreateFile(name, windows.FILE_APPEND_DATA, windows.FILE_SHARE_READ, nil,
		windows.OPEN_ALWAYS, windows.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		if errors.Is(err, windows.ERROR_SHARING_VIOLATION) {
			err = errors.Join(ErrRuntimeLeaseBusy, err)
		}
		return nil, &os.PathError{Op: "lease", Path: path, Err: err}
	}
	return os.NewFile(uintptr(handle), path), nil
}
