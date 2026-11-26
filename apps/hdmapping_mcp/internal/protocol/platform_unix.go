//go:build darwin || linux

package protocol

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"

	"golang.org/x/sys/unix"
)

func prepareDirectory(path string) error {
	if err := os.Mkdir(path, 0700); err != nil && !errors.Is(err, os.ErrExist) {
		return err
	}
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !info.IsDir() || info.Mode().Perm() != 0700 || !ok || int(stat.Uid) != os.Geteuid() {
		return fmt.Errorf("expected a private directory owned by the current user: %s", path)
	}
	return nil
}

func endpointName(dir string) (string, error) {
	// sockaddr_un is short on both platforms. Keep the endpoint in a
	// user-owned directory even when the journal path contains long names.
	runtimeDir := filepath.Join("/tmp", fmt.Sprintf("hdmapping-mcp-%d", os.Geteuid()))
	if err := prepareDirectory(runtimeDir); err != nil {
		return "", err
	}
	hash := sha256.Sum256([]byte(dir))
	return filepath.Join(runtimeDir, fmt.Sprintf("%x.sock", hash[:16])), nil
}

func listenLocal(endpoint string) (net.Listener, error) {
	info, err := os.Lstat(endpoint)
	if err == nil {
		if info.Mode()&os.ModeSocket == 0 {
			return nil, errors.New("coordinator endpoint is not a socket")
		}
		conn, dialErr := net.Dial("unix", endpoint)
		if dialErr == nil {
			_ = conn.Close()
			return nil, errors.New("coordinator endpoint already has a listener")
		}
		if !errors.Is(dialErr, unix.ECONNREFUSED) && !errors.Is(dialErr, os.ErrNotExist) {
			return nil, dialErr
		}
		if err := os.Remove(endpoint); err != nil && !errors.Is(err, os.ErrNotExist) {
			return nil, err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	return net.Listen("unix", endpoint)
}

func dialLocal(ctx context.Context, endpoint string) (net.Conn, error) {
	return (&net.Dialer{}).DialContext(ctx, "unix", endpoint)
}

func peerPID(conn net.Conn, _ bool) (int, error) {
	socket, ok := conn.(*net.UnixConn)
	if !ok {
		return 0, errors.New("expected Unix domain socket")
	}
	raw, err := socket.SyscallConn()
	if err != nil {
		return 0, err
	}
	var pid int
	var peerErr error
	if err := raw.Control(func(fd uintptr) {
		pid, peerErr = socketPeer(int(fd))
	}); err != nil {
		return 0, err
	}
	return pid, peerErr
}

func detachedCommand(cmd *exec.Cmd) error {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	return nil
}

// VerifyDetached has no extra host constraints after setsid succeeds.
func VerifyDetached() error { return nil }

func peerGone(err error) bool {
	return errors.Is(err, unix.ESRCH) || errors.Is(err, unix.ECONNRESET) || errors.Is(err, unix.EPIPE)
}
