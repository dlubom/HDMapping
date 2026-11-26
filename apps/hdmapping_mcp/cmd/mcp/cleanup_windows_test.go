package main

import (
	"fmt"

	"golang.org/x/sys/windows"
)

func stopTestCoordinator(pid int) error {
	handle, err := windows.OpenProcess(windows.SYNCHRONIZE, false, uint32(pid))
	if err != nil {
		return err
	}
	defer func() { _ = windows.CloseHandle(handle) }()
	if err := killCoordinator(pid); err != nil {
		return err
	}
	state, err := windows.WaitForSingleObject(handle, 2000)
	if err != nil {
		return err
	}
	if state != windows.WAIT_OBJECT_0 {
		return fmt.Errorf("test coordinator did not exit: wait returned %d", state)
	}
	return nil
}
