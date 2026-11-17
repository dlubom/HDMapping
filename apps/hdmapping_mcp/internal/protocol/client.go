package protocol

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

type Client struct {
	directory, version, coordinator, endpoint string
}

// RuntimeDirectory selects the per-user persistent registry.
func RuntimeDirectory() (string, error) {
	dir := os.Getenv("HDMAPPING_MCP_STATE_DIR")
	if dir == "" {
		base, err := os.UserConfigDir()
		if err != nil {
			return "", fmt.Errorf("locate user configuration directory: %w", err)
		}
		if err := os.MkdirAll(base, 0700); err != nil {
			return "", err
		}
		dir = filepath.Join(base, "HDMapping MCP")
	}
	return ValidateDirectory(dir)
}

func ValidateDirectory(dir string) (string, error) {
	if !filepath.IsAbs(dir) {
		return "", errors.New("MCP state directory must be absolute")
	}
	dir = filepath.Clean(dir)
	if err := prepareDirectory(dir); err != nil {
		return "", fmt.Errorf("prepare MCP state directory: %w", err)
	}
	resolved, err := filepath.EvalSymlinks(dir)
	if err != nil {
		return "", fmt.Errorf("resolve MCP state directory: %w", err)
	}
	return resolved, nil
}

// Companion resolves a product binary next to another product binary.
func Companion(executable, current, companion string) (string, error) {
	real, err := filepath.EvalSymlinks(executable)
	if err != nil {
		return "", fmt.Errorf("resolve product executable: %w", err)
	}
	ext := ""
	if runtime.GOOS == "windows" {
		ext = ".exe"
	}
	base := filepath.Base(real)
	if !strings.HasSuffix(base, current+ext) {
		return "", fmt.Errorf("unrecognized product executable name: %s", base)
	}
	prefix := strings.TrimSuffix(base, current+ext)
	path := filepath.Join(filepath.Dir(real), prefix+companion+ext)
	info, err := os.Stat(path)
	if err != nil {
		return "", fmt.Errorf("find product companion %s: %w", companion, err)
	}
	if !info.Mode().IsRegular() {
		return "", fmt.Errorf("product companion is not a regular file: %s", path)
	}
	return path, nil
}

func NewClient(dir, version, coordinator string) (*Client, error) {
	endpoint, err := endpointName(dir)
	if err != nil {
		return nil, fmt.Errorf("resolve coordinator endpoint: %w", err)
	}
	return &Client{directory: dir, version: version, coordinator: coordinator, endpoint: endpoint}, nil
}

// Ensure starts a coordinator on demand. The journal lock serializes launchers.
func (c *Client) Ensure(ctx context.Context) error {
	ctx, cancel := context.WithTimeout(ctx, requestTimeout)
	defer cancel()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	var exited <-chan error
	var launchErr error
	attempts := 0
	for {
		verified, err := c.call(ctx, "ping", nil, nil)
		if verified || err == nil {
			return err
		}
		if ctx.Err() != nil {
			return fmt.Errorf("coordinator unavailable: %w", errors.Join(ctx.Err(), launchErr, err))
		}
		if exited == nil && attempts < 4 {
			exited, err = c.start()
			if err != nil {
				return err
			}
			attempts++
		}
		select {
		case <-ctx.Done():
			return fmt.Errorf("coordinator unavailable: %w", errors.Join(ctx.Err(), launchErr, err))
		case launchErr = <-exited:
			exited = nil
			// Let an old coordinator finish releasing its journal lock.
			select {
			case <-ctx.Done():
			case <-ticker.C:
			}
		case <-ticker.C:
		}
	}
}

func (c *Client) start() (<-chan error, error) {
	cmd := exec.Command(c.coordinator, "serve", c.directory)
	logFile, err := os.OpenFile(filepath.Join(c.directory, "coordinator.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return nil, fmt.Errorf("open coordinator log: %w", err)
	}
	defer func() { _ = logFile.Close() }()
	cmd.Stderr = logFile
	if err := detachedCommand(cmd); err != nil {
		return nil, fmt.Errorf("configure detached coordinator: %w", err)
	}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start detached coordinator: %w", err)
	}
	// Reap a losing launcher even when the adapter remains open.
	exited := make(chan error, 1)
	go func() { exited <- cmd.Wait() }()
	return exited, nil
}

func (c *Client) Call(ctx context.Context, method string, input, output any) error {
	_, err := c.call(ctx, method, input, output)
	return err
}

func (c *Client) call(ctx context.Context, method string, input, output any) (bool, error) {
	ctx, cancel := context.WithTimeout(ctx, requestTimeout)
	defer cancel()
	conn, err := dialLocal(ctx, c.endpoint)
	if err != nil {
		return false, fmt.Errorf("dial coordinator: %w", err)
	}
	defer func() { _ = conn.Close() }()
	deadline, _ := ctx.Deadline()
	if err := conn.SetDeadline(deadline); err != nil {
		return true, err
	}
	stop := context.AfterFunc(ctx, func() { _ = conn.Close() })
	defer stop()

	pid, err := peerPID(conn, false)
	if err != nil {
		return !peerGone(err), fmt.Errorf("verify coordinator peer: %w", err)
	}
	if err := verifyExecutable(pid, c.coordinator); err != nil {
		return !peerGone(err) && !errors.Is(err, os.ErrNotExist), err
	}
	reader := bufio.NewReader(conn)
	var hello Hello
	if err := ReadFrame(reader, &hello); err != nil {
		return !disconnected(err), fmt.Errorf("read coordinator identity: %w", err)
	}
	if err := hello.Verify(c.version, pid); err != nil {
		return true, err
	}

	req := Request{Hello: localHello(c.version), Method: method}
	if input != nil {
		req.Body, err = json.Marshal(input)
		if err != nil {
			return true, fmt.Errorf("encode %s arguments: %w", method, err)
		}
	}
	if err := WriteFrame(conn, req); err != nil {
		return !disconnected(err), fmt.Errorf("send %s: %w", method, err)
	}
	var resp Response
	if err := ReadFrame(reader, &resp); err != nil {
		return !disconnected(err), fmt.Errorf("receive %s: %w", method, err)
	}
	if err := resp.Verify(c.version, pid); err != nil {
		return true, err
	}
	if resp.Error != "" {
		return true, fmt.Errorf("coordinator %s: %s", method, resp.Error)
	}
	if output != nil {
		if err := json.Unmarshal(resp.Body, output); err != nil {
			return true, fmt.Errorf("decode %s result: %w", method, err)
		}
	}
	return true, nil
}

func verifyExecutable(pid int, expected string) error {
	executable, err := peerExecutable(pid)
	if err != nil {
		return fmt.Errorf("inspect coordinator executable: %w", err)
	}
	peerInfo, err := os.Stat(executable)
	if err != nil {
		return fmt.Errorf("stat coordinator peer executable: %w", err)
	}
	expectedInfo, err := os.Stat(expected)
	if err != nil {
		return fmt.Errorf("stat expected coordinator executable: %w", err)
	}
	if !os.SameFile(peerInfo, expectedInfo) {
		return errors.New("coordinator peer executable identity mismatch")
	}
	if err := verifyExecutableAge(pid, peerInfo); err != nil {
		return fmt.Errorf("verify coordinator executable age: %w", err)
	}
	return nil
}

func disconnected(err error) bool {
	return errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) || peerGone(err)
}
