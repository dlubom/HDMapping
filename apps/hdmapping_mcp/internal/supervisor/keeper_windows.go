package supervisor

import (
	"os/exec"
	"syscall"

	"golang.org/x/sys/windows"
)

func ignoreBrokenOutput() {}

func configureChild(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: windows.CREATE_NO_WINDOW}
}
