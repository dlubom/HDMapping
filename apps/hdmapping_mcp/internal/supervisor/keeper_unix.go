//go:build darwin || linux

package supervisor

import (
	"os/exec"
	"os/signal"
	"syscall"
)

// The keeper must reap its child even when a crashed parent closes stdout.
func ignoreBrokenOutput() { signal.Ignore(syscall.SIGPIPE) }

func configureChild(*exec.Cmd) {}
