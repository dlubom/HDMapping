package protocol

import (
	"bytes"
	"errors"
	"os"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

func socketPeer(fd int) (int, error) {
	cred, err := unix.GetsockoptXucred(fd, unix.SOL_LOCAL, unix.LOCAL_PEERCRED)
	if err != nil {
		return 0, err
	}
	if int(cred.Uid) != os.Geteuid() {
		return 0, errors.New("coordinator peer UID mismatch")
	}
	return unix.GetsockoptInt(fd, unix.SOL_LOCAL, unix.LOCAL_PEERPID)
}

func peerExecutable(pid int) (string, error) {
	data, err := unix.SysctlRaw("kern.procargs2", pid)
	if errors.Is(err, unix.EINVAL) || errors.Is(err, unix.ESRCH) {
		return "", os.ErrNotExist
	}
	if err != nil {
		return "", err
	}
	// KERN_PROCARGS2 returns argc, then the saved executable path and argv.
	if len(data) > 4 {
		path, _, ok := bytes.Cut(data[4:], []byte{0})
		if ok && len(path) > 0 {
			return string(path), nil
		}
	}
	return "", errors.New("missing executable path for coordinator peer")
}

// Darwin reports a saved path, not an open executable vnode. A pathname
// whose metadata changed after the peer was launched needs a fresh process.
func verifyExecutableAge(pid int, info os.FileInfo) error {
	process, err := unix.SysctlKinfoProc("kern.proc.pid", pid)
	if errors.Is(err, unix.EINVAL) || errors.Is(err, unix.ESRCH) || errors.Is(err, unix.ENOMEM) {
		return os.ErrNotExist
	}
	if err != nil {
		return err
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return errors.New("missing coordinator executable metadata")
	}
	start := process.Proc.P_starttime
	changed := stat.Ctimespec
	if time.Unix(changed.Sec, changed.Nsec).After(time.Unix(start.Sec, int64(start.Usec)*1000)) {
		return errors.New("coordinator executable changed after process launch")
	}
	return nil
}
