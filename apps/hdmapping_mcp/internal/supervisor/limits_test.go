package supervisor

import (
	"strings"
	"testing"
)

func TestWorkerArgumentDepth(t *testing.T) {
	for _, depth := range []int{62, 63} {
		arg := strings.Repeat(`{"next":`, depth) + `"escaped\"{}[]"` + strings.Repeat("}", depth)
		if err := CheckArgumentDepth([]byte(arg)); (err == nil) != (depth == 62) {
			t.Fatalf("depth %v: %v", depth, err)
		}
	}
}
