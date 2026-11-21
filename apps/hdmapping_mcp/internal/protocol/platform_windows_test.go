package protocol

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

func TestWindowsPrivateNamedPipe(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "stan żółć")
	if err := prepareDirectory(dir); err != nil {
		t.Fatal(err)
	}
	if err := prepareDirectory(dir); err != nil {
		t.Fatalf("reopen state directory: %v", err)
	}
	name, err := endpointName(dir)
	if err != nil {
		t.Fatal(err)
	}
	ln, err := listenLocal(name)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	if duplicate, err := listenLocal(name); err == nil {
		_ = duplicate.Close()
		t.Fatal("second listener acquired the same pipe")
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for range 2 {
		client, err := dialLocal(ctx, name)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = client.Close() })
		server, err := ln.Accept()
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = server.Close() })

		checkWindowsPeer(t, client, false)
		checkWindowsPeer(t, server, true)
		deadline, _ := ctx.Deadline()
		if err := client.SetDeadline(deadline); err != nil {
			t.Fatal(err)
		}
		if err := server.SetDeadline(deadline); err != nil {
			t.Fatal(err)
		}
		if _, err := io.WriteString(client, "abc"); err != nil {
			t.Fatal(err)
		}
		var payload [3]byte
		if _, err := io.ReadFull(server, payload[:]); err != nil || string(payload[:]) != "abc" {
			t.Fatalf("read from pipe = %q, %v", payload, err)
		}
		if err := client.SetReadDeadline(time.Now().Add(-time.Second)); err != nil {
			t.Fatal(err)
		}
		if _, err := client.Read(payload[:]); !errors.Is(err, os.ErrDeadlineExceeded) {
			t.Fatalf("read past deadline returned %v", err)
		}
		if err := client.Close(); err != nil {
			t.Fatal(err)
		}
		if err := server.Close(); err != nil {
			t.Fatal(err)
		}
	}
	// Closing both accepted connections leaves the endpoint still owned.
	if duplicate, err := listenLocal(name); err == nil {
		_ = duplicate.Close()
		t.Fatal("pipe ownership was lost after closing accepted connections")
	}
	pending := make(chan error, 1)
	go func() {
		conn, err := ln.Accept()
		if conn != nil {
			_ = conn.Close()
		}
		pending <- err
	}()
	if err := ln.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-pending:
		if !errors.Is(err, net.ErrClosed) {
			t.Fatalf("accept after close returned %v", err)
		}
	case <-ctx.Done():
		t.Fatal("listener close did not release Accept")
	}
}

func TestWindowsRejectsStateDirectoryWithOtherAccounts(t *testing.T) {
	sid, err := currentSID()
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "shared")
	name, err := statePathUTF16(path)
	if err != nil {
		t.Fatal(err)
	}
	sd, err := windows.SecurityDescriptorFromString(fmt.Sprintf("O:%[1]sD:P(A;OICI;FA;;;%[1]s)(A;OICI;FR;;;WD)", sid.String()))
	if err != nil {
		t.Fatal(err)
	}
	sa := windows.SecurityAttributes{Length: uint32(unsafe.Sizeof(windows.SecurityAttributes{})), SecurityDescriptor: sd}
	if err := windows.CreateDirectory(name, &sa); err != nil {
		t.Fatal(err)
	}
	if err := prepareDirectory(path); err == nil || !strings.Contains(err.Error(), "state directory ACL") {
		t.Fatalf("directory readable by other accounts returned %v", err)
	}
}

func TestWindowsNamedPipeDeniesAnonymousAccount(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "state")
	if err := prepareDirectory(dir); err != nil {
		t.Fatal(err)
	}
	name, err := endpointName(dir)
	if err != nil {
		t.Fatal(err)
	}
	ln, err := listenLocal(name)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	ctx, cancel := context.WithTimeout(t.Context(), 2*time.Second)
	defer cancel()
	impersonate := windows.NewLazySystemDLL("advapi32.dll").NewProc("ImpersonateAnonymousToken")
	runtime.LockOSThread()
	result, _, err := impersonate.Call(uintptr(windows.CurrentThread()))
	if result == 0 {
		runtime.UnlockOSThread()
		t.Fatalf("impersonate anonymous token: %v", err)
	}
	defer func() {
		if err := windows.RevertToSelf(); err != nil {
			// Leave the thread locked so Go retires it when this test exits.
			t.Fatalf("revert anonymous test thread: %v", err)
		}
		runtime.UnlockOSThread()
	}()
	conn, err := dialLocal(ctx, name)
	if conn != nil {
		_ = conn.Close()
	}
	if !errors.Is(err, windows.ERROR_ACCESS_DENIED) {
		t.Fatalf("anonymous named pipe connection returned %v", err)
	}
}

