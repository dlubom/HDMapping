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
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

func currentSID() (*windows.SID, error) {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return nil, fmt.Errorf("read current user SID: %w", err)
	}
	return user.User.Sid, nil
}

func statePath(path string) (string, error) {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	absolute = strings.TrimPrefix(absolute, `\\?\`)
	if volume := filepath.VolumeName(absolute); len(volume) != 2 || volume[1] != ':' {
		return "", errors.New("coordinator state requires an absolute local drive path")
	}
	return absolute, nil
}

func statePathUTF16(path string) (*uint16, error) {
	return windows.UTF16PtrFromString(`\\?\` + path)
}

func prepareDirectory(path string) error {
	absolute, err := statePath(path)
	if err != nil {
		return err
	}
	for parent := filepath.Dir(absolute); ; parent = filepath.Dir(parent) {
		name, err := statePathUTF16(parent)
		if err != nil {
			return err
		}
		attributes, err := windows.GetFileAttributes(name)
		if err != nil {
			return fmt.Errorf("inspect state directory ancestor %q: %w", parent, err)
		}
		if attributes&windows.FILE_ATTRIBUTE_DIRECTORY == 0 || attributes&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 {
			return fmt.Errorf("state directory ancestor %q is not a real directory", parent)
		}
		if filepath.Dir(parent) == parent {
			break
		}
	}
	sid, err := currentSID()
	if err != nil {
		return err
	}
	// Set security as part of creation, so no other account can open the
	// directory before its ACL is installed. The ACE inherits to job files.
	sd, err := windows.SecurityDescriptorFromString(fmt.Sprintf("O:%sD:P(A;OICI;FA;;;%s)", sid.String(), sid.String()))
	if err != nil {
		return fmt.Errorf("build state directory ACL: %w", err)
	}
	name, err := statePathUTF16(absolute)
	if err != nil {
		return err
	}
	sa := windows.SecurityAttributes{Length: uint32(unsafe.Sizeof(windows.SecurityAttributes{})), SecurityDescriptor: sd}
	if err := windows.CreateDirectory(name, &sa); err != nil && !errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		return fmt.Errorf("create private state directory: %w", err)
	}
	handle, err := openStateDirectory(absolute, sid)
	if err != nil {
		return err
	}
	return windows.CloseHandle(handle)
}

func openStateDirectory(path string, sid *windows.SID) (windows.Handle, error) {
	name, err := statePathUTF16(path)
	if err != nil {
		return windows.InvalidHandle, err
	}
	handle, err := windows.CreateFile(name, windows.READ_CONTROL|windows.FILE_READ_ATTRIBUTES, windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE,
		nil, windows.OPEN_EXISTING, windows.FILE_FLAG_BACKUP_SEMANTICS|windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return windows.InvalidHandle, fmt.Errorf("open state directory: %w", err)
	}
	if err := validateStateDirectory(handle, sid); err != nil {
		_ = windows.CloseHandle(handle)
		return windows.InvalidHandle, err
	}
	return handle, nil
}

func validateStateDirectory(handle windows.Handle, sid *windows.SID) error {
	var info windows.ByHandleFileInformation
	if err := windows.GetFileInformationByHandle(handle, &info); err != nil {
		return fmt.Errorf("inspect state directory attributes: %w", err)
	}
	if info.FileAttributes&windows.FILE_ATTRIBUTE_DIRECTORY == 0 || info.FileAttributes&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 {
		return errors.New("coordinator state path is not a real directory")
	}
	sd, err := windows.GetSecurityInfo(handle, windows.SE_FILE_OBJECT, windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read state directory ACL: %w", err)
	}
	owner, _, err := sd.Owner()
	if err != nil {
		return fmt.Errorf("read state directory owner: %w", err)
	}
	if owner == nil || !owner.Equals(sid) {
		return errors.New("state directory is owned by another account")
	}
	control, _, err := sd.Control()
	if err != nil {
		return fmt.Errorf("read state directory ACL flags: %w", err)
	}
	acl, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("read state directory access list: %w", err)
	}
	if control&windows.SE_DACL_PROTECTED == 0 || acl == nil || acl.AceCount != 1 {
		return errors.New("state directory ACL must grant access only to the current account")
	}
	var ace *windows.ACCESS_ALLOWED_ACE
	if err := windows.GetAce(acl, 0, &ace); err != nil {
		return fmt.Errorf("read state directory ACL entry: %w", err)
	}
	const inheritFilesAndDirectories = 3 // OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE
	aceSID := (*windows.SID)(unsafe.Pointer(&ace.SidStart))
	if ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE || ace.Header.AceFlags != inheritFilesAndDirectories || !aceSID.Equals(sid) {
		return errors.New("state directory ACL must grant inheritable access only to the current account")
	}
	return nil
}

func endpointName(dir string) (string, error) {
	sid, err := currentSID()
	if err != nil {
		return "", err
	}
	path, err := statePath(dir)
	if err != nil {
		return "", err
	}
	handle, err := openStateDirectory(path, sid)
	if err != nil {
		return "", err
	}
	defer func() { _ = windows.CloseHandle(handle) }()
	// Use native directory identity so case and short-name aliases connect to
	// the same coordinator. The SID separates accounts using the same machine.
	var id struct {
		VolumeSerial uint64
		FileID       [16]byte
	}
	if err := windows.GetFileInformationByHandleEx(handle, windows.FileIdInfo, (*byte)(unsafe.Pointer(&id)), uint32(unsafe.Sizeof(id))); err != nil {
		return "", fmt.Errorf("read state directory identity: %w", err)
	}
	key := sha256.Sum256(fmt.Appendf(nil, "%s:%016x:%x", sid.String(), id.VolumeSerial, id.FileID))
	return fmt.Sprintf(`\\.\pipe\HDMapping-%x`, key[:16]), nil
}

type pipeAddress string

func (a pipeAddress) String() string  { return string(a) }
func (a pipeAddress) Network() string { return "named-pipe" }

type pipeConnection struct {
	*os.File
	address pipeAddress
}

func (c *pipeConnection) LocalAddr() net.Addr  { return c.address }
func (c *pipeConnection) RemoteAddr() net.Addr { return c.address }

type pipeListener struct {
	address pipeAddress
	name    *uint16
	sd      *windows.SECURITY_DESCRIPTOR
	// Accept is serialized; mu protects the pending handle from concurrent Close.
	acceptMu  sync.Mutex
	mu        sync.Mutex
	pending   windows.Handle
	accepting bool
	closed    bool
}

func listenLocal(endpoint string) (net.Listener, error) {
	name, err := windows.UTF16PtrFromString(endpoint)
	if err != nil {
		return nil, err
	}
	sid, err := currentSID()
	if err != nil {
		return nil, err
	}
	sd, err := windows.SecurityDescriptorFromString(fmt.Sprintf("O:%sD:P(A;;GA;;;%s)", sid.String(), sid.String()))
	if err != nil {
		return nil, fmt.Errorf("build named pipe ACL: %w", err)
	}
	l := &pipeListener{address: pipeAddress(endpoint), name: name, sd: sd}
	l.pending, err = l.newInstance(windows.FILE_FLAG_FIRST_PIPE_INSTANCE)
	if err != nil {
		return nil, err
	}
	return l, nil
}

func (l *pipeListener) newInstance(extraFlags uint32) (windows.Handle, error) {
	sa := windows.SecurityAttributes{Length: uint32(unsafe.Sizeof(windows.SecurityAttributes{})), SecurityDescriptor: l.sd}
	handle, err := windows.CreateNamedPipe(l.name, windows.PIPE_ACCESS_DUPLEX|windows.FILE_FLAG_OVERLAPPED|extraFlags,
		windows.PIPE_TYPE_BYTE|windows.PIPE_READMODE_BYTE|windows.PIPE_WAIT|windows.PIPE_REJECT_REMOTE_CLIENTS,
		windows.PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &sa)
	if err != nil {
		return windows.InvalidHandle, fmt.Errorf("create local named pipe: %w", err)
	}
	return handle, nil
}

func (l *pipeListener) Addr() net.Addr { return l.address }

func (l *pipeListener) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.closed {
		return net.ErrClosed
	}
	l.closed = true
	if !l.accepting {
		return windows.CloseHandle(l.pending)
	}
	// Accept owns the handle until the overlapped connect has finished.
	if err := windows.CancelIoEx(l.pending, nil); err != nil && !errors.Is(err, windows.ERROR_NOT_FOUND) {
		return fmt.Errorf("cancel pending pipe connection: %w", err)
	}
	return nil
}

func (l *pipeListener) Accept() (net.Conn, error) {
	l.acceptMu.Lock()
	defer l.acceptMu.Unlock()
	event, err := windows.CreateEvent(nil, 1, 0, nil)
	if err != nil {
		return nil, fmt.Errorf("create pipe accept event: %w", err)
	}
	defer func() { _ = windows.CloseHandle(event) }()
	for {
		handle, err := l.connect(event)
		if !errors.Is(err, windows.ERROR_NO_DATA) {
			if err != nil {
				return nil, err
			}
			return &pipeConnection{File: os.NewFile(uintptr(handle), l.address.String()), address: l.address}, nil
		}
		// A client closed its handle between CreateNamedPipe and ConnectNamedPipe.
	}
}

func (l *pipeListener) connect(event windows.Handle) (windows.Handle, error) {
	l.mu.Lock()
	if l.closed {
		l.mu.Unlock()
		return windows.InvalidHandle, net.ErrClosed
	}
	handle := l.pending
	l.accepting = true
	overlapped := windows.Overlapped{HEvent: event}
	err := windows.ConnectNamedPipe(handle, &overlapped)
	l.mu.Unlock()

	if errors.Is(err, windows.ERROR_IO_PENDING) {
		var transferred uint32
		err = windows.GetOverlappedResult(handle, &overlapped, &transferred, true)
	}
	if errors.Is(err, windows.ERROR_PIPE_CONNECTED) {
		err = nil
	}

	l.mu.Lock()
	defer l.mu.Unlock()
	l.accepting = false
	if l.closed {
		_ = windows.CloseHandle(handle)
		return windows.InvalidHandle, net.ErrClosed
	}
	// Maintain ownership of the pipe's name even when accepted connections
	// close immediately. Reserving the next instance first leaves no gap.
	pending, nextErr := l.newInstance(0)
	if nextErr != nil {
		l.closed = true
		_ = windows.CloseHandle(handle)
		return windows.InvalidHandle, nextErr
	}
	l.pending = pending
	if err != nil {
		_ = windows.CloseHandle(handle)
		return windows.InvalidHandle, fmt.Errorf("accept local named pipe: %w", err)
	}
	return handle, nil
}

func dialLocal(ctx context.Context, endpoint string) (net.Conn, error) {
	name, err := windows.UTF16PtrFromString(endpoint)
	if err != nil {
		return nil, err
	}
	for {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		handle, err := windows.CreateFile(name, windows.GENERIC_READ|windows.GENERIC_WRITE, 0, nil,
			windows.OPEN_EXISTING, windows.FILE_FLAG_OVERLAPPED|windows.SECURITY_SQOS_PRESENT|windows.SECURITY_IDENTIFICATION, 0)
		if err == nil {
			return &pipeConnection{File: os.NewFile(uintptr(handle), endpoint), address: pipeAddress(endpoint)}, nil
		}
		if !errors.Is(err, windows.ERROR_PIPE_BUSY) {
			return nil, &os.PathError{Op: "dial", Path: endpoint, Err: err}
		}
		// No instance is ready while the listener hands an accepted one over.
		timer := time.NewTimer(15 * time.Millisecond)
		select {
		case <-ctx.Done():
			timer.Stop()
			return nil, ctx.Err()
		case <-timer.C:
		}
	}
}

func peerPID(conn net.Conn, serverSide bool) (int, error) {
	pipe, ok := conn.(*pipeConnection)
	if !ok {
		return 0, errors.New("peer credentials require a local named pipe")
	}
	raw, err := pipe.SyscallConn()
	if err != nil {
		return 0, err
	}
	var pid uint32
	var credentialErr error
	if err := raw.Control(func(handle uintptr) {
		if serverSide {
			credentialErr = windows.GetNamedPipeClientProcessId(windows.Handle(handle), &pid)
		} else {
			credentialErr = windows.GetNamedPipeServerProcessId(windows.Handle(handle), &pid)
		}
	}); err != nil {
		return 0, err
	}
	if credentialErr != nil {
		return 0, fmt.Errorf("read named pipe peer PID: %w", credentialErr)
	}
	if pid == 0 {
		return 0, errors.New("named pipe did not identify a peer process")
	}
	process, err := openPeerProcess(pid)
	if err != nil {
		return 0, err
	}
	return int(pid), windows.CloseHandle(process)
}

func openPeerProcess(pid uint32) (windows.Handle, error) {
	process, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION|windows.SYNCHRONIZE, false, pid)
	if err != nil {
		return 0, fmt.Errorf("open named pipe peer process: %w", err)
	}
	var token windows.Token
	if err := windows.OpenProcessToken(process, windows.TOKEN_QUERY, &token); err != nil {
		err = peerAccessError(process, err)
		_ = windows.CloseHandle(process)
		return 0, fmt.Errorf("open named pipe peer token: %w", err)
	}
	defer func() { _ = token.Close() }()
	user, err := token.GetTokenUser()
	if err != nil {
		_ = windows.CloseHandle(process)
		return 0, fmt.Errorf("read named pipe peer SID: %w", err)
	}
	sid, err := currentSID()
	if err != nil {
		_ = windows.CloseHandle(process)
		return 0, err
	}
	if !user.User.Sid.Equals(sid) {
		_ = windows.CloseHandle(process)
		return 0, errors.New("named pipe peer is owned by another account")
	}
	return process, nil
}

func peerExecutable(pid int) (string, error) {
	if pid <= 0 || uint64(pid) > uint64(^uint32(0)) {
		return "", errors.New("invalid named pipe peer PID")
	}
	process, err := openPeerProcess(uint32(pid))
	if err != nil {
		return "", err
	}
	defer func() { _ = windows.CloseHandle(process) }()
	path := make([]uint16, 32768)
	size := uint32(len(path))
	if err := windows.QueryFullProcessImageName(process, 0, &path[0], &size); err != nil {
		return "", fmt.Errorf("read named pipe peer image: %w", peerAccessError(process, err))
	}
	return windows.UTF16ToString(path[:size]), nil
}

// Windows can deny a token or image query before TerminateProcess finishes.
// Keep the original denial for a process whose handle remains unsignalled.
func peerAccessError(process windows.Handle, err error) error {
	if errors.Is(err, windows.ERROR_ACCESS_DENIED) {
		state, waitErr := windows.WaitForSingleObject(process, 200)
		if waitErr == nil && state == windows.WAIT_OBJECT_0 {
			return errors.Join(os.ErrNotExist, err)
		}
	}
	return err
}

var isProcessInJobAPI = windows.NewLazySystemDLL("kernel32.dll").NewProc("IsProcessInJob")

func isProcessInJob(process, job windows.Handle) (bool, error) {
	var inJob uint32
	result, _, err := syscall.SyscallN(isProcessInJobAPI.Addr(), uintptr(process), uintptr(job), uintptr(unsafe.Pointer(&inJob)))
	if result == 0 {
		return false, fmt.Errorf("check process job membership: %w", err)
	}
	return inJob != 0, nil
}

// VerifyDetached rejects a coordinator that remains in an outer Windows Job
// Object after attempting to leave the launcher's immediate job.
func VerifyDetached() error {
	inJob, err := isProcessInJob(windows.CurrentProcess(), 0)
	if err != nil {
		return err
	}
	if inJob {
		return errors.New("coordinator remains in a Windows Job Object")
	}
	return nil
}

func detachedCommand(cmd *exec.Cmd) error {
	inJob, err := isProcessInJob(windows.CurrentProcess(), 0)
	if err != nil {
		return err
	}
	flags := uint32(windows.DETACHED_PROCESS | windows.CREATE_NEW_PROCESS_GROUP)
	if inJob {
		var limits windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION
		// The x/sys job API accepts an integer buffer address.
		var pin runtime.Pinner
		pin.Pin(&limits)
		err := windows.QueryInformationJobObject(0, windows.JobObjectExtendedLimitInformation, uintptr(unsafe.Pointer(&limits)), uint32(unsafe.Sizeof(limits)), nil)
		pin.Unpin()
		if err != nil {
			return fmt.Errorf("inspect launcher job-object policy: %w", err)
		}
		limitFlags := limits.BasicLimitInformation.LimitFlags
		switch {
		case limitFlags&windows.JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK != 0:
			// The child leaves the current job automatically.
		case limitFlags&windows.JOB_OBJECT_LIMIT_BREAKAWAY_OK != 0:
			flags |= windows.CREATE_BREAKAWAY_FROM_JOB
		default:
			return errors.New("launcher is in a Windows Job Object that forbids coordinator breakaway")
		}
	}
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	cmd.SysProcAttr.CreationFlags |= flags
	return nil
}

func peerGone(err error) bool {
	return errors.Is(err, windows.ERROR_INVALID_PARAMETER) || errors.Is(err, windows.ERROR_BROKEN_PIPE) ||
		errors.Is(err, windows.ERROR_NO_DATA) || errors.Is(err, windows.ERROR_PIPE_NOT_CONNECTED)
}

func verifyExecutableAge(_ int, _ os.FileInfo) error { return nil }
