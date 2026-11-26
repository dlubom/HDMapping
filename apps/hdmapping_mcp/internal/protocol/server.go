package protocol

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"sync"
	"time"
)

const requestTimeout = 15 * time.Second

type Handler func(context.Context, string, json.RawMessage) (any, error)

func localHello(version string) Hello {
	return Hello{Wire: WireVersion, Version: version, PID: os.Getpid()}
}

// Serve blocks until the listener closes. The caller holds the journal lock.
func Serve(ctx context.Context, dir, version string, handler Handler) error {
	endpoint, err := endpointName(dir)
	if err != nil {
		return err
	}
	listener, err := listenLocal(endpoint)
	if err != nil {
		return fmt.Errorf("listen on coordinator endpoint: %w", err)
	}
	stop := context.AfterFunc(ctx, func() { _ = listener.Close() })
	defer stop()
	defer func() { _ = listener.Close() }()

	slots := make(chan struct{}, 32)
	var active sync.WaitGroup
	defer active.Wait()
	for {
		select {
		case <-ctx.Done():
			return nil
		case slots <- struct{}{}:
		}
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return fmt.Errorf("accept coordinator connection: %w", err)
		}
		active.Go(func() {
			defer func() { <-slots }()
			stop := context.AfterFunc(ctx, func() { _ = conn.Close() })
			defer stop()
			serveConnection(ctx, conn, version, handler)
		})
	}
}

func serveConnection(ctx context.Context, conn net.Conn, version string, handler Handler) {
	defer func() { _ = conn.Close() }()
	pid, err := peerPID(conn, true)
	if err != nil {
		return
	}
	if err := conn.SetDeadline(time.Now().Add(requestTimeout)); err != nil {
		return
	}
	hello := localHello(version)
	if err := WriteFrame(conn, hello); err != nil {
		return
	}
	var req Request
	if err := ReadFrame(bufio.NewReader(conn), &req); err != nil {
		return
	}
	resp := Response{Hello: hello}
	if err = req.Verify(version, pid); err == nil {
		var result any
		ctx, cancel := context.WithTimeout(ctx, requestTimeout)
		defer cancel()
		result, err = handler(ctx, req.Method, req.Body)
		if err == nil && result != nil {
			resp.Body, err = json.Marshal(result)
			if len(resp.Body) >= MaxFrameSize-1024 {
				err = errors.New("response too large; use a smaller page size")
			}
		}
	}
	if err != nil {
		resp.Error, resp.Body = err.Error(), nil
	}
	_ = WriteFrame(conn, resp)
}
