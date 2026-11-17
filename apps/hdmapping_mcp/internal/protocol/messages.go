// Package protocol implements the private local coordinator wire.
package protocol

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"unicode/utf8"
)

const WireVersion = 1
const MaxFrameSize = 4 << 20

type Hello struct {
	Wire    int    `json:"wire"`
	Version string `json:"version"`
	PID     int    `json:"pid"`
}

func (h Hello) Verify(version string, pid int) error {
	if h.Wire != WireVersion || h.Version != version {
		return fmt.Errorf("incompatible process version: wire %d, product %q (expected %d, %q)", h.Wire, h.Version, WireVersion, version)
	}
	if h.PID <= 0 || h.PID != pid {
		return fmt.Errorf("process identity mismatch: received PID %d, expected %d", h.PID, pid)
	}
	return nil
}

type Request struct {
	Hello
	Method string          `json:"method"`
	Body   json.RawMessage `json:"body,omitempty"`
}

type Response struct {
	Hello
	Body  json.RawMessage `json:"body,omitempty"`
	Error string          `json:"error,omitempty"`
}

func ReadFrame(reader *bufio.Reader, frame any) error {
	var line []byte
	for {
		part, err := reader.ReadSlice('\n')
		if len(part)+len(line) > MaxFrameSize {
			return errors.New("IPC frame exceeds size limit")
		}
		if len(line) == 0 && err == nil {
			return decodeFrame(part, frame)
		}
		line = append(line, part...)
		switch {
		case err == nil:
			return decodeFrame(line, frame)
		case errors.Is(err, bufio.ErrBufferFull):
			continue
		case errors.Is(err, io.EOF) && len(line) != 0:
			return io.ErrUnexpectedEOF
		default:
			return err
		}
	}
}

func decodeFrame(line []byte, frame any) error {
	line = bytes.TrimSpace(line)
	if len(line) == 0 || line[0] != '{' || !utf8.Valid(line) {
		return errors.New("IPC frame requires a UTF-8 JSON object")
	}
	if err := json.Unmarshal(line, frame); err != nil {
		return fmt.Errorf("decode IPC frame: %w", err)
	}
	return nil
}

func WriteFrame(writer io.Writer, frame any) error {
	data, err := json.Marshal(frame)
	if err != nil {
		return fmt.Errorf("encode IPC frame: %w", err)
	}
	if len(data) >= MaxFrameSize {
		return errors.New("IPC frame exceeds size limit")
	}
	data = append(data, '\n')
	n, err := writer.Write(data)
	if err != nil {
		return fmt.Errorf("write IPC frame: %w", err)
	}
	if n != len(data) {
		return io.ErrShortWrite
	}
	return nil
}
