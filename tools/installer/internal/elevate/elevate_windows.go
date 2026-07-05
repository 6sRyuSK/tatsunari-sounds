//go:build windows

package elevate

import "os/exec"

// runElevated re-launches the helper with `-Verb RunAs`, triggering a single
// UAC prompt. The elevated child runs hidden and waits; its exit code is
// propagated so a cancelled UAC surfaces as an error. Result is still read from
// result.json by the caller (the elevated child writes it). The command string
// (and its argument quoting) is assembled by the pure, unit-tested
// buildWindowsCommand.
func runElevated(self, planPath, resultPath string) error {
	command := buildWindowsCommand(self, planPath, resultPath)
	cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command)
	return cmd.Run()
}