const (
	testWindowsJobMode = "HDMAPPING_TEST_WINDOWS_JOB_MODE"
	testWindowsJobName = "HDMAPPING_TEST_WINDOWS_JOB_NAME"
)

func TestWindowsCoordinatorJobPolicy(t *testing.T) {
	if mode := os.Getenv(testWindowsJobMode); mode != "" {
		checkWindowsJobChild(t, mode, os.Getenv(testWindowsJobName))
		return
	}
	for _, scenario := range []struct {
		name  string
		flags uint32
	}{
		{name: "forbidden"},
		{name: "explicit", flags: windows.JOB_OBJECT_LIMIT_BREAKAWAY_OK},
		{name: "silent", flags: windows.JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK},
	} {
		t.Run(scenario.name, func(t *testing.T) {
			name := fmt.Sprintf(`Local\HDMapping-test-job-%d-%s`, os.Getpid(), scenario.name)
			startTestWindowsJob(t, name, scenario.name, scenario.flags)
		})
	}
}

func startTestWindowsJob(t *testing.T, name, mode string, flags uint32) {
	t.Helper()
	nameUTF16, err := windows.UTF16PtrFromString(name)
	if err != nil {
		t.Fatal(err)
	}
	job, err := windows.CreateJobObject(nil, nameUTF16)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = windows.CloseHandle(job) })
	limits := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{}
	limits.BasicLimitInformation.LimitFlags = windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | flags
	var pin runtime.Pinner
	pin.Pin(&limits)
	_, err = windows.SetInformationJobObject(job, windows.JobObjectExtendedLimitInformation, uintptr(unsafe.Pointer(&limits)), uint32(unsafe.Sizeof(limits)))
	pin.Unpin()
	if err != nil {
		t.Fatal(err)
	}

	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, self, "-test.run=^TestWindowsCoordinatorJobPolicy$")
	cmd.Env = append(os.Environ(), testWindowsJobMode+"="+mode, testWindowsJobName+"="+name)
	var output strings.Builder
	cmd.Stdout, cmd.Stderr = &output, &output
	gate, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := cmd.Start(); err != nil {
		_ = gate.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_ = gate.Close()
		if cmd.ProcessState == nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
	})
	launcher, err := windows.OpenProcess(windows.PROCESS_SET_QUOTA|windows.PROCESS_TERMINATE, false, uint32(cmd.Process.Pid))
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = windows.CloseHandle(launcher) }()
	if err := windows.AssignProcessToJobObject(job, launcher); err != nil {
		t.Fatal(err)
	}
	if _, err := io.WriteString(gate, "g"); err != nil {
		t.Fatal(err)
	}
	if err := gate.Close(); err != nil {
		t.Fatal(err)
	}
	if err := cmd.Wait(); err != nil {
		t.Fatalf("Job Object launcher returned %v: %s", err, output.String())
	}
}

func checkWindowsJobChild(t *testing.T, mode, name string) {
	t.Helper()
	if mode == "detached" {
		nameUTF16, err := windows.UTF16PtrFromString(name)
		if err != nil {
			t.Fatal(err)
		}
		job, err := windows.CreateJobObject(nil, nameUTF16)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = windows.CloseHandle(job) }()
		member, err := isProcessInJob(windows.CurrentProcess(), job)
		if err != nil || member {
			t.Fatalf("detached process remains in launcher job: member=%t, err=%v", member, err)
		}
		return
	}
	var gate [1]byte
	if _, err := io.ReadFull(os.Stdin, gate[:]); err != nil {
		t.Fatal(err)
	}
	if err := VerifyDetached(); err == nil || !strings.Contains(err.Error(), "remains in a Windows Job Object") {
		t.Fatalf("confined coordinator returned %v", err)
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, self, "-test.run=^TestWindowsCoordinatorJobPolicy$")
	cmd.Env = append(os.Environ(), testWindowsJobMode+"=detached", testWindowsJobName+"="+name)
	if err := detachedCommand(cmd); mode == "forbidden" {
		if err == nil || !strings.Contains(err.Error(), "forbids coordinator breakaway") {
			t.Fatalf("non-breakaway launcher returned %v", err)
		}
		return
	} else if err != nil {
		t.Fatal(err)
	}
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("detached child returned %v: %s", err, output)
	}
}

func checkWindowsPeer(t *testing.T, c net.Conn, serverSide bool) {
	t.Helper()
	pid, err := peerPID(c, serverSide)
	if err != nil || pid != os.Getpid() {
		t.Fatalf("peerPID = %d, %v, want %d", pid, err, os.Getpid())
	}
	image, err := peerExecutable(pid)
	if err != nil {
		t.Fatal(err)
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	expected, err := os.Stat(self)
	if err != nil {
		t.Fatal(err)
	}
	actual, err := os.Stat(image)
	if err != nil || !os.SameFile(expected, actual) {
		t.Fatalf("peer image %q differs from executable %q: %v", image, self, err)
	}
}
