package supervisor

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
)

const cancelGrace = 2 * time.Second
const startupTimeout = 5 * time.Second

type processResult struct {
	event event
	err   error
}

// exchange owns a child, including its pipes and reap. EOF requests cooperative
// cancellation and also reaches the keeper if its coordinator dies.
func exchange(ctx context.Context, cmd *exec.Cmd, version string, req command, onProgress func(event), keeper bool) (event, error) {
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return event{}, err
	}
	defer func() { _ = stdin.Close() }()
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return event{}, err
	}
	defer func() { _ = stdout.Close() }()
	configureChild(cmd)
	if err := cmd.Start(); err != nil {
		return event{}, fmt.Errorf("start %s: %w", cmd.Path, err)
	}

	ready := make(chan struct{})
	received := make(chan processResult, 1)
	go func() {
		reader := bufio.NewReader(stdout)
		var hello event
		if err := protocol.ReadFrame(reader, &hello); err != nil {
			received <- processResult{err: fmt.Errorf("read worker hello: %w", err)}
			return
		}
		if hello.Type != "hello" {
			received <- processResult{err: errors.New("missing worker hello")}
			return
		}
		if err := hello.Verify(version, cmd.Process.Pid); err != nil {
			received <- processResult{err: err}
			return
		}
		if err := protocol.WriteFrame(stdin, req); err != nil {
			received <- processResult{err: err}
			return
		}
		close(ready)
		for {
			var reply event
			if err := protocol.ReadFrame(reader, &reply); err != nil {
				received <- processResult{err: fmt.Errorf("read worker reply: %w", err)}
				return
			}
			if reply.Type != "progress" || reply.Progress == nil {
				received <- processResult{event: reply}
				return
			}
			if onProgress != nil {
				onProgress(reply)
			}
		}
	}()

	timer := time.NewTimer(startupTimeout)
	defer timer.Stop()
	var result processResult
	select {
	case <-ready:
		select {
		case result = <-received:
		case <-ctx.Done():
			result = stopProcess(stdin, cmd, received, keeper)
		}
	case result = <-received:
	case <-ctx.Done():
		result = stopProcess(stdin, cmd, received, keeper)
	case <-timer.C:
		// A keeper cannot launch its worker before the handshake is accepted
		// and it receives a complete command. Stop cooperatively in a tie.
		result = stopProcess(stdin, cmd, received, keeper)
		result.err = errors.Join(errors.New("worker handshake deadline exceeded"), result.err)
	}

	_ = stdin.Close()
	reaped := make(chan error, 1)
	go func() { reaped <- cmd.Wait() }()
	if keeper {
		// The keeper must reap the worker before releasing its lease.
		return result.event, errors.Join(result.err, <-reaped)
	}
	timer.Reset(cancelGrace)
	select {
	case waitErr := <-reaped:
		return result.event, errors.Join(result.err, waitErr)
	case <-timer.C:
		killErr := cmd.Process.Kill()
		return result.event, errors.Join(errors.New("worker exit deadline exceeded"), result.err, killErr, <-reaped)
	}
}

func stopProcess(stdin io.Closer, cmd *exec.Cmd, received <-chan processResult, keeper bool) processResult {
	_ = stdin.Close()
	if keeper {
		return <-received
	}
	timer := time.NewTimer(cancelGrace)
	defer timer.Stop()
	select {
	case result := <-received:
		return result
	case <-timer.C:
		killErr := cmd.Process.Kill()
		result := <-received
		result.err = errors.Join(errors.New("worker cancellation deadline exceeded"), killErr, result.err)
		return result
	}
}
