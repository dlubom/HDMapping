package protocol

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRejectsAnotherExecutableAtEndpoint(t *testing.T) {
	dir, err := ValidateDirectory(filepath.Join(t.TempDir(), "state"))
	if err != nil {
		t.Fatal(err)
	}
	runner, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	// Use a different regular file as the expected product executable. The
	// impostor listens with the correct user and uses a real kernel PID.
	expected := filepath.Join(t.TempDir(), "expected-coordinator")
	if err := os.WriteFile(expected, []byte("different binary"), 0600); err != nil {
		t.Fatal(err)
	}
	client, err := NewClient(dir, "1.2.3", expected)
	if err != nil {
		t.Fatal(err)
	}
	listener, err := listenLocal(client.endpoint)
	if err != nil {
		t.Fatal(err)
	}
	handled := make(chan struct{})
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	go func() {
		defer close(handled)
		conn, err := listener.Accept()
		if err == nil {
			defer func() { _ = conn.Close() }()
			_ = WriteFrame(conn, localHello("1.2.3"))
		}
	}()
	defer func() {
		_ = listener.Close()
		<-handled
	}()
	if err := client.Ensure(ctx); err == nil || !strings.Contains(err.Error(), "executable identity mismatch") {
		t.Fatalf("accepted coordinator peer %s at wrong endpoint: %v", runner, err)
	}
}
