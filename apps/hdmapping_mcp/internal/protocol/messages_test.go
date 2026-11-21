package protocol

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"strings"
	"testing"
)

func TestFrames(t *testing.T) {
	var stream bytes.Buffer
	want := Hello{Wire: WireVersion, Version: "1.2.3", PID: 42}
	if err := WriteFrame(&stream, want); err != nil {
		t.Fatal(err)
	}
	input := bufio.NewReader(&stream)
	var got Hello
	if err := ReadFrame(input, &got); err != nil || got != want {
		t.Fatalf("hello=%+v err=%v", got, err)
	}
	if err := ReadFrame(input, &got); !errors.Is(err, io.EOF) {
		t.Fatalf("expected EOF, got %v", err)
	}
	for _, bad := range []string{"", "{}", "{} {}\n", "null\n", "{bad}\n", "{\"bad\":\"\xff\"}\n", strings.Repeat(" ", MaxFrameSize+1) + "\n"} {
		if err := ReadFrame(bufio.NewReader(strings.NewReader(bad)), &got); err == nil {
			t.Fatalf("accepted malformed frame %q", bad[:min(len(bad), 40)])
		}
	}
}

func FuzzReadFrame(f *testing.F) {
	for _, seed := range []string{"{}\n", "null\n", "{\"pid\":18446744073709551615}\n", "{} {}\n", "{}", "{\"version\":\"\xff\"}\n", "{\"pid\":42,\"version\":\"1.2.3\",\"wire\":1}\n"} {
		f.Add([]byte(seed))
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		var frame Hello
		_ = ReadFrame(bufio.NewReader(bytes.NewReader(data)), &frame)
	})
}
