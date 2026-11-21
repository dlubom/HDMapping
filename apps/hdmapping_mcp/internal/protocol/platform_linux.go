package protocol

import (
	"errors"
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

func socketPeer(fd int) (int, error) {
	cred, err := unix.GetsockoptUcred(fd, unix.SOL_SOCKET, unix.SO_PEERCRED)
	if err != nil {
		return 0, err
	}
	if int(cred.Uid) != os.Geteuid() {
		return 0, errors.New("coordinator peer UID mismatch")
	}
	return int(cred.Pid), nil
}

func peerExecutable(pid int) (string, error) {
	return fmt.Sprintf("/proc/%d/exe", pid), nil
}

func verifyExecutableAge(_ int, _ os.FileInfo) error { return nil }
