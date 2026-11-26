package supervisor

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
)

// CheckArgumentDepth reserves the two object levels around scope/parameters
// in the C++ run packet. Its byte limit is 1 MiB; the job journal already
// caps a complete normalized request at 256 KiB.
func CheckArgumentDepth(arguments json.RawMessage) error {
	const maxArgumentDepth = 62
	decoder := json.NewDecoder(bytes.NewReader(arguments))
	decoder.UseNumber()
	depth := 0
	for {
		token, err := decoder.Token()
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return fmt.Errorf("read worker arguments: %w", err)
		}
		switch token {
		case json.Delim('{'), json.Delim('['):
			depth++
			if depth > maxArgumentDepth {
				return errors.New("worker arguments exceed JSON depth limit")
			}
		case json.Delim('}'), json.Delim(']'):
			depth--
		}
	}
}
